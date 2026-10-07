/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_QUEUE_H
#define PANVK_QUEUE_H

#ifndef PAN_ARCH
#error "PAN_ARCH must be defined"
#endif

#include "genxml/gen_macros.h"

#include <stdint.h>

#include "panvk_device.h"

#include "vk_queue.h"

enum panvk_subqueue_id {
   PANVK_SUBQUEUE_VERTEX_TILER = 0,
   PANVK_SUBQUEUE_FRAGMENT,
   PANVK_SUBQUEUE_COMPUTE,
   PANVK_SUBQUEUE_COUNT,
};

struct panvk_tiler_heap {
   uint32_t chunk_size;
   struct panvk_priv_mem desc;
   struct panvk_priv_mem oom_fbd;
   struct {
      uint32_t handle;
      uint64_t dev_addr;
   } context;
#ifdef HAVE_PAN_KMOD_KBASE
   /* Per-generation TILER_HEAP descriptor slots carved out of the first 4K
    * of desc (64 slots x 64B).  Each heap generation owns one slot; the
    * per-subqueue contexts point at the current generation's slot, and
    * in-flight VERTEX_TILER jobs keep loading the older slot they were
    * emitted with -- so a renewal never rewrites memory an in-flight job
    * still walks.  The [4K, 68K) tail of desc stays the shared geom_buf. */
#define KBASE_HEAP_DESC_SLOT_SIZE 64
#define KBASE_HEAP_DESC_SLOTS 64
   uint32_t desc_slot_cur;
   uint32_t desc_slot_next;
#endif
};

struct panvk_subqueue {
   struct panvk_priv_mem context;
   uint32_t *reg_file;

   /* Memory to save/restore CS registers in functions/exception handlers.
    * Because registers are dumped to a fixed address rather than a moving
    * stack pointer, nested function/exception handler calls are not supported.
    */
   struct panvk_priv_mem regs_save;


   struct {
      /* Mask of resources requested by this subqueue. */
      uint32_t mask;
      /* Address and size of the linear buffer containing REQ_RESOURCE. */
      uint32_t cs_buffer_size;
      uint64_t cs_buffer_addr;
      /* Allocation */
      struct panvk_priv_mem buf;
   } req_resource;

   struct {
      struct pan_kmod_bo *bo;
      uint64_t size;
      struct {
         uint64_t dev;
         void *host;
      } addr;
   } tracebuf;

#ifdef HAVE_PAN_KMOD_KBASE
   /* kbase-backed queues manage the CS ring buffer in userspace (the
    * panthor kernel driver does the equivalent in kernel-owned rings). */
   struct {
      /* kbase compatibility path: all PanVK subqueues share one CSG, each
       * bound to its own CSI (CSI0..CSI2 = VT/FRAG/COMPUTE), matching the
       * panthor topology. */
      uint32_t group_handle;

      struct pan_kmod_bo *ringbuf_bo;
      void *ringbuf_cpu;
      uint64_t ringbuf_dev;

      /* USER_IO pages from KBASE_IOCTL_CS_QUEUE_BIND: doorbell page,
       * input page (CS_INSERT), output page (CS_EXTRACT/CS_ACTIVE). */
      void *user_io;

      /* Monotonically-increasing byte offset of the next ring entry. */
      uint64_t insert;

      /* Number of jobs emitted; each bumps the subqueue seqno cell by one
       * when it retires. */
      uint64_t emitted_jobs;

      uint32_t last_last_job_offset;
      uint32_t last_job_offset;
      uint32_t last_job_size;
      uint32_t last_job_entry_size;
      uint32_t last_stream_size;
      uint32_t last_flush_id;
      uint64_t last_stream_addr;
      /* DIAG-ONLY (panvk.17-diag): CPU mapping of the last stream submitted
       * on this subqueue.  The 16-diag ring census proved the 64KB ring holds
       * no sync atoms (entries are pure CALL trampolines); the fence/semaphore
       * signal atoms live in the stream reached through cs_call(), so the
       * fatal dump needs the stream's CPU mapping to census them. */
      const void *last_stream_cpu;

      struct panvk_priv_mem init_cs;
      uint64_t init_stream_addr;
      uint32_t init_stream_size;
      uint32_t init_flush_id;
      uint32_t init_pending;
   } kbase;
#endif
};

