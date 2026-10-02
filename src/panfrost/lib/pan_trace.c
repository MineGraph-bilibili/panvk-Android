/*
 * Copyright © 2026 Amazon.com, Inc. or its affiliates.
 *
 * SPDX-License-Identifier: MIT
 */

#include "pan_trace.h"

#include "util/os_misc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unwind.h>
#include <dlfcn.h>

struct android_backtrace_state {
    void** current;
    void** end;
};

#define PAN_TRACE_ENV_VAR "PAN_CPU_TRACE"

#define CATEGORY(str, flag) { str, ARRAY_SIZE(str) - 1, (uint64_t) (flag) }

/* To be kept in sync with the pan_trace_category enum in pan_trace.h. */
/* clang-format off */
static struct {
   const char *str;
   size_t len;
   uint64_t flag;
} categories_table[] = {
   /* Library categories. */
   CATEGORY("lib.afbc",      PAN_TRACE_LIB_AFBC),
   CATEGORY("lib.desc",      PAN_TRACE_LIB_DESC),
   CATEGORY("lib.kmod",      PAN_TRACE_LIB_KMOD),

   CATEGORY("lib",           PAN_TRACE_LIB_AFBC |
                             PAN_TRACE_LIB_DESC |
                             PAN_TRACE_LIB_KMOD),

   /* Gallium categories. */
   CATEGORY("gl.blit",       PAN_TRACE_GL_BLIT),
   CATEGORY("gl.bo",         PAN_TRACE_GL_BO),
   CATEGORY("gl.cmdstream",  PAN_TRACE_GL_CMDSTREAM),
   CATEGORY("gl.context",    PAN_TRACE_GL_CONTEXT),
   CATEGORY("gl.csf",        PAN_TRACE_GL_CSF),
   CATEGORY("gl.disk_cache", PAN_TRACE_GL_DISK_CACHE),
   CATEGORY("gl.fb_preload", PAN_TRACE_GL_FB_PRELOAD),
   CATEGORY("gl.jm",         PAN_TRACE_GL_JM),
   CATEGORY("gl.job",        PAN_TRACE_GL_JOB),
   CATEGORY("gl.mempool",    PAN_TRACE_GL_MEMPOOL),
   CATEGORY("gl.resource",   PAN_TRACE_GL_RESOURCE),
   CATEGORY("gl.shader",     PAN_TRACE_GL_SHADER),

   CATEGORY("gl",            PAN_TRACE_GL_BLIT |
                             PAN_TRACE_GL_BO |
                             PAN_TRACE_GL_CMDSTREAM |
                             PAN_TRACE_GL_CONTEXT |
                             PAN_TRACE_GL_CSF |
                             PAN_TRACE_GL_DISK_CACHE |
                             PAN_TRACE_GL_FB_PRELOAD |
                             PAN_TRACE_GL_JM |
                             PAN_TRACE_GL_JOB |
                             PAN_TRACE_GL_MEMPOOL |
                             PAN_TRACE_GL_RESOURCE |
                             PAN_TRACE_GL_SHADER),

   /* Vulkan categories. */
   CATEGORY("vk.csf",        PAN_TRACE_VK_CSF),

   CATEGORY("vk",            PAN_TRACE_VK_CSF),
};
/* clang-format on */

uint64_t pan_trace_categories = 0;

static bool
is_separator(char c)
{
   return c == ',' || c == ';' || c == ' ';
}

void
pan_trace_init(void)
{
   const char *list = os_get_option(PAN_TRACE_ENV_VAR);
   const char *str = NULL;
   uint64_t categories = 0;
   char prev_char = ',';

   if (!list)
      return;

   /* Parse list and flag enabled categories. */
   for (int i = 0; prev_char; prev_char = list[i++]) {
      if (!is_separator(list[i]) && list[i]) {
         if (is_separator(prev_char))
            str = &list[i];
      } else if (!is_separator(prev_char)) {
         for (int j = 0; j < ARRAY_SIZE(categories_table); j++) {
            size_t len = &list[i] - str;
            if (categories_table[j].len == len &&
                !strncasecmp(categories_table[j].str, str, len)) {
               categories |= categories_table[j].flag;
               break;
            }
         }
      }
   }

   pan_trace_categories = categories;
}

static _Unwind_Reason_Code android_unwind_callback(struct _Unwind_Context* context, void* arg) {
    struct android_backtrace_state* state = (struct android_backtrace_state*)arg;
    uintptr_t pc = _Unwind_GetIP(context);
    if (pc) {
        if (state->current == state->end) {
            return _URC_END_OF_STACK;
        } else {
            *state->current++ = (void*)pc;
        }
    }
    return _URC_NO_REASON;
}

void print_stack_trace(void) {
    void* buffer[32];
    struct android_backtrace_state state = {buffer, buffer + 32};
    _Unwind_Backtrace(android_unwind_callback, &state);

    int count = state.current - buffer;
    fprintf(stderr, "--- Backtrace (%d frames) ---\n", count);
    bool prev_is_panvk = false;

    for (int i = 0; i < count; i++) {
        void* addr = buffer[i];
        const char* symbol = "";
        Dl_info info;
        if (dladdr(addr, &info) && info.dli_sname) {
            symbol = info.dli_sname;
        }
        int64_t fbase = (int64_t) info.dli_fbase;
        int64_t faddr = (int64_t) addr;
        bool is_panvk = strstr(info.dli_fname, "vulkan_panfrost");
        if (!is_panvk) {
            if (prev_is_panvk) fprintf(stderr, "\n");
            fprintf(stderr, "  #%02d pc %p  %s (%s:%p @ %lx)\n",
                     i,
                     addr,
                     info.dli_fname ? info.dli_fname : "unknown",
                     symbol[0] ? symbol : "unknown symbol",
                     info.dli_fbase,
                     faddr - fbase);
        } else {
            if (!prev_is_panvk) {
                fprintf(stderr, "  panvk: ");
            }
            fprintf(stderr, "%lx ", faddr - fbase);
        }
        prev_is_panvk = is_panvk;
    }
}
