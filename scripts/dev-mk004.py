#!/usr/bin/env python3
"""Assemble patch 004 from header + full .h diff + filtered .c hunks."""
import subprocess

M = 'work/mesa'
B = 'work/ref-g720-beta'
def udiff(a, b):
    p = subprocess.run(['diff', '-U3', a, b], capture_output=True, text=True)
    assert p.returncode in (0, 1), p.stderr
    return p.stdout.splitlines(keepends=True)

h = udiff(f'{M}/src/panfrost/vulkan/panvk_physical_device.h',
          f'{B}/src/panfrost/vulkan/panvk_physical_device.h')
# rewrite labels to a/ b/
h = [('--- a/src/panfrost/vulkan/panvk_physical_device.h\n'
      if l.startswith('--- ') else
      ('+++ b/src/panfrost/vulkan/panvk_physical_device.h\n'
       if l.startswith('+++ ') else l)) for l in h]
c = open('/tmp/pdev-kbase.diff').read()
# Adaptation 1: beta calls get_gpu_model(), which upstream pinned removed
# (model is resolved inline in panvk_physical_device_init). Resolve inline
# here too, strict like the DRM path (no fallback model).
old_call = """+   unsigned arch = pan_arch(device->kmod.dev->props.gpu_id);
+
+   bool unknown_gpu;
+   result = get_gpu_model(device, instance, &unknown_gpu);
+   if (result != VK_SUCCESS)
+      goto fail_kbase;
+"""
new_call = """+   device->model = pan_get_model(device->kmod.dev->props.gpu_id,
+                                 device->kmod.dev->props.gpu_variant);
+
+   unsigned arch = pan_arch(device->kmod.dev->props.gpu_id);
+
+   if (!device->model) {
+      result = panvk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
+                            "Unknown gpu_id (%#" PRIx64 ") or variant (%#x)",
+                            device->kmod.dev->props.gpu_id,
+                            device->kmod.dev->props.gpu_variant);
+      goto fail_kbase;
+   }
+"""
assert old_call in c, 'get_gpu_model hunk not found'
c = c.replace(old_call, new_call)
old_name = """+   memset(device->name, 0, sizeof(device->name));
+   if (unknown_gpu)
+      snprintf(device->name, sizeof(device->name),
+               "Mali unknown 0x%" PRIx64 " MC%u",
+               device->kmod.dev->props.gpu_id, core_count);
+   else
+      sprintf(device->name, "%s MC%u", device->model->name, core_count);
+"""
new_name = """+   memset(device->name, 0, sizeof(device->name));
+   sprintf(device->name, "%s MC%u", device->model->name, core_count);
+"""
assert old_name in c, 'unknown_gpu name hunk not found'
c = c.replace(old_name, new_name)
# surgery net effect on the init_kbase hunk: +1 added line
c = c.replace('@@ -509,6 +1311,142 @@', '@@ -509,6 +1311,143 @@')
c = c.splitlines(keepends=True)
c = [('--- a/src/panfrost/vulkan/panvk_physical_device.c\n'
      if l.startswith('--- ') else
      ('+++ b/src/panfrost/vulkan/panvk_physical_device.c\n'
       if l.startswith('+++ ') else l)) for l in c]
header = """From: panvk-kbase-android worker <panvk-worker@localhost>
Subject: [PATCH kbase-common 004] panvk: kbase physical-device init path

Purpose: Add kbase_node_path to panvk_physical_device plus the
 panvk_physical_device_init_kbase() bring-up (sysfs helpers, kmod
 creation, GPU model/arch gating incl. v11, kbase sync helpers).
 Caller (instance enumeration) comes in 005.
Source/Reference: https://github.com/wonderkast02/panvk-g720-kbase-csf
 branch android-candidate-beta-1.9.4
 (src/panfrost/vulkan/panvk_physical_device.[hc]).
 .h taken whole (pure addition). .c keeps only hunks containing kbase:
 pure additions, no removals, no arch-list changes (v11 kept).
Tested GPU: none yet (compile-level).
Tested Kbase UAPI: n/a.
Mesa base range: 26.3.0-devel (pinned 5a07217f).
Dependencies: 003 (kbase_kmod backend + create_with_driver).
Validation test: build with kbase; DRM enumeration unchanged.

"""
open('patches/kbase-common/004-physical-device-kbase.patch', 'w').write(
    header + ''.join(h) + ''.join(c) + """--- a/src/panfrost/vulkan/panvk_physical_device.c
+++ b/src/panfrost/vulkan/panvk_physical_device.c
@@ -14,7 +14,17 @@

 #include <sys/stat.h>

+#if defined(HAVE_PAN_KMOD_KBASE)
+#include <errno.h>
+#include <fcntl.h>
+#include <poll.h>
+#include <string.h>
+#include <sys/ioctl.h>
+#include <unistd.h>
+#include "util/timespec.h"
+#endif
+
 #include "util/disk_cache.h"
 #include "util/os_misc.h"
 #include "util/u_atomic.h"
 #include "git_sha1.h"
""")
print('wrote 004')
