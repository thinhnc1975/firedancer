#include "utils/fd_ssctrl.h"
#include "utils/fd_snapshot_parser.h"
#include "utils/fd_snapshot_messages.h"

#include "../../disco/topo/fd_topo.h"
#include "../../disco/metrics/fd_metrics.h"
#include "../../flamenco/runtime/fd_acc_mgr.h"
#include "../../flamenco/types/fd_types.h"
#include "../../funk/fd_funk.h"
#include "../../ballet/lthash/fd_lthash.h"

#define NAME "snapin"

#define FD_SNAPIN_SCRATCH_MAX   (1UL<<20UL) /* 1MiB */
#define FD_SNAPIN_SCRATCH_DEPTH (1UL<<5UL ) /* 32 */

/* The snapin tile is a state machine that parses and loads a full
   and optionally an incremental snapshot.  It is currently responsible
   for loading accounts into an in-memory database, though this may
   change. */

#define FD_SNAPIN_STATE_LOADING   (0) /* We are inserting accounts from a snapshot */
#define FD_SNAPIN_STATE_MALFORMED (1) /* The snapshot is malformed, we are waiting for a reset notification */
#define FD_SNAPIN_STATE_SHUTDOWN  (2) /* The tile is done, been told to shut down, and has likely already exited */

struct fd_snapin_tile {
  fd_snapshot_parser_t * parser;

  int full;
  int state;

  fd_funk_t       funk[1];
  fd_funk_txn_t * funk_txn;
  uchar *         acc_data;

  /* Accounts lthash.  TODO: This is currently unused.  Need to add
     inline account hashing rather than relying on the lthash from the
     manifest. */
  fd_lthash_value_t lthash;

  /* A shared dcache object between snapin and replay that holds the
     decoded solana manifest.
     TODO: remove when replay can receive the snapshot manifest. */
  uchar * replay_manifest_dcache;
  ulong   replay_manifest_dcache_obj_id;

  /* TODO: remove when replay can receive the snapshot manifest. */
  ulong manifest_sz;

  struct {
    fd_snapshot_parser_metrics_t full;
    fd_snapshot_parser_metrics_t incremental;

    ulong num_accounts_inserted;
  } metrics;

  struct {
    fd_wksp_t * wksp;
    ulong       chunk0;
    ulong       wmark;
    ulong       mtu;
  } in;

  struct {
    fd_wksp_t * wksp;
    ulong       chunk0;
    ulong       wmark;
    ulong       chunk;
  } manifest_out;
};

typedef struct fd_snapin_tile fd_snapin_tile_t;

static inline int
should_shutdown( fd_snapin_tile_t * ctx ) {
  return ctx->state==FD_SNAPIN_STATE_SHUTDOWN;
}

static ulong
scratch_align( void ) {
  return alignof(fd_snapin_tile_t);
}

static ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(fd_snapin_tile_t),  sizeof(fd_snapin_tile_t)       );
  l = FD_LAYOUT_APPEND( l, fd_snapshot_parser_align(), fd_snapshot_parser_footprint() );
  l = FD_LAYOUT_APPEND( l, fd_scratch_smem_align(),    fd_scratch_smem_footprint( FD_SNAPIN_SCRATCH_MAX ) );
  l = FD_LAYOUT_APPEND( l, fd_scratch_fmem_align(),    fd_scratch_fmem_footprint( FD_SNAPIN_SCRATCH_DEPTH ) );
  return FD_LAYOUT_FINI( l, alignof(fd_snapin_tile_t) );
}

