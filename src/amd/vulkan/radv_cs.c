/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based on si_state.c
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_cs.h"
#include "tools/radv_debug.h"
#include "tools/radv_sqtt.h"
#include "radv_buffer.h"
#include "radv_sdma.h"
#include "radv_shader.h"
#include "sid.h"

void
radv_cs_emit_write_event_eop(struct radv_cmd_stream *cs, enum amd_gfx_level gfx_level, unsigned event,
                             unsigned event_flags, unsigned dst_sel, unsigned int_sel, unsigned data_sel, uint64_t va,
                             uint32_t new_fence, uint64_t gfx9_eop_bug_va)
{
   assert(cs->hw_ip == AMD_IP_GFX || cs->hw_ip == AMD_IP_COMPUTE);

   /* The EOP bug is specific to GFX9. Though, RadeonSI also implements it for GFX6-8 but it
    * shouldn't be necessary because it's using SURFACE_SYNC to flush L2. See
    * waEventWriteEopPrematureL2Inv in PAL.
    */
   const uint64_t eop_bug_va = gfx_level >= GFX9 ? gfx9_eop_bug_va : va;

   ac_emit_cp_release_mem(cs->b, gfx_level, cs->hw_ip, event, event_flags, dst_sel, int_sel, data_sel, va, new_fence,
                          eop_bug_va);
}

static void
radv_cp_acquire_mem(struct radv_cmd_stream *cs, enum amd_gfx_level gfx_level, unsigned gcr_cntl, unsigned engine,
                    enum ac_rgp_flush_bits *rgp_flush_bits)
{
   if (gfx_level >= GFX10) {
      ac_emit_cp_acquire_mem(cs->b, gfx_level, cs->hw_ip, engine, gcr_cntl);
   } else {
      const bool is_mec = cs->hw_ip == AMD_IP_COMPUTE && gfx_level >= GFX7;

      /* This seems problematic with GFX7. */
      if (gfx_level != GFX7)
         gcr_cntl |= 1u << 31; /* don't sync PFP, i.e. execute the sync in ME */

      ac_emit_cp_acquire_mem(cs->b, gfx_level, cs->hw_ip, engine, gcr_cntl);

      if (engine == V_581A_PREFETCH_PARSER && !is_mec) {
         ac_emit_cp_pfp_sync_me(cs->b, false);

         *rgp_flush_bits |= AC_RGP_FLUSH_PFP_SYNC_ME;
      }
   }
}

static void
gfx10_cs_emit_cache_flush(struct radv_cmd_stream *cs, enum amd_gfx_level gfx_level, uint32_t *flush_cnt,
                          uint64_t flush_va, enum radv_cmd_flush_bits flush_bits,
                          enum radv_pws_acquire_point pws_acquire_point, enum ac_rgp_flush_bits *rgp_flush_bits)
{
   const bool is_mec = cs->hw_ip == AMD_IP_COMPUTE;
   uint32_t gcr_cntl = 0;
   unsigned cb_db_event = 0;

   /* We don't need these. */
   assert(!(flush_bits & (RADV_CMD_FLAG_VGT_STREAMOUT_SYNC)));

   if (flush_bits & RADV_CMD_FLAG_INV_ICACHE) {
      gcr_cntl |= S_587_GLI_INV(V_587_GLI_ALL);

      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_ICACHE;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_SCACHE) {
      gcr_cntl |= S_587_GLK_INV(1);

      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_SMEM_L0;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_VCACHE) {
      gcr_cntl |= S_587_GLV_INV(1);

      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_VMEM_L0;
   }
   if (flush_bits & (RADV_CMD_FLAG_INV_SCACHE | RADV_CMD_FLAG_INV_VCACHE) && gfx_level < GFX12) {
      gcr_cntl |= S_587_GL1_INV(1);

      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_L1;
   }

   if (flush_bits & RADV_CMD_FLAG_INV_L2) {
      /* Writeback and invalidate everything in L2. */
      gcr_cntl |= S_587_GL2_INV(1) | S_587_GL2_WB(1);

      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_L2;
   } else if (flush_bits & RADV_CMD_FLAG_WB_L2) {
      /* Writeback but do not invalidate.
       * GLM doesn't support WB alone. If WB is set, INV must be set too.
       */
      gcr_cntl |= S_587_GL2_WB(1);

      *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_L2;
   }

