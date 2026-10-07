/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_cmd_buffer.h"
#include "panvk_entrypoints.h"
#include "panvk_event.h"
#include "panvk_instr.h"

#include "util/bitscan.h"

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdResetEvent2)(VkCommandBuffer commandBuffer, VkEvent _event,
                               VkPipelineStageFlags2 stageMask)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_event, event, _event);

   /* Wrap stageMask with a VkDependencyInfo object so we can re-use
    * add_cs_deps(). */
   const VkMemoryBarrier2 barrier = {
      .srcStageMask = stageMask,
   };
   const VkDependencyInfo info = {
      .memoryBarrierCount = 1,
      .pMemoryBarriers = &barrier,
   };
   struct panvk_cs_deps deps = {0};

   panvk_per_arch(add_cs_deps)(cmdbuf, &info, &deps, false);

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);
      uint32_t sb_mask = deps.src[i].wait_sb_mask;
      struct cs_index sync_addr = cs_scratch_reg64(b, 0);
      struct cs_index seqno = cs_scratch_reg32(b, 2);

      /* Do NOT load the current seqno to branch on it: LOAD is a generic
       * LSU access and hangs silently on CSF_EVENT (GPU-uncached) memory
       * on MT6985 r38p1 -- the stream stops right after the LOAD and the
       * subqueue never completes (kbase timeout, DEVICE_LOST). The store
       * below goes through the dedicated microcode sync path which works
       * (partial seqnos observed written while the stream was stuck on
       * the LOAD). Resetting unconditionally is semantically equivalent:
       * writing 0 twice is idempotent. */
      cs_move64_to(b, sync_addr,
                   panvk_priv_mem_dev_addr(event->syncobjs) +
                      (i * sizeof(struct panvk_cs_sync32)));
      cs_move32_to(b, seqno, 0);
      cs_sync32_set(b, false, MALI_CS_SYNC_SCOPE_CSG, seqno, sync_addr,
                    cs_defer(sb_mask | SB_MASK(DEFERRED_FLUSH),
                             SB_ID(DEFERRED_SYNC)));
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdSetEvent2)(VkCommandBuffer commandBuffer, VkEvent _event,
                             const VkDependencyInfo *pDependencyInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   VK_FROM_HANDLE(panvk_event, event, _event);
   struct panvk_cs_deps deps = {0};

   panvk_per_arch(add_cs_deps)(cmdbuf, pDependencyInfo, &deps, true);

   /* vkCmdSetEvents() is not allowed to be called mid-render-pass */
   assert(!deps.needs_fb_barrier);

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);
      uint32_t sb_mask = deps.src[i].wait_sb_mask;
      struct cs_index sync_addr = cs_scratch_reg64(b, 0);
      struct cs_index seqno = cs_scratch_reg32(b, 2);
      struct panvk_cache_flush_info cache_flush = deps.src[i].cache_flush;

      /* Same as CmdResetEvent2: no generic-LSU LOAD on CSF_EVENT memory
       * (silent hang on MT6985 r38p1). Flush caches and set the event
       * unconditionally; both are idempotent when the event is already
       * set. */
      cs_move64_to(b, sync_addr,
                   panvk_priv_mem_dev_addr(event->syncobjs) +
                      (i * sizeof(struct panvk_cs_sync32)));

      if (!panvk_cache_flush_is_nop(&cache_flush)) {
         cs_flush_caches(b, cache_flush.l2, cache_flush.lsc,
                         cache_flush.others, seqno,
                         cs_defer(sb_mask, SB_ID(DEFERRED_FLUSH)));
      }

      cs_move32_to(b, seqno, 1);
      cs_sync32_set(b, false, MALI_CS_SYNC_SCOPE_CSG, seqno, sync_addr,
                    cs_defer(sb_mask | SB_MASK(DEFERRED_FLUSH),
                             SB_ID(DEFERRED_SYNC)));
   }
}

static void
cmd_wait_event(struct panvk_cmd_buffer *cmdbuf, struct panvk_event *event,
               const VkDependencyInfo *info)
{
   struct panvk_cs_deps deps = {0};

   panvk_per_arch(add_cs_deps)(cmdbuf, info, &deps, false);

   for (uint32_t i = 0; i < PANVK_SUBQUEUE_COUNT; i++) {
      struct cs_builder *b = panvk_get_cs_builder(cmdbuf, i);

      u_foreach_bit(j, deps.dst[i].wait_subqueue_mask) {
         struct cs_index sync_addr = cs_scratch_reg64(b, 0);
         struct cs_index seqno = cs_scratch_reg32(b, 2);

         cs_move64_to(b, sync_addr,
                      panvk_priv_mem_dev_addr(event->syncobjs) +
                         (j * sizeof(struct panvk_cs_sync32)));

         cs_move32_to(b, seqno, 0);
         panvk_instr_sync32_wait(cmdbuf, i, false, cmdbuf->sync_scope,
                                 MALI_CS_CONDITION_GREATER, seqno, sync_addr);
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdWaitEvents2)(VkCommandBuffer commandBuffer,
                               uint32_t eventCount, const VkEvent *pEvents,
                               const VkDependencyInfo *pDependencyInfos)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   for (uint32_t i = 0; i < eventCount; i++) {
      VK_FROM_HANDLE(panvk_event, event, pEvents[i]);
      const VkDependencyInfo *info = &pDependencyInfos[i];

      cmd_wait_event(cmdbuf, event, info);
   }
}
