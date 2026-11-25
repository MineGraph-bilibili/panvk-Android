/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_pipe.h"
#include "gfx/si_gfx.h"
#include "ac_cmdbuf_cp.h"

void si_cp_release_acquire_mem_pws(struct si_context *sctx, struct radeon_cmdbuf *cs,
                                   unsigned event_type, unsigned gcr_cntl, unsigned stage_sel,
                                   unsigned sqtt_flush_flags)
{
   ac_emit_cp_release_mem_pws(&cs->current, sctx->gfx_level,
                              sctx->is_gfx_queue ? AMD_IP_GFX : AMD_IP_COMPUTE,
                              event_type, gcr_cntl);

   if (unlikely(sctx->sqtt_enabled))
      si_sqtt_describe_barrier_start(sctx, cs);

   ac_emit_cp_acquire_mem_pws(&cs->current, sctx->gfx_level,
                              sctx->is_gfx_queue ? AMD_IP_GFX : AMD_IP_COMPUTE,
                              event_type, stage_sel, 0, gcr_cntl);

   if (unlikely(sctx->sqtt_enabled))
      si_sqtt_describe_barrier_end(sctx, cs, sqtt_flush_flags);
}

void si_cp_acquire_mem(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                       enum amd_ip_type ip_type, unsigned gcr_cntl,
                       unsigned engine, unsigned *context_roll,
                       enum ac_rgp_flush_bits *flush_bits)
{
   if (gfx_level >= GFX10) {
      ac_emit_cp_acquire_mem(cs, gfx_level, ip_type, engine, gcr_cntl);
   } else {
      /* this seems problematic with gfx7 (see #4764) */
      if (gfx_level != GFX7)
         gcr_cntl |= 1u << 31; /* don't sync pfp, i.e. execute the sync in ME */

      ac_emit_cp_acquire_mem(cs, gfx_level, ip_type, engine, gcr_cntl);

      /* ACQUIRE_MEM & SURFACE_SYNC roll the context if the current context is busy. */
      if (ip_type == AMD_IP_GFX)
         *context_roll = true;

      if (engine == V_581A_PREFETCH_PARSER) {
         ac_emit_cp_pfp_sync_me(cs, false);

         *flush_bits |= AC_RGP_FLUSH_PFP_SYNC_ME;
      }
   }
}