   if (gfx_level < GFX12 &&
       (flush_bits & (RADV_CMD_FLAG_INV_L2 | RADV_CMD_FLAG_WB_L2 | RADV_CMD_FLAG_INV_L2_METADATA))) {
      gcr_cntl |= S_587_GLM_INV(1) | S_587_GLM_WB(1);
   }

   if (flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB)) {
      if ((flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB && flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB) ||
          /* Gfx11 can't use the DB_META event and must use a full flush to flush DB_META. */
          (gfx_level == GFX11 && flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB)) {
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
      } else if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB) {
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
      } else {
         assert(flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB);
         cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
      }

      /* We must flush CMASK/FMASK/DCC separately if the main event only flushes CB_DATA. */
      if (gfx_level < GFX12 && cb_db_event == V_028A90_FLUSH_AND_INV_CB_DATA_TS) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_FLUSH_AND_INV_CB_META);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_CB | AC_RGP_FLUSH_INVAL_CB;
      }

      /* We must flush HTILE separately if the main event only flushes DB_DATA. */
      if (gfx_level < GFX12 && cb_db_event == V_028A90_FLUSH_AND_INV_DB_DATA_TS) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_FLUSH_AND_INV_DB_META);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_DB | AC_RGP_FLUSH_INVAL_DB;
      }

      /* First flush CB/DB, then L1/L2. */
      gcr_cntl |= S_587_SEQ(V_587_SEQ_FORWARD);

      if (gfx_level >= GFX11) {
         /* Send an event that flushes caches. */
         ac_emit_cp_release_mem_pws(cs->b, gfx_level, cs->hw_ip, cb_db_event, gcr_cntl & C_587_GLI_INV);

         /* The RELEASE_MEM above already flushed the data caches, so the ACQUIRE only has to
          * invalidate the I$ (GLI_INV, which RELEASE_MEM can't handle).
          */
         uint32_t acquire_gcr_cntl = gcr_cntl & ~C_587_GLI_INV; /* keep only GLI_INV */

         /* Select the ACQUIRE point (PWS stage). The data caches are already flushed, so ME is
          * enough unless a PFP_SYNC_ME is pending (then PFP); the barrier destination stage below
          * can defer the wait further to PRE_DEPTH.
          */
         uint32_t pws_stage = flush_bits & RADV_CMD_FLAG_PFP_SYNC_ME ? V_581B_CP_PFP : V_581B_CP_ME;

         if (!is_mec) {
            enum radv_pws_acquire_point acquire_point = pws_acquire_point;

            /* HW limitation: GCR cache ops during an ACQUIRE can only be performed at the PFP/ME
             * stage. If the ACQUIRE still needs to invalidate the I$ (GLI_INV), don't defer the
             * wait past ME.
             */
            if (acquire_point < RADV_PWS_ACQUIRE_POINT_ME && G_587_GLI_INV(acquire_gcr_cntl) != 0)
               acquire_point = RADV_PWS_ACQUIRE_POINT_ME;

            switch (acquire_point) {
            case RADV_PWS_ACQUIRE_POINT_PRE_DEPTH:
               pws_stage = V_581B_PRE_DEPTH;
               /* A PRE_DEPTH ACQUIRE can't carry GCR bits; GLI_INV is 0 here (see the clamp
                * above), so drop the remaining bits to make the ACQUIRE a pure wait.
                */
               acquire_gcr_cntl = 0;
               break;
            case RADV_PWS_ACQUIRE_POINT_ME:
               pws_stage = V_581B_CP_ME;
               break;
            default:
               break;
            }
         }

         /* Wait for the event and invalidate remaining caches if needed. */
         ac_emit_cp_acquire_mem_pws(cs->b, gfx_level, cs->hw_ip, cb_db_event, pws_stage, 0, acquire_gcr_cntl);

         gcr_cntl = 0; /* all done */

         /* A PFP ACQUIRE_MEM is implemented as an ACQUIRE at the ME plus a PFP_SYNC_ME, so it
          * already syncs the PFP. A deferred (ME/PRE_DEPTH) ACQUIRE does not, so only drop a
          * pending PFP_SYNC_ME when the ACQUIRE actually runs at the PFP.
          */
         if (pws_stage == V_581B_CP_PFP)
            flush_bits &= ~RADV_CMD_FLAG_PFP_SYNC_ME;
      } else {
         /* CB/DB flush and invalidate (or possibly just a wait for a
          * meta flush) via RELEASE_MEM.
          *
          * Combine this with other cache flushes when possible; this
          * requires affected shaders to be idle, so do it after the
          * CS_PARTIAL_FLUSH before (VS/PS partial flushes are always
          * implied).
          */
         /* Get GCR_CNTL fields, because the encoding is different in RELEASE_MEM. */
         unsigned glm_wb = G_587_GLM_WB(gcr_cntl);
         unsigned glm_inv = G_587_GLM_INV(gcr_cntl);
         unsigned glv_inv = G_587_GLV_INV(gcr_cntl);
         unsigned gl1_inv = G_587_GL1_INV(gcr_cntl);
         assert(G_587_GL2_US(gcr_cntl) == 0);
         assert(G_587_GL2_RANGE(gcr_cntl) == 0);
         assert(G_587_GL2_DISCARD(gcr_cntl) == 0);
         unsigned gl2_inv = G_587_GL2_INV(gcr_cntl);
         unsigned gl2_wb = G_587_GL2_WB(gcr_cntl);
         unsigned gcr_seq = G_587_SEQ(gcr_cntl);

         gcr_cntl &=
            C_587_GLM_WB & C_587_GLM_INV & C_587_GLV_INV & C_587_GL1_INV & C_587_GL2_INV & C_587_GL2_WB; /* keep SEQ */

         assert(flush_cnt);
         (*flush_cnt)++;

         ac_emit_cp_release_mem(
            cs->b, gfx_level, cs->hw_ip, cb_db_event,
            S_491_GLM_WB(glm_wb) | S_491_GLM_INV(glm_inv) | S_491_GLV_INV(glv_inv) | S_491_GL1_INV(gl1_inv) |
               S_491_GL2_INV(gl2_inv) | S_491_GL2_WB(gl2_wb) | S_491_SEQ(gcr_seq),
            EOP_DST_SEL_MEM, EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM, EOP_DATA_SEL_VALUE_32BIT, flush_va, *flush_cnt, 0);

         ac_emit_cp_wait_mem(cs->b, flush_va, *flush_cnt, 0xffffffff, WAIT_REG_MEM_EQUAL);
      }
   } else {
      /* The TS event above also makes sure that PS and CS are idle, so we have to do this only
       * if we are not flushing CB or DB.
       */

      /* Wait for graphics shaders to go idle if requested.
       *
       * On GFX10-11.7, PS_PARTIAL_FLUSH doesn't wait for GS waves that send "gs_alloc_req 0",
       * so we have to use VS_PARTIAL_FLUSH. (only tested Raphael and Navi33)
       */
      if (flush_bits & RADV_CMD_FLAG_VS_PARTIAL_FLUSH &&
          (gfx_level < GFX12 || !(flush_bits & RADV_CMD_FLAG_PS_PARTIAL_FLUSH))) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_VS_PARTIAL_FLUSH;
      }

      if (flush_bits & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_PS_PARTIAL_FLUSH);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_PS_PARTIAL_FLUSH;
      }

      if (flush_bits & RADV_CMD_FLAG_CS_PARTIAL_FLUSH) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_CS_PARTIAL_FLUSH);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_CS_PARTIAL_FLUSH;
      }
   }

   /* VGT state sync */
   if (flush_bits & RADV_CMD_FLAG_VGT_FLUSH) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_VGT_FLUSH);
      radeon_end();
   }

   /* Ignore fields that only modify the behavior of other fields. */
   if (gcr_cntl & C_587_GL2_RANGE & C_587_SEQ & (gfx_level >= GFX12 ? ~0 : C_587_GL1_RANGE)) {
      radv_cp_acquire_mem(cs, gfx_level, gcr_cntl,
                          flush_bits & RADV_CMD_FLAG_PFP_SYNC_ME ? V_581A_PREFETCH_PARSER : V_581A_MICRO_ENGINE,
                          rgp_flush_bits);
   } else if (flush_bits & RADV_CMD_FLAG_PFP_SYNC_ME && !is_mec) {
      /* We need to ensure that PFP waits as well. */
      ac_emit_cp_pfp_sync_me(cs->b, false);

      *rgp_flush_bits |= AC_RGP_FLUSH_PFP_SYNC_ME;
   }

   radeon_begin(cs);

   if (flush_bits & RADV_CMD_FLAG_START_PIPELINE_STATS) {
      if (!is_mec) {
         radeon_event_write(V_028A90_PIPELINESTAT_START);
      } else {
         radeon_set_sh_reg(R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(1));
      }
   } else if (flush_bits & RADV_CMD_FLAG_STOP_PIPELINE_STATS) {
      if (!is_mec) {
         radeon_event_write(V_028A90_PIPELINESTAT_STOP);
      } else {
         radeon_set_sh_reg(R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(0));
      }
   }

   radeon_end();
}

