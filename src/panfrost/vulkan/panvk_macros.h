/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_MACROS_H
#define PANVK_MACROS_H

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

/* Diagnostics are compiled out so they cannot cost anything on hot paths.
 * Rebuild with -DPANVK_DIAG_ENABLE to get the stderr/logcat version back. */
#ifdef PANVK_DIAG_ENABLE
static inline void
panvk_diag_printf(const char *format, ...)
{
   char buf[512];
   va_list va;

   va_start(va, format);
   vsnprintf(buf, sizeof(buf), format, va);
   va_end(va);

   fputs(buf, stderr);
#ifdef __ANDROID__
   __android_log_write(ANDROID_LOG_ERROR, "PANVK-DIAG", buf);
#endif
}

#define PANVK_DIAG_PRINTF(...) panvk_diag_printf(__VA_ARGS__)
#else
#define PANVK_DIAG_PRINTF(...) ((void)0)
#endif

#ifdef HAVE_VALGRIND
#include <memcheck.h>
#include <valgrind.h>
#define VG(x) x
#else
#define VG(x)
#endif

#include "vk_log.h"

static inline VkResult
panvk_catch_indirect_alloc_failure(VkResult error)
{
   /* errno is set to -ENOMEM in the kmod allocator callback when an allocation
    * fails. When that's the case, the allocation failure takes precedence on
    * the original error code. We also reset errno before leaving so we don't
    * end up reporting the same allocation failure twice. */
   if (errno == -ENOMEM) {
      PANVK_DIAG_PRINTF("[PANVK-DIAG] indirect alloc failure (-ENOMEM)\n");
      errno = 0;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return error;
}

static inline VkResult
panvk_diag_result(VkResult result, const char *file, int line)
{
   if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
       result == VK_ERROR_OUT_OF_HOST_MEMORY)
      PANVK_DIAG_PRINTF("[PANVK-DIAG] %s return at %s:%d\n",
              result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "OOM" : "OOHM",
              file, line);
   return result;
}

#define panvk_error(obj, error)                                                \
   vk_error(obj, panvk_catch_indirect_alloc_failure(                           \
                     panvk_diag_result(error, __FILE__, __LINE__)))

#define panvk_errorf(obj, error, ...)                                          \
   vk_errorf(obj, panvk_catch_indirect_alloc_failure(                          \
                       panvk_diag_result(error, __FILE__, __LINE__)),          \
             __VA_ARGS__)

#define panvk_stub() assert(!"stub")

#define panvk_arch_name(name, version) panvk_##version##_##name

#define panvk_arch_dispatch(arch, name, ...)                                   \
   do {                                                                        \
      switch (arch) {                                                          \
      case 6:                                                                  \
         panvk_arch_name(name, v6)(__VA_ARGS__);                               \
         break;                                                                \
      case 7:                                                                  \
         panvk_arch_name(name, v7)(__VA_ARGS__);                               \
         break;                                                                \
      case 9:                                                                  \
         panvk_arch_name(name, v9)(__VA_ARGS__);                               \
         break;                                                                \
      case 10:                                                                 \
         panvk_arch_name(name, v10)(__VA_ARGS__);                              \
         break;                                                                \
      case 11:                                                                 \
         panvk_arch_name(name, v11)(__VA_ARGS__);                              \
         break;                                                                \
      case 12:                                                                 \
         panvk_arch_name(name, v12)(__VA_ARGS__);                              \
         break;                                                                \
      case 13:                                                                 \
         panvk_arch_name(name, v13)(__VA_ARGS__);                              \
         break;                                                                \
      case 14:                                                                 \
         panvk_arch_name(name, v14)(__VA_ARGS__);                              \
         break;                                                                \
      default:                                                                 \
         UNREACHABLE("Unsupported architecture");                              \
      }                                                                        \
   } while (0)

#define panvk_arch_dispatch_ret(arch, name, ret, ...)                          \
   do {                                                                        \
      switch (arch) {                                                          \
      case 6:                                                                  \
         ret = panvk_arch_name(name, v6)(__VA_ARGS__);                         \
         break;                                                                \
      case 7:                                                                  \
         ret = panvk_arch_name(name, v7)(__VA_ARGS__);                         \
         break;                                                                \
      case 9:                                                                  \
         ret = panvk_arch_name(name, v9)(__VA_ARGS__);                         \
         break;                                                                \
      case 10:                                                                 \
         ret = panvk_arch_name(name, v10)(__VA_ARGS__);                        \
         break;                                                                \
      case 11:                                                                 \
         ret = panvk_arch_name(name, v11)(__VA_ARGS__);                        \
         break;                                                                \
      case 12:                                                                 \
         ret = panvk_arch_name(name, v12)(__VA_ARGS__);                        \
         break;                                                                \
      case 13:                                                                 \
         ret = panvk_arch_name(name, v13)(__VA_ARGS__);                        \
         break;                                                                \
      case 14:                                                                 \
         ret = panvk_arch_name(name, v14)(__VA_ARGS__);                        \
         break;                                                                \
      default:                                                                 \
         UNREACHABLE("Unsupported architecture");                              \
      }                                                                        \
   } while (0)

#ifdef PAN_ARCH
#if PAN_ARCH == 6
#define panvk_per_arch(name) panvk_arch_name(name, v6)
#elif PAN_ARCH == 7
#define panvk_per_arch(name) panvk_arch_name(name, v7)
#elif PAN_ARCH == 9
#define panvk_per_arch(name) panvk_arch_name(name, v9)
#elif PAN_ARCH == 10
#define panvk_per_arch(name) panvk_arch_name(name, v10)
#elif PAN_ARCH == 11
#define panvk_per_arch(name) panvk_arch_name(name, v11)
#elif PAN_ARCH == 12
#define panvk_per_arch(name) panvk_arch_name(name, v12)
#elif PAN_ARCH == 13
#define panvk_per_arch(name) panvk_arch_name(name, v13)
#elif PAN_ARCH == 14
#define panvk_per_arch(name) panvk_arch_name(name, v14)
#else
#error "Unsupported arch"
#endif
#endif

#endif