static void
metrics_write( fd_snapin_tile_t * ctx ) {
  FD_MGAUGE_SET( SNAPIN, FULL_ACCOUNTS_FILES_PROCESSED,        ctx->metrics.full.accounts_files_processed );
  FD_MGAUGE_SET( SNAPIN, FULL_ACCOUNTS_FILES_TOTAL,            ctx->metrics.full.accounts_files_total );
  FD_MGAUGE_SET( SNAPIN, FULL_ACCOUNTS_PROCESSED,              ctx->metrics.full.accounts_processed );
  FD_MGAUGE_SET( SNAPIN, INCREMENTAL_ACCOUNTS_FILES_PROCESSED, ctx->metrics.incremental.accounts_files_processed );
  FD_MGAUGE_SET( SNAPIN, INCREMENTAL_ACCOUNTS_FILES_TOTAL,     ctx->metrics.incremental.accounts_files_total );
  FD_MGAUGE_SET( SNAPIN, INCREMENTAL_ACCOUNTS_PROCESSED,       ctx->metrics.incremental.accounts_processed );
  FD_MGAUGE_SET( SNAPIN, ACCOUNTS_INSERTED,                    ctx->metrics.num_accounts_inserted );

  FD_MGAUGE_SET( SNAPIN, STATE, (ulong)ctx->state );
}

static void
save_manifest( fd_snapshot_parser_t *        parser,
               void *                        _ctx,
               fd_solana_manifest_global_t * manifest,
               ulong                         manifest_sz ) {
  (void)parser;
  fd_snapin_tile_t * ctx = _ctx;

  fd_snapshot_manifest_t * ssmanifest = fd_chunk_to_laddr( ctx->manifest_out.wksp, ctx->manifest_out.chunk );
  fd_snapshot_manifest_init_from_solana_manifest( ssmanifest, manifest );

  FD_LOG_NOTICE(( "Snapshot manifest loaded for slot %lu", ssmanifest->slot ));

  /* Send decoded manifest to replay */
  fd_memcpy( ctx->replay_manifest_dcache, manifest, manifest_sz );
  ctx->manifest_sz = manifest_sz;
}

static int
snapshot_is_duplicate_account( fd_snapshot_parser_t * parser,
                               fd_snapin_tile_t *     ctx,
                               fd_pubkey_t const *    account_key ) {
  /* Check if account exists */
  fd_account_meta_t const * rec_meta = fd_funk_get_acc_meta_readonly( ctx->funk,
                                                                      ctx->funk_txn,
                                                                      account_key,
                                                                      NULL,
                                                                      NULL,
                                                                      NULL );
  if( rec_meta ) {
    if( rec_meta->slot > parser->accv_slot ) {
      return 1;
    }

    /* TODO: Reaching here means this is a duplicate account.
       We need to hash the existing account and subtract that hash from
       the running lthash. */
  }
  return 0;
}

static void
snapshot_insert_account( fd_snapshot_parser_t *          parser,
                         fd_solana_account_hdr_t const * hdr,
                         void *                          _ctx ) {
  fd_snapin_tile_t * ctx = fd_type_pun( _ctx );
  fd_pubkey_t const * account_key  = fd_type_pun_const( hdr->meta.pubkey );

  if( !snapshot_is_duplicate_account( parser, ctx, account_key ) ) {
    FD_TXN_ACCOUNT_DECL( rec );
    int err = fd_txn_account_init_from_funk_mutable( rec,
                                                     account_key,
                                                     ctx->funk,
                                                     ctx->funk_txn,
                                                     /* do_create */ 1,
                                                     hdr->meta.data_len );
    if( FD_UNLIKELY( err != FD_ACC_MGR_SUCCESS ) ) {
      FD_LOG_ERR(( "fd_txn_account_init_from_funk_mutable failed (%d)", err ));
    }

    rec->vt->set_data_len( rec, hdr->meta.data_len );
    rec->vt->set_slot( rec, parser->accv_slot );
    rec->vt->set_hash( rec, &hdr->hash );
    rec->vt->set_info( rec, &hdr->info );

    ctx->acc_data = rec->vt->get_data_mut( rec );
    ctx->metrics.num_accounts_inserted++;
    fd_txn_account_mutable_fini( rec, ctx->funk, ctx->funk_txn );
  }
}

static void
snapshot_copy_acc_data( fd_snapshot_parser_t * parser FD_PARAM_UNUSED,
                        void *                 _ctx,
                        uchar const *          buf,
                        ulong                  data_sz ) {
  fd_snapin_tile_t * ctx = fd_type_pun( _ctx );

  if( ctx->acc_data ) {
    fd_memcpy( ctx->acc_data, buf, data_sz );
    ctx->acc_data += data_sz;
  }
}