void
radv_cs_emit_cache_flush(struct radeon_winsys *ws, struct radv_cmd_stream *cs, enum amd_gfx_level gfx_level,
                         uint32_t *flush_cnt, uint64_t flush_va, enum radv_cmd_flush_bits flush_bits,
                         enum ac_rgp_flush_bits *rgp_flush_bits, enum radv_pws_acquire_point pws_acquire_point,
                         uint64_t gfx9_eop_bug_va)
{
   unsigned cp_coher_cntl = 0;
   uint32_t flush_cb_db = flush_bits & (RADV_CMD_FLAG_FLUSH_AND_INV_CB | RADV_CMD_FLAG_FLUSH_AND_INV_DB);

   radeon_check_space(ws, cs->b, 128);

   if (gfx_level >= GFX10) {
      /* GFX10 cache flush handling is quite different. */
      gfx10_cs_emit_cache_flush(cs, gfx_level, flush_cnt, flush_va, flush_bits, pws_acquire_point, rgp_flush_bits);
      return;
   }

   const bool is_mec = cs->hw_ip == AMD_IP_COMPUTE && gfx_level >= GFX7;

   if (flush_bits & RADV_CMD_FLAG_INV_ICACHE) {
      cp_coher_cntl |= S_0085F0_SH_ICACHE_ACTION_ENA(1);
      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_ICACHE;
   }
   if (flush_bits & RADV_CMD_FLAG_INV_SCACHE) {
      cp_coher_cntl |= S_0085F0_SH_KCACHE_ACTION_ENA(1);
      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_SMEM_L0;
   }

   if (gfx_level <= GFX8) {
      if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB) {
         cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1) | S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
                          S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
                          S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
                          S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1);

         /* Necessary for DCC */
         if (gfx_level >= GFX8) {
            ac_emit_cp_release_mem(cs->b, gfx_level, cs->hw_ip, V_028A90_FLUSH_AND_INV_CB_DATA_TS, 0, EOP_DST_SEL_MEM,
                                   EOP_INT_SEL_NONE, EOP_DATA_SEL_DISCARD, 0, 0, 0);
         }

         *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_CB | AC_RGP_FLUSH_INVAL_CB;
      }
      if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB) {
         cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1) | S_0085F0_DB_DEST_BASE_ENA(1);

         *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_DB | AC_RGP_FLUSH_INVAL_DB;
      }
   }

   if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_CB_META) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_FLUSH_AND_INV_CB_META);
      radeon_end();

      *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_CB | AC_RGP_FLUSH_INVAL_CB;
   }

   if (flush_bits & RADV_CMD_FLAG_FLUSH_AND_INV_DB_META) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_FLUSH_AND_INV_DB_META);
      radeon_end();

      *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_DB | AC_RGP_FLUSH_INVAL_DB;
   }

   /* Wait for shader engines to go idle.
    * VS and PS waits are unnecessary if SURFACE_SYNC is going to wait
    * for everything including CB/DB cache flushes.
    *
    * GFX6-8: SURFACE_SYNC with CB_ACTION_ENA doesn't do anything if there are no CB/DB bindings.
    * Reproducible with: piglit/arb_framebuffer_no_attachments-atomic
    *
    * GFX9: The TS event is always written after full pipeline completion regardless of CB/DB
    * bindings.
    */
   if (gfx_level <= GFX8 || !flush_cb_db) {
      if (flush_bits & RADV_CMD_FLAG_PS_PARTIAL_FLUSH) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_PS_PARTIAL_FLUSH);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_PS_PARTIAL_FLUSH;
      } else if (flush_bits & RADV_CMD_FLAG_VS_PARTIAL_FLUSH) {
         radeon_begin(cs);
         radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);
         radeon_end();

         *rgp_flush_bits |= AC_RGP_FLUSH_VS_PARTIAL_FLUSH;
      }
   }

   if (flush_bits & RADV_CMD_FLAG_CS_PARTIAL_FLUSH) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_CS_PARTIAL_FLUSH);
      radeon_end();

      *rgp_flush_bits |= AC_RGP_FLUSH_CS_PARTIAL_FLUSH;
   }

   if (gfx_level == GFX9 && flush_cb_db) {
      unsigned cb_db_event, tc_flags;

      /* Set the CB/DB flush event. */
      switch (flush_cb_db) {
      case RADV_CMD_FLAG_FLUSH_AND_INV_CB:
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
         break;
      case RADV_CMD_FLAG_FLUSH_AND_INV_DB:
         cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
         break;
      default:
         /* both CB & DB */
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
         break;
      }

      /* These are the only allowed combinations. If you need to
       * do multiple operations at once, do them separately.
       * All operations that invalidate L2 also seem to invalidate
       * metadata. Volatile (VOL) and WC flushes are not listed here.
       *
       * TC    | TC_WB         = writeback & invalidate L2
       * TC    | TC_WB | TC_NC = writeback & invalidate L2 for MTYPE == NC
       *         TC_WB | TC_NC = writeback L2 for MTYPE == NC
       * TC            | TC_NC = invalidate L2 for MTYPE == NC
       * TC    | TC_MD         = writeback & invalidate L2 metadata (DCC, etc.)
       * TCL1                  = invalidate L1
       */
      tc_flags = 0;

      if (flush_bits & RADV_CMD_FLAG_INV_L2_METADATA) {
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_MD_ACTION_ENA;
      }

      *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_CB | AC_RGP_FLUSH_INVAL_CB | AC_RGP_FLUSH_FLUSH_DB | AC_RGP_FLUSH_INVAL_DB;

      /* Ideally flush TC together with CB/DB. */
      if (flush_bits & RADV_CMD_FLAG_INV_L2) {
         /* Writeback and invalidate everything in L2. */
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_WB_ACTION_ENA;

         /* Clear the flags. */
         flush_bits &= ~(RADV_CMD_FLAG_INV_L2 | RADV_CMD_FLAG_WB_L2);

         *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_L2;
      }

      assert(flush_cnt);
      (*flush_cnt)++;

      ac_emit_cp_release_mem(cs->b, gfx_level, cs->hw_ip, cb_db_event, tc_flags, EOP_DST_SEL_MEM,
                             EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM, EOP_DATA_SEL_VALUE_32BIT, flush_va, *flush_cnt,
                             gfx9_eop_bug_va);
      ac_emit_cp_wait_mem(cs->b, flush_va, *flush_cnt, 0xffffffff, WAIT_REG_MEM_EQUAL);
   }

   /* VGT state sync */
   if (flush_bits & RADV_CMD_FLAG_VGT_FLUSH) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_VGT_FLUSH);
      radeon_end();
   }

   /* VGT streamout state sync */
   if (flush_bits & RADV_CMD_FLAG_VGT_STREAMOUT_SYNC) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_VGT_STREAMOUT_SYNC);
      radeon_end();
   }

   /* GFX6-GFX8 only: When one of the CP_COHER_CNTL.DEST_BASE flags is set, SURFACE_SYNC waits
    * for idle, so it should be last.
    *
    * cp_coher_cntl should contain everything except TC flags at this point.
    *
    * GFX6-GFX7 don't support L2 write-back.
    */
   const unsigned engine = flush_bits & RADV_CMD_FLAG_PFP_SYNC_ME ? V_581A_PREFETCH_PARSER : V_581A_MICRO_ENGINE;

   if ((flush_bits & RADV_CMD_FLAG_INV_L2) || (gfx_level <= GFX7 && (flush_bits & RADV_CMD_FLAG_WB_L2))) {
      /* Invalidate L1 & L2. WB must be set on GFX8+ when TC_ACTION is set. */
      radv_cp_acquire_mem(cs, gfx_level,
                          cp_coher_cntl | S_0085F0_TC_ACTION_ENA(1) | S_0085F0_TCL1_ACTION_ENA(1) |
                             S_0301F0_TC_WB_ACTION_ENA(gfx_level >= GFX8),
                          engine, rgp_flush_bits);

      *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_L2 | AC_RGP_FLUSH_INVAL_VMEM_L0;
   } else {
      /* L1 invalidation and L2 writeback must be done separately, because both operations can't
       * be done together.
       */
      if (flush_bits & RADV_CMD_FLAG_WB_L2) {
         /* WB = write-back
          * NC = apply to non-coherent MTYPEs
          *      (i.e. MTYPE <= 1, which is what we use everywhere)
          *
          * WB doesn't work without NC.
          *
          * If we get here, the only flag that can't be executed together with WB_L2 is VMEM cache
          * invalidation.
          */
         const bool last_acquire_mem = !(flush_bits & RADV_CMD_FLAG_INV_VCACHE);

         radv_cp_acquire_mem(
            cs, gfx_level,
            cp_coher_cntl | S_0301F0_TC_WB_ACTION_ENA(1) |
               S_0301F0_TC_NC_ACTION_ENA(1), /* If this is not the last ACQUIRE_MEM, flush in ME.
                                              * We only want to synchronize with PFP in the last ACQUIRE_MEM. */
            last_acquire_mem ? engine : V_581A_MICRO_ENGINE, rgp_flush_bits);

         if (last_acquire_mem)
            flush_bits &= ~RADV_CMD_FLAG_PFP_SYNC_ME;
         cp_coher_cntl = 0;

         *rgp_flush_bits |= AC_RGP_FLUSH_FLUSH_L2 | AC_RGP_FLUSH_INVAL_VMEM_L0;
      }

      if (flush_bits & RADV_CMD_FLAG_INV_VCACHE) {
         cp_coher_cntl |= S_0085F0_TCL1_ACTION_ENA(1);

         *rgp_flush_bits |= AC_RGP_FLUSH_INVAL_VMEM_L0;
      }

      /* If there are still some cache flags left. */
      if (cp_coher_cntl) {
         radv_cp_acquire_mem(cs, gfx_level, cp_coher_cntl, engine, rgp_flush_bits);
         flush_bits &= ~RADV_CMD_FLAG_PFP_SYNC_ME;
      }

      /* This might be needed even without any cache flags, such as when doing buffer stores
       * to an index buffer.
       */
      if (flush_bits & RADV_CMD_FLAG_PFP_SYNC_ME && !is_mec) {
         ac_emit_cp_pfp_sync_me(cs->b, false);

         *rgp_flush_bits |= AC_RGP_FLUSH_PFP_SYNC_ME;
      }
   }

   radeon_begin(cs);

   if (flush_bits & RADV_CMD_FLAG_START_PIPELINE_STATS) {
      if (!is_mec) {
         radeon_event_write(V_028A90_PIPELINESTAT_START);
      } else {
         radeon_set_sh_reg(R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(1));
      }
   } else if (flush_bits & RADV_CMD_FLAG_STOP_PIPELINE_STATS) {
      if (!is_mec) {
         radeon_event_write(V_028A90_PIPELINESTAT_STOP);
      } else {
         radeon_set_sh_reg(R_00B828_COMPUTE_PIPELINESTAT_ENABLE, S_00B828_PIPELINESTAT_ENABLE(0));
      }
   }

   radeon_end();
}

