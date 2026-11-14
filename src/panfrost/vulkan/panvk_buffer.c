/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_buffer.h"
#include "panvk_device.h"
#include "panvk_device_memory.h"
#include "panvk_entrypoints.h"

#include "pan_props.h"

#include "vk_log.h"

VKAPI_ATTR uint64_t VKAPI_CALL
panvk_GetBufferOpaqueCaptureAddress(VkDevice _device,
                                    const VkBufferDeviceAddressInfo *pInfo)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, pInfo->buffer);

   return buffer->vk.device_address;
}

static uint64_t
panvk_buffer_get_sparse_size(const struct panvk_buffer *buffer)
{
   struct panvk_device *device = to_panvk_device(buffer->vk.base.device);
   uint64_t buffer_size = buffer->vk.size;
   uint64_t page_size = panvk_get_gpu_page_size(device);
   return ALIGN_POT(buffer_size, page_size);
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetDeviceBufferMemoryRequirements(VkDevice _device,
                                        const VkDeviceBufferMemoryRequirements *pInfo,
                                        VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);

   /* For sparse resources alignment specifies binding granularity, rather than
    * the alignment requirement. It's up to us to satisfy the alignment
    * requirement when allocating the VA range.
    */
   const uint64_t align =
      pInfo->pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT
         ? panvk_get_gpu_page_size(device)
         : 64;
   const uint64_t size = align64(pInfo->pCreateInfo->size, align);

   pMemoryRequirements->memoryRequirements.memoryTypeBits =
      BITFIELD_MASK(phys_dev->memory.type_count);
   pMemoryRequirements->memoryRequirements.alignment = align;
   pMemoryRequirements->memoryRequirements.size = size;

   vk_foreach_struct_const(sType, ext, pMemoryRequirements->pNext) {
      switch (sType) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         dedicated->requiresDedicatedAllocation = false;
         dedicated->prefersDedicatedAllocation = dedicated->requiresDedicatedAllocation;
         break;
      }
      default:
         vk_debug_ignored_stype(sType);
         break;
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                        const VkBindBufferMemoryInfo *pBindInfos)
{
   VK_FROM_HANDLE(panvk_device, device, _device);

   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(panvk_buffer, buffer, pBindInfos[i].buffer);
      const VkBindMemoryStatus *bind_status =
         vk_find_struct_const(&pBindInfos[i], BIND_MEMORY_STATUS);

      assert(!(buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT));
      assert(buffer->vk.device_address == 0);
      assert(mem != NULL);

      if (bind_status)
         *bind_status->pResult = VK_SUCCESS;

      buffer->vk.device_address = mem->addr.dev + pBindInfos[i].memoryOffset;

      panvk_address_binding_report(device, &buffer->vk.base,
                                   buffer->vk.device_address, buffer->vk.size,
                                   VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);

      buffer->diag_bo = mem->bo;
      buffer->diag_mem_off = pBindInfos[i].memoryOffset;

      panvk_import_probe_track_buffer(pBindInfos[i].buffer, mem->bo);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_buffer *buffer;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

   buffer =
      vk_buffer_create(&device->vk, pCreateInfo, pAllocator, sizeof(*buffer));
   if (buffer == NULL)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (buffer->vk.size >= (32ull << 20))
      PANVK_DIAG_PRINTF("[PANVK-DIAG] CreateBuffer size=%llu MiB flags=0x%x usage=0x%x\n",
              (unsigned long long)(buffer->vk.size >> 20),
              (unsigned)buffer->vk.create_flags, (unsigned)buffer->vk.usage);

   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);
   if (buffer->vk.size > panvk_get_max_buffer_size(phys_dev)) {
      result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto err_destroy_buffer;
   }

   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      uint64_t va_range = panvk_buffer_get_sparse_size(buffer);

      /* kbase assigns GPU VAs at BO allocation time and has no native sparse
       * VM op, so the generic dummy-object emulation can never map the range
       * at the caller-chosen VA.  Back the whole range with a real zeroed BO
       * and use the kernel-assigned VA instead.  panvk does not implement
       * vkQueueBindSparse, so sparse buffers are only ever used unbound.
       */
      if (!pan_kmod_vm_supports_native_sparse(device->kmod.vm)) {
         /* A sparse buffer can be several GiB wide while the game only
          * touches a small part of it.  Ask kbase to grow the region on GPU
          * page faults (GROW_ON_GPF, 2 MiB steps) instead of pinning the
          * whole range up front; otherwise a handful of these buffers
          * exhausts system memory and the low-memory killer takes the app
          * down.  The BO is never CPU-mapped (NO_MMAP), so only the GPU can
          * commit pages.  Fall back to the fully committed BO if the kernel
          * rejects growable regions. */
         static uint64_t sparse_backing_total;
         bool full_backing =
            sparse_backing_total + va_range <= (3ull << 30);
         if (full_backing)
            sparse_backing_total += va_range;
         struct pan_kmod_bo *bo =
            pan_kmod_bo_alloc(device->kmod.dev, device->kmod.vm, va_range,
                              full_backing ? 0 :
                              PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT);
         if (bo == NULL) {
            PANVK_DIAG_PRINTF("[PANVK-DIAG] sparse buffer growable alloc "
                    "failed for %llu MiB, falling back to full commit\n",
                    (unsigned long long)(va_range >> 20));
            bo = pan_kmod_bo_alloc(device->kmod.dev, device->kmod.vm, va_range,
                                   PAN_KMOD_BO_FLAG_NO_MMAP);
         }
         int bind_ret = -1;

         if (bo != NULL) {
            struct pan_kmod_vm_op map = {
               .type = PAN_KMOD_VM_OP_TYPE_MAP,
               .va = {
                  .start = PAN_KMOD_VM_MAP_AUTO_VA,
                  .size = va_range,
               },
               .map = {
                  .bo = bo,
                  .bo_offset = 0,
               },
            };

            bind_ret = pan_kmod_vm_bind(device->kmod.vm,
                                        PAN_KMOD_VM_OP_MODE_IMMEDIATE, &map, 1);
            if (bind_ret == 0) {
               buffer->sparse_backing = bo;
               buffer->sparse_backing_full = full_backing;
               buffer->vk.device_address = map.va.start;
            } else {
               pan_kmod_bo_put(bo);
            }
         }

         PANVK_DIAG_PRINTF("[PANVK-DIAG] sparse buffer backing: size=%llu MiB "
                 "flags=0x%x backed=%d full=%d va=0x%llx ret=%d\n",
                 (unsigned long long)(va_range >> 20),
                 (unsigned)buffer->vk.create_flags,
                 buffer->sparse_backing != NULL,
                 buffer->sparse_backing != NULL &&
                 buffer->sparse_backing_full,
                 (unsigned long long)buffer->vk.device_address, bind_ret);

         if (buffer->sparse_backing != NULL)
            goto address_ready;
      }

      buffer->vk.device_address =
         panvk_as_alloc(device, &device->as.heap, va_range,
                        pan_choose_gpu_va_alignment(device->kmod.vm, va_range));
      if (!buffer->vk.device_address) {
         result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_destroy_buffer;
      }

      if ((buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT) ||
          PANVK_DEBUG(FORCE_BLACKHOLE)) {
         /* Map last so that we don't have a possibility of getting any more
          * errors, in which case we'd have to unmap.
          */
         struct pan_kmod_vm_op map = {
            .type = PAN_KMOD_VM_OP_TYPE_MAP,
            .va = {
               .start = buffer->vk.device_address,
               .size = va_range,
            },
            .flags = PAN_KMOD_VM_OP_OP_MAP_SPARSE,
         };

         int ret = pan_kmod_vm_bind(device->kmod.vm,
                                    PAN_KMOD_VM_OP_MODE_IMMEDIATE, &map, 1);
         if (ret) {
            result = panvk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            goto err_free_va;
         }
      }
   }