static void
snapshot_reset_acc_data( fd_snapshot_parser_t * parser FD_PARAM_UNUSED,
                         void *                 _ctx ) {
  fd_snapin_tile_t * ctx = fd_type_pun( _ctx );
  ctx->acc_data = NULL;
}

static void
soft_reset_funk( fd_snapin_tile_t * ctx ) {
  if( ctx->funk_txn == NULL ) {
    fd_funk_txn_cancel_root( ctx->funk );
  } else {
    fd_funk_txn_cancel( ctx->funk, ctx->funk_txn, 0 );
  }

  /* TODO: Assert soft reset succeeded */
}

static void
hard_reset_funk( fd_snapin_tile_t * ctx ) {
  fd_funk_txn_cancel_root( ctx->funk );

  /* TODO: Assert that hard reset suceeded */
}

static void
send_manifest( fd_snapin_tile_t * ctx,
               fd_stem_context_t * stem ) {
  /* Assumes the manifest is already mem copied into the snap_out
     dcache and the replay_manifest_dcache from the save_manifest
     callback. */
  FD_TEST( ctx->manifest_sz );

  ulong sig          = ctx->full ? FD_FULL_SNAPSHOT_MANIFEST : FD_INCREMENTAL_SNAPSHOT_MANIFEST;
  ulong external_sig = ctx->full ? FD_FULL_SNAPSHOT_MANIFEST_EXTERNAL : FD_INCREMENTAL_SNAPSHOT_MANIFEST_EXTERNAL;

  /* Send snapshot manifest message over snap_out link */
  fd_stem_publish( stem, 0UL, sig, ctx->manifest_out.chunk, sizeof(fd_snapshot_manifest_t), 0UL, 0UL, 0UL );
  ctx->manifest_out.chunk = fd_dcache_compact_next( ctx->manifest_out.chunk,
                                                    sizeof(fd_snapshot_manifest_t),
                                                    ctx->manifest_out.chunk0,
                                                    ctx->manifest_out.wmark );

  /* send manifest over replay manifest dcache */
  ulong chunk = fd_dcache_compact_chunk0( fd_wksp_containing( ctx->replay_manifest_dcache ), ctx->replay_manifest_dcache );
  fd_stem_publish( stem, 0UL, external_sig, chunk, ctx->manifest_sz, 0UL, ctx->replay_manifest_dcache_obj_id, 0UL );
}

static void
transition_malformed( fd_snapin_tile_t * ctx,
                     fd_stem_context_t * stem ) {
  ctx->state = FD_SNAPIN_STATE_MALFORMED;
  fd_stem_publish( stem, 1UL, FD_SNAPSHOT_MSG_CTRL_MALFORMED, 0UL, 0UL, 0UL, 0UL, 0UL );
}

static void
handle_data_frag( fd_snapin_tile_t *  ctx,
                  ulong               chunk,
                  ulong               sz,
                  fd_stem_context_t * stem ) {
  if( FD_UNLIKELY( ctx->state==FD_SNAPIN_STATE_MALFORMED ) ) return;

  FD_TEST( ctx->state==FD_SNAPIN_STATE_LOADING );
  FD_TEST( chunk>=ctx->in.chunk0 && chunk<=ctx->in.wmark && sz<=ctx->in.mtu );

  if( FD_UNLIKELY( ctx->parser->flags & SNAP_FLAG_DONE ) ) {
    transition_malformed( ctx, stem );
    return;
  }

  uchar const * const chunk_start = fd_chunk_to_laddr_const( ctx->in.wksp, chunk );
  uchar const * const chunk_end = chunk_start + sz;
  uchar const *       cur       = chunk_start;

  for(;;) {
    if( FD_UNLIKELY( cur>=chunk_end ) ) {
      break;
    }

    cur = fd_snapshot_parser_process_chunk( ctx->parser,
                                            cur,
                                            (ulong)( chunk_end-cur ) );
    if( FD_UNLIKELY( ctx->parser->flags ) ) {
      if( FD_UNLIKELY( ctx->parser->flags & SNAP_FLAG_FAILED ) ) {
        transition_malformed( ctx, stem );
        return;
      }
    }
  }

  if( FD_LIKELY( ctx->full ) ) ctx->metrics.full = fd_snapshot_parser_get_metrics( ctx->parser );
  else                         ctx->metrics.incremental = fd_snapshot_parser_get_metrics( ctx->parser );
}