void
radv_init_cmd_stream(const struct radv_device *device, struct radv_cmd_stream *cs, const enum amd_ip_type ip_type)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   cs->buffered_sh_regs.num = 0;
   cs->hw_ip = ip_type;

   ac_init_tracked_regs(&cs->tracked_regs, &pdev->info, false);
}

VkResult
radv_create_cmd_stream(const struct radv_device *device, const enum amd_ip_type ip_type, const bool is_secondary,
                       struct radv_cmd_stream **cs_out)
{
   struct radeon_winsys *ws = device->ws;
   struct radv_cmd_stream *cs;

   cs = malloc(sizeof(*cs));
   if (!cs)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   radv_init_cmd_stream(device, cs, ip_type);

   cs->b = ws->cs_create(ws, ip_type, is_secondary);
   if (!cs->b) {
      free(cs);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   *cs_out = cs;
   return VK_SUCCESS;
}

void
radv_reset_cmd_stream(const struct radv_device *device, struct radv_cmd_stream *cs)
{
   struct radeon_winsys *ws = device->ws;

   radv_init_cmd_stream(device, cs, cs->hw_ip);

   ws->cs_reset(cs->b);
}

VkResult
radv_finalize_cmd_stream(const struct radv_device *device, struct radv_cmd_stream *cs)
{
   struct radeon_winsys *ws = device->ws;

   return ws->cs_finalize(cs->b);
}

void
radv_destroy_cmd_stream(const struct radv_device *device, struct radv_cmd_stream *cs)
{
   struct radeon_winsys *ws = device->ws;

   ws->cs_destroy(cs->b);
   free(cs);
}