address_ready:

   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
      panvk_address_binding_report(device, &buffer->vk.base,
                                   buffer->vk.device_address,
                                   panvk_buffer_get_sparse_size(buffer),
                                   VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT);

   *pBuffer = panvk_buffer_to_handle(buffer);

   return VK_SUCCESS;

err_free_va:
   if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
      panvk_as_free(device, &device->as.heap, buffer->vk.device_address,
                    panvk_buffer_get_sparse_size(buffer));

err_destroy_buffer:
   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyBuffer(VkDevice _device, VkBuffer _buffer,
                    const VkAllocationCallbacks *pAllocator)
{
   panvk_sparse_mirror_forget(_buffer, VK_NULL_HANDLE);

   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_buffer, buffer, _buffer);

   if (!buffer)
      return;

   if (buffer->sparse_backing != NULL) {
      pan_kmod_bo_put(buffer->sparse_backing);
   } else if (buffer->vk.create_flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
      uint64_t va_range = panvk_buffer_get_sparse_size(buffer);

      panvk_address_binding_report(device, &buffer->vk.base,
                                   buffer->vk.device_address, va_range,
                                   VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);

      struct pan_kmod_vm_op unmap = {
         .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
         .va = {
            .start = buffer->vk.device_address,
            .size = va_range,
         },
      };
      ASSERTED int ret = pan_kmod_vm_bind(
         device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &unmap, 1);
      assert(!ret);

      panvk_as_free(device, &device->as.heap, buffer->vk.device_address,
                    va_range);
   } else if (buffer->vk.device_address) {
      panvk_address_binding_report(device, &buffer->vk.base,
                                   buffer->vk.device_address, buffer->vk.size,
                                   VK_DEVICE_ADDRESS_BINDING_TYPE_UNBIND_EXT);
   }

   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}