static void
handle_control_frag( fd_snapin_tile_t *  ctx,
                     fd_stem_context_t * stem,
                     ulong               sig ) {
  switch( sig ) {
    case FD_SNAPSHOT_MSG_CTRL_RESET_FULL:
      ctx->full = 1;
      fd_snapshot_parser_reset( ctx->parser );
      hard_reset_funk( ctx );
      break;
    case FD_SNAPSHOT_MSG_CTRL_RESET_INCREMENTAL:
      ctx->full = 0;
      fd_snapshot_parser_reset( ctx->parser );
      soft_reset_funk( ctx );
      break;
    case FD_SNAPSHOT_MSG_CTRL_EOF_FULL:
      FD_TEST( ctx->full );
      fd_snapshot_parser_reset( ctx->parser );

      /* Prepare a new funk txn to load the incremental snapshot */
      fd_funk_txn_xid_t incremental_xid = fd_funk_generate_xid();
      ctx->funk_txn = fd_funk_txn_prepare( ctx->funk, ctx->funk_txn, &incremental_xid, 0 );
      ctx->full = 0;
      break;
    case FD_SNAPSHOT_MSG_CTRL_DONE:
      /* Publish any outstanding funk txn. */
      if( FD_LIKELY( ctx->funk_txn ) ) fd_funk_txn_publish_into_parent( ctx->funk, ctx->funk_txn, 0 );

      /* Once the snapshot is fully loaded, we can send the manifest
         message over. */
      send_manifest( ctx, stem );

      /* Notify consumers of manifest out that the snapshot is fully
         loaded. */
      fd_stem_publish( stem, 0UL, FD_SNAPSHOT_DONE, 0UL, 0UL, 0UL, 0UL, 0UL );

      fd_snapshot_parser_close( ctx->parser );
      break;
    case FD_SNAPSHOT_MSG_CTRL_SHUTDOWN:
      ctx->state = FD_SNAPIN_STATE_SHUTDOWN;
      break;
    default:
      FD_LOG_ERR(( "unexpected control sig %lu", sig ));
      return;
  }

  /* We must acknowledge after handling the control frag, because if it
     causes us to generate a malformed transition, that must be sent
     back to the snaprd controller before the acknowledgement. */
  fd_stem_publish( stem, 1UL, FD_SNAPSHOT_MSG_CTRL_ACK, 0UL, 0UL, 0UL, 0UL, 0UL );
}

static inline int
returnable_frag( fd_snapin_tile_t *  ctx,
                 ulong               in_idx,
                 ulong               seq,
                 ulong               sig,
                 ulong               chunk,
                 ulong               sz,
                 ulong               tsorig,
                 ulong               tspub,
                 fd_stem_context_t * stem ) {
  (void)in_idx;
  (void)seq;
  (void)sig;
  (void)tsorig;
  (void)tspub;

  FD_TEST( ctx->state!=FD_SNAPIN_STATE_SHUTDOWN );

  if( FD_UNLIKELY( sig==FD_SNAPSHOT_MSG_DATA ) ) handle_data_frag( ctx, chunk, sz, stem );
  else                                           handle_control_frag( ctx, stem, sig  );

  return 0;
}

