/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_DEVICE_MEMORY_H
#define PANVK_DEVICE_MEMORY_H

#include <stdint.h>

#include "vk_device_memory.h"

struct panvk_priv_bo;

struct panvk_device_memory {
   struct vk_device_memory vk;
   struct pan_kmod_bo *bo;
   struct {
      uint64_t dev;
      void *host;
   } addr;

   struct {
      /* Don't use this pointer, it's only to have user memory dumped when
       * PANVK_DEBUG=dump. */
      void *host_mapping;
   } debug;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(panvk_device_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)

void panvk_import_probe_track_image(VkImage img, void *bo);
void panvk_import_probe_note_bind(VkImage img, void *bo);
void panvk_import_probe_set_blit_src(VkImage src, VkImage dst);
void panvk_import_probe_set_render_target(VkImage img);
void panvk_import_probe_track_buffer(VkBuffer buf, void *bo);
bool panvk_import_probe_is_image(VkImage img);
bool panvk_import_probe_is_buffer(VkBuffer buf);

#endif