struct panvk_desc_ringbuf {
   struct panvk_priv_mem syncobj;
   struct pan_kmod_bo *bo;
   uint64_t size;
   struct {
      uint64_t dev;
      void *host;
   } addr;
};

struct panvk_gpu_queue {
   struct vk_queue vk;

   uint32_t group_handle;
   uint32_t syncobj_handle;

   struct panvk_tiler_heap tiler_heap;
   struct panvk_desc_ringbuf render_desc_ringbuf;
   struct panvk_priv_mem syncobjs;

#ifdef HAVE_PAN_KMOD_KBASE
   /* Per-subqueue completion seqno cells (panvk_cs_sync64 layout), written
    * with a plain LS store at the end of every ring entry and polled by the
    * CPU.  Must live in GPU-uncached memory: on inner-shareable pages the
    * sync write can linger in the GPU L2 where the CPU never sees it. */
   struct {
      struct pan_kmod_bo *bo;
      void *cpu;
      uint64_t dev;
   } kbase_seqnos;
   uint32_t kbase_tiler_submit_count;
   uint64_t kbase_tiler_work_count;
   /* DIAG-ONLY (panvk.15-diag): fires the immediate fatal-state dump exactly
    * once, as close to the faulting atomic as possible.  The 10s timeout
    * snapshot lands long after the fatal notification was consumed, and the
    * exception tears the CS down asynchronously -- the widest evidence is
    * right after the notification read returns. */
   bool kbase_fatal_dumped;
   /* Consecutive skipped renewals (retirement ring full or no free desc
    * slot).  While a renewal skips, the current heap generation keeps
    * growing through the kernel's grow-on-fault path; if the skips persist
    * the generation hits max_chunks and the kernel's unhandled-OOM
    * termination of the CSG is the panic trigger.  After
    * KBASE_RENEW_SKIP_DRAIN_THRESHOLD consecutive skips the next attempt
    * falls back to the old drain-per-renewal semantics, which always makes
    * progress.  Submit-thread only, no lock needed. */
   uint32_t kbase_renew_skip_count;
   /* Tiler heap generations retired by past heap renewals.  Renewal no
    * longer drains the graphics subqueues: it switches to a brand-new
    * kernel heap (fresh chunk pool) and parks the old context here until
    * both graphics subqueues have executed ring entries emitted after the
    * retirement (their in-flight HEAP_SETs still reference it).  Slots are
    * destroyed lazily, but only at graphics-silence points (both graphics
    * subqueues fully drained); a renewal that finds no free slot falls
    * back to the old graphics drain.  16 slots: the 32x renew-work cadence
    * rotates ~4x more often than the original 128x one, so the ring grew
    * 4x to match, keeping the full-ring skip rare under sustained tiler
    * load (2026-10-07). */
#define KBASE_RETIRED_HEAP_SLOTS 16
   struct {
      uint32_t count;
      struct {
         uint64_t ctx;
         uint64_t vt_jobs;
         uint64_t frag_jobs;
         uint32_t desc_slot;
      } slots[KBASE_RETIRED_HEAP_SLOTS];
   } kbase_retired_heaps;
   /* Guards kbase_retired_heaps: destroy attempts run from fence-wait /
    * status threads as well as the submit thread. */
   simple_mtx_t kbase_retired_heaps_lock;
#endif

   struct {
      struct vk_sync *sync;
      uint64_t next_value;
   } utrace;

   struct panvk_subqueue subqueues[PANVK_SUBQUEUE_COUNT];
};

VK_DEFINE_HANDLE_CASTS(panvk_gpu_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

VkResult panvk_per_arch(create_gpu_queue)(
   struct panvk_device *dev, const VkDeviceQueueCreateInfo *create_info,
   uint32_t queue_idx, struct vk_queue **out_queue);
void panvk_per_arch(destroy_gpu_queue)(struct vk_queue *vk_queue);
VkResult panvk_per_arch(gpu_queue_submit)(struct vk_queue *vk_queue,
                                          struct vk_queue_submit *vk_submit);
VkResult panvk_per_arch(gpu_queue_check_status)(struct vk_queue *vk_queue);

#endif