FD_FN_UNUSED static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_snapin_tile_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_snapin_tile_t),  sizeof(fd_snapin_tile_t)                             );
  void * parser_mem      = FD_SCRATCH_ALLOC_APPEND( l, fd_snapshot_parser_align(), fd_snapshot_parser_footprint()                       );
  void * smem            = FD_SCRATCH_ALLOC_APPEND( l, fd_scratch_smem_align(),    fd_scratch_smem_footprint( FD_SNAPIN_SCRATCH_MAX )   );
  void * fmem            = FD_SCRATCH_ALLOC_APPEND( l, fd_scratch_fmem_align(),    fd_scratch_fmem_footprint( FD_SNAPIN_SCRATCH_DEPTH ) );

  ctx->full = 1;
  ctx->state = FD_SNAPIN_STATE_LOADING;
  fd_lthash_zero( &ctx->lthash );

  fd_snapshot_parser_process_manifest_fn_t manifest_cb = NULL;
  if( 0==strcmp( topo->links[tile->out_link_id[ 0UL ]].name, "snap_out" ) ) {
    manifest_cb = save_manifest;
  }

  ctx->parser = fd_snapshot_parser_new( parser_mem,
                                        manifest_cb,
                                        snapshot_insert_account,
                                        snapshot_copy_acc_data,
                                        snapshot_reset_acc_data,
                                        ctx );
  FD_TEST( ctx->parser );

  fd_scratch_attach( smem, fmem, FD_SNAPIN_SCRATCH_MAX, FD_SNAPIN_SCRATCH_DEPTH );

  FD_TEST( fd_funk_join( ctx->funk, fd_topo_obj_laddr( topo, tile->snapin.funk_obj_id ) ) );
  ctx->funk_txn = fd_funk_txn_query( fd_funk_root( ctx->funk ), ctx->funk->txn_map );

  fd_memset( &ctx->metrics, 0, sizeof(ctx->metrics) );

  ctx->replay_manifest_dcache        = fd_topo_obj_laddr( topo, tile->snapin.manifest_dcache_obj_id );
  ctx->replay_manifest_dcache_obj_id = tile->snapin.manifest_dcache_obj_id;
  ctx->manifest_sz                   = 0UL;

  if( FD_UNLIKELY( tile->kind_id ) ) FD_LOG_ERR(( "There can only be one `" NAME "` tile" ));
  if( FD_UNLIKELY( tile->in_cnt!=1UL ) ) FD_LOG_ERR(( "tile `" NAME "` has %lu ins, expected 1",  tile->in_cnt  ));
  if( FD_UNLIKELY( tile->out_cnt!=2UL ) ) FD_LOG_ERR(( "tile `" NAME "` has %lu outs, expected 2",  tile->out_cnt  ));

  fd_topo_link_t * writer_link = &topo->links[ tile->out_link_id[ 0UL ] ];
  ctx->manifest_out.wksp    = topo->workspaces[ topo->objs[ writer_link->dcache_obj_id ].wksp_id ].wksp;
  ctx->manifest_out.chunk0  = fd_dcache_compact_chunk0( fd_wksp_containing( writer_link->dcache ), writer_link->dcache );
  ctx->manifest_out.wmark   = fd_dcache_compact_wmark ( ctx->manifest_out.wksp, writer_link->dcache, writer_link->mtu );
  ctx->manifest_out.chunk   = ctx->manifest_out.chunk0;

  fd_topo_link_t const * in_link = &topo->links[ tile->in_link_id[ 0UL ] ];
  fd_topo_wksp_t const * in_wksp = &topo->workspaces[ topo->objs[ in_link->dcache_obj_id ].wksp_id ];
  ctx->in.wksp                   = in_wksp->wksp;;
  ctx->in.chunk0                 = fd_dcache_compact_chunk0( ctx->in.wksp, in_link->dcache );
  ctx->in.wmark                  = fd_dcache_compact_wmark( ctx->in.wksp, in_link->dcache, in_link->mtu );
  ctx->in.mtu                    = in_link->mtu;
}

#define STEM_BURST 2UL /* For control fragments, one acknowledgement, and one malformed message */
#define STEM_LAZY  1000L

#define STEM_CALLBACK_CONTEXT_TYPE  fd_snapin_tile_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_snapin_tile_t)

#define STEM_CALLBACK_SHOULD_SHUTDOWN should_shutdown
#define STEM_CALLBACK_METRICS_WRITE   metrics_write
#define STEM_CALLBACK_RETURNABLE_FRAG returnable_frag

#include "../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_snapin = {
  .name              = NAME,
  .scratch_align     = scratch_align,
  .scratch_footprint = scratch_footprint,
  .unprivileged_init = unprivileged_init,
  .run               = stem_run,
};

#undef NAME
