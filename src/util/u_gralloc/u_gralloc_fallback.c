/*
 * Mesa 3-D graphics library
 *
 * Copyright © 2021, Google Inc.
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/system_properties.h>
#include "u_gralloc_internal.h"

#include <hardware/gralloc.h>

#include "drm-uapi/drm_fourcc.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_memory.h"

#include <dlfcn.h>
#include <errno.h>
#include <string.h>

struct fallback_gralloc {
   struct u_gralloc base;
   gralloc_module_t *gralloc_module;
};

/* returns # of fds, and by reference the actual fds */
static unsigned
get_native_buffer_fds(const native_handle_t *handle, int fds[3])
{
   if (!handle)
      return 0;

   /*
    * Various gralloc implementations exist, but the dma-buf fd tends
    * to be first. Access it directly to avoid a dependency on specific
    * gralloc versions.
    */
   for (int i = 0; i < handle->numFds; i++)
      fds[i] = handle->data[i];

   return handle->numFds;
}


typedef const native_handle_t *panvk_v19_buffer_handle_t;
typedef int32_t (*panvk_v19_import_fn)(const native_handle_t *, panvk_v19_buffer_handle_t *);
typedef int32_t (*panvk_v19_free_fn)(panvk_v19_buffer_handle_t);
typedef int32_t (*panvk_v19_getstd_fn)(panvk_v19_buffer_handle_t, int64_t, void *, size_t);
struct panvk_v19_mapper_v5 {
   panvk_v19_import_fn importBuffer;
   panvk_v19_free_fn freeBuffer;
   void *getTransportSize, *lock, *unlock, *flushLockedBuffer, *rereadLockedBuffer, *getMetadata;
   panvk_v19_getstd_fn getStandardMetadata;
};
struct panvk_v19_mapper {
   __attribute__((aligned(16))) uint32_t version;
   struct panvk_v19_mapper_v5 v5;
};
typedef int32_t (*panvk_v19_load_mapper_fn)(struct panvk_v19_mapper **);
typedef void *(*panvk_v19_open_passthrough_fn)(const char *, const char *, int);
typedef void *(*panvk_v19_load_sphal_fn)(const char *, int);

static pthread_once_t panvk_v19_once = PTHREAD_ONCE_INIT;
static struct panvk_v19_mapper *panvk_v19_mapper_ptr;
static void *panvk_v19_mapper_so;
static int panvk_v19_mapper_status = -ENOTSUP;

static void
panvk_v19_mapper_init_once(void)
{
   /* Doc-recommended hardening: don't hardcode one vendor mapper name.
    * Probe the SoC name from build properties and walk the common defaults
    * so other devices in the support matrix (G710/G610/...) can reach their
    * real vendor AIMapper before we ever fall back to guessing the layout. */
   char plat[PROP_VALUE_MAX] = {0}, hw[PROP_VALUE_MAX] = {0};
   __system_property_get("ro.board.platform", plat);
   __system_property_get("ro.hardware", hw);

   const char *names[5];
   int name_count = 0;
   names[name_count++] = "mediatek";
   names[name_count++] = "arm";
   if (plat[0] && strcmp(plat, "mediatek") && strcmp(plat, "arm"))
      names[name_count++] = plat;
   if (hw[0] && strcmp(hw, "mediatek") && strcmp(hw, "arm") &&
       (!plat[0] || strcmp(hw, plat)))
      names[name_count++] = hw;
   names[name_count++] = "default";

   void *bn = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
   if (!bn)
      bn = dlopen("/system/lib64/libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
   panvk_v19_open_passthrough_fn open_hal = NULL;
   if (bn)
      open_hal =
         (panvk_v19_open_passthrough_fn)dlsym(bn, "AServiceManager_openDeclaredPassthroughHal");

   void *vs = dlopen("libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
   if (!vs)
      vs = dlopen("/system/lib64/libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
   panvk_v19_load_sphal_fn load_sphal = NULL;
   if (vs)
      load_sphal =
         (panvk_v19_load_sphal_fn)dlsym(vs, "android_load_sphal_library");

   void *so = NULL;
   const char *how = "NONE";
   const char *used = NULL;
   char tried[256] = "";

   for (int i = 0; i < name_count; i++) {
      size_t len = strlen(tried);
      snprintf(tried + len, sizeof(tried) - len, "%s%s", len ? ", " : "", names[i]);

      if (open_hal &&
          (so = open_hal("mapper", names[i], RTLD_NOW | RTLD_LOCAL))) {
         how = "BINDER_PASSTHROUGH";
         used = names[i];
         break;
      }

      char so_name[PROP_VALUE_MAX + 16];
      char so_path[PROP_VALUE_MAX + 48];
      snprintf(so_name, sizeof(so_name), "mapper.%s.so", names[i]);
      snprintf(so_path, sizeof(so_path), "/vendor/lib64/hw/mapper.%s.so", names[i]);

      if (load_sphal &&
          ((so = load_sphal(so_name, RTLD_NOW | RTLD_LOCAL)) ||
           (so = load_sphal(so_path, RTLD_NOW | RTLD_LOCAL)))) {
         how = "SPHAL";
         used = names[i];
         break;
      }

      if ((so = dlopen(so_path, RTLD_NOW | RTLD_LOCAL))) {
         how = "DIRECT";
         used = names[i];
         break;
      }
   }

   if (!so) {
      mesa_logw("[P0A-V19-FULLPLANE] mapper load failed (tried: %s)", tried);
      return;
   }
   panvk_v19_load_mapper_fn load =
      (panvk_v19_load_mapper_fn)dlsym(so, "AIMapper_loadIMapper");
   if (!load) {
      mesa_logw("[P0A-V19-FULLPLANE] AIMapper_loadIMapper absent");
      return;
   }
   int32_t rc = load(&panvk_v19_mapper_ptr);
   mesa_logi("[P0A-V19-FULLPLANE] init how=%s name=%s rc=%d mapper=%p version=%u", how, used, rc,
             panvk_v19_mapper_ptr, panvk_v19_mapper_ptr ? panvk_v19_mapper_ptr->version : 0);
   if (rc || !panvk_v19_mapper_ptr || panvk_v19_mapper_ptr->version < 5 ||
       !panvk_v19_mapper_ptr->v5.importBuffer || !panvk_v19_mapper_ptr->v5.freeBuffer ||
       !panvk_v19_mapper_ptr->v5.getStandardMetadata)
      return;
   panvk_v19_mapper_so = so;
   panvk_v19_mapper_status = 0;
}

static int
panvk_v19_init_mapper(void)
{
   int rc = pthread_once(&panvk_v19_once, panvk_v19_mapper_init_once);
   if (rc || !panvk_v19_mapper_so) return -ENOTSUP;
   return panvk_v19_mapper_status;
}

struct panvk_v19_blob { uint8_t *data; size_t size; };

static int
panvk_v19_get_blob(panvk_v19_buffer_handle_t h, int64_t type, struct panvk_v19_blob *out)
{
   memset(out, 0, sizeof(*out));
   int32_t need = panvk_v19_mapper_ptr->v5.getStandardMetadata(h, type, NULL, 0);
   if (need <= 0 || need > 65536) return -EINVAL;
   uint8_t *data = malloc((size_t)need);
   if (!data) return -ENOMEM;
   int32_t got = panvk_v19_mapper_ptr->v5.getStandardMetadata(h, type, data, (size_t)need);
   if (got <= 0 || got > need) { free(data); return -EINVAL; }
   out->data = data;
   out->size = (size_t)got;
   return 0;
}

static int
panvk_v19_read_u64(const struct panvk_v19_blob *b, size_t *pos, uint64_t *v)
{
   if (*pos > b->size || b->size - *pos < sizeof(*v)) return -EINVAL;
   memcpy(v, b->data + *pos, sizeof(*v));
   *pos += sizeof(*v);
   return 0;
}

static int
panvk_v19_read_i64(const struct panvk_v19_blob *b, size_t *pos, int64_t *v)
{
   if (*pos > b->size || b->size - *pos < sizeof(*v)) return -EINVAL;
   memcpy(v, b->data + *pos, sizeof(*v));
   *pos += sizeof(*v);
   return 0;
}

static int
panvk_v19_skip_string(const struct panvk_v19_blob *b, size_t *pos, const char *expected)
{
   uint64_t n = 0;
   if (panvk_v19_read_u64(b, pos, &n) || n > 512 || *pos > b->size || n > b->size - *pos)
      return -EINVAL;
   if (expected && (n != strlen(expected) || memcmp(b->data + *pos, expected, (size_t)n)))
      return -EINVAL;
   *pos += (size_t)n;
   return 0;
}

static int
panvk_v19_metadata_header(const struct panvk_v19_blob *b, int64_t expected_type, size_t *pos)
{
   static const char name[] = "android.hardware.graphics.common.StandardMetadataType";
   int64_t type = -1;
   *pos = 0;
   if (panvk_v19_skip_string(b, pos, name) || panvk_v19_read_i64(b, pos, &type) ||
       type != expected_type)
      return -EINVAL;
   return 0;
}

static int
panvk_v19_get_scalar(panvk_v19_buffer_handle_t h, int64_t type, void *dst, size_t width)
{
   struct panvk_v19_blob b;
   int rc = panvk_v19_get_blob(h, type, &b);
   if (rc) return rc;
   size_t pos = 0;
   rc = panvk_v19_metadata_header(&b, type, &pos);
   if (!rc && pos <= b.size && width == b.size - pos)
      memcpy(dst, b.data + pos, width);
   else if (!rc)
      rc = -EINVAL;
   free(b.data);
   return rc;
}

static int
panvk_v19_decode_planes(panvk_v19_buffer_handle_t h, const native_handle_t *raw,
                        uint64_t allocation, struct u_gralloc_buffer_basic_info *out)
{
   static const char component_name[] =
      "android.hardware.graphics.common.PlaneLayoutComponentType";
   struct panvk_v19_blob b;
   int rc = panvk_v19_get_blob(h, 15, &b);
   if (rc) return rc;
   size_t pos = 0;
   uint64_t planes = 0;
   rc = panvk_v19_metadata_header(&b, 15, &pos);
   if (rc || panvk_v19_read_u64(&b, &pos, &planes) || planes == 0 || planes > 4) {
      free(b.data); return -EINVAL;
   }
   int strides[4] = {0}, offsets[4] = {0};
   int fds[4] = {-1, -1, -1, -1};
   int fd_index = 0;
   for (uint64_t i = 0; i < planes; i++) {
      uint64_t components = 0;
      if (panvk_v19_read_u64(&b, &pos, &components) || components > 16) { rc = -EINVAL; break; }
      for (uint64_t c = 0; c < components; c++) {
         int64_t component_type, bit_offset, bit_size;
         if (panvk_v19_skip_string(&b, &pos, component_name) ||
             panvk_v19_read_i64(&b, &pos, &component_type) ||
             panvk_v19_read_i64(&b, &pos, &bit_offset) ||
             panvk_v19_read_i64(&b, &pos, &bit_size) ||
             bit_offset < 0 || bit_size <= 0) { rc = -EINVAL; break; }
      }
      if (rc) break;
      int64_t offset, sample_inc, stride, width, height, total, hsub, vsub;
      if (panvk_v19_read_i64(&b, &pos, &offset) ||
          panvk_v19_read_i64(&b, &pos, &sample_inc) ||
          panvk_v19_read_i64(&b, &pos, &stride) ||
          panvk_v19_read_i64(&b, &pos, &width) ||
          panvk_v19_read_i64(&b, &pos, &height) ||
          panvk_v19_read_i64(&b, &pos, &total) ||
          panvk_v19_read_i64(&b, &pos, &hsub) ||
          panvk_v19_read_i64(&b, &pos, &vsub) ||
          offset < 0 || offset > INT_MAX ||
          sample_inc <= 0 || stride <= 0 || stride > INT_MAX ||
          width <= 0 || height <= 0 || total <= 0 ||
          (uint64_t)offset > allocation || (uint64_t)total > allocation - (uint64_t)offset ||
          hsub <= 0 || vsub <= 0) { rc = -EINVAL; break; }
      offsets[i] = (int)offset;
      strides[i] = (int)stride;
      if (offsets[i] == 0 && i > 0) fd_index++;
      if (!raw || fd_index >= raw->numFds) { rc = -EINVAL; break; }
      fds[i] = raw->data[fd_index];
      mesa_logi("[P0A-V19-FULLPLANE] plane=%u fd_index=%d offset=%d stride=%d total=%lld sample_bits=%lld samples=%lldx%lld sub=%lldx%lld",
                (unsigned)i, fd_index, offsets[i], strides[i], (long long)total,
                (long long)sample_inc, (long long)width, (long long)height,
                (long long)hsub, (long long)vsub);
   }
   if (!rc && pos != b.size) rc = -EINVAL;
   if (!rc) {
      out->num_planes = (int)planes;
      for (int i = 0; i < out->num_planes; i++) {
         out->fds[i] = fds[i];
         out->strides[i] = strides[i];
         out->offsets[i] = offsets[i];
      }
   }
   free(b.data);
   return rc;
}

static int
panvk_v19_query_mapper(const native_handle_t *raw, struct u_gralloc_buffer_basic_info *out)
{
   if (!raw || !out || panvk_v19_init_mapper()) return -ENOTSUP;
   panvk_v19_buffer_handle_t imported = NULL;
   int32_t ir = panvk_v19_mapper_ptr->v5.importBuffer(raw, &imported);
   if (ir || !imported) {
      mesa_logw("[P0A-V19-FULLPLANE] import rc=%d", ir);
      return -EINVAL;
   }
   uint32_t fourcc = 0;
   uint64_t modifier = DRM_FORMAT_MOD_INVALID, allocation = 0, layer_count = 0;
   int rl = panvk_v19_get_scalar(imported, 5, &layer_count, sizeof(layer_count));
   int rf = panvk_v19_get_scalar(imported, 7, &fourcc, sizeof(fourcc));
   int rm = panvk_v19_get_scalar(imported, 8, &modifier, sizeof(modifier));
   int ra = panvk_v19_get_scalar(imported, 10, &allocation, sizeof(allocation));
   int rp = 0;
   if (!rl && !rf && !rm && !ra && layer_count && fourcc &&
       modifier != DRM_FORMAT_MOD_INVALID && allocation)
      rp = panvk_v19_decode_planes(imported, raw, allocation, out);
   else
      rp = -EINVAL;
   int32_t fr = panvk_v19_mapper_ptr->v5.freeBuffer(imported);
   mesa_logi("[P0A-V19-FULLPLANE] metadata layer_rc=%d layers=%llu fourcc_rc=%d fourcc=0x%08x modifier_rc=%d modifier=0x%016llx alloc_rc=%d alloc=%llu planes_rc=%d free_rc=%d",
             rl, (unsigned long long)layer_count, rf, fourcc, rm,
             (unsigned long long)modifier, ra, (unsigned long long)allocation, rp, fr);
   if (rl || rf || rm || ra || rp || fr) return -EINVAL;
   out->drm_fourcc = fourcc;
   out->modifier = modifier;
   mesa_logi("[P0A-V19-FULLPLANE] accepted fourcc=0x%08x modifier=0x%016llx planes=%d",
             out->drm_fourcc, (unsigned long long)out->modifier, out->num_planes);
   return 0;
}

static int
fallback_gralloc_get_yuv_info(struct u_gralloc *gralloc,
                              struct u_gralloc_buffer_handle *hnd,
                              struct u_gralloc_buffer_basic_info *out)
{
   struct fallback_gralloc *gr = (struct fallback_gralloc *)gralloc;
   gralloc_module_t *gr_mod = gr->gralloc_module;
   struct android_ycbcr ycbcr;
   int num_fds = 0;
   int fds[3];
   int ret;

   num_fds = get_native_buffer_fds(hnd->handle, fds);
   if (num_fds == 0)
      return -EINVAL;

   if (!gr_mod || !gr_mod->lock_ycbcr) {
      return -EINVAL;
   }

   memset(&ycbcr, 0, sizeof(ycbcr));
   ret = gr_mod->lock_ycbcr(gr_mod, hnd->handle, 0, 0, 0, 0, 0, &ycbcr);
   if (ret) {
      /* HACK: See native_window_buffer_get_buffer_info() and
       * https://issuetracker.google.com/32077885.*/
      if (hnd->hal_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
         return -EAGAIN;

      mesa_logw("gralloc->lock_ycbcr failed: %d", ret);
      return -EINVAL;
   }
   gr_mod->unlock(gr_mod, hnd->handle);

   ret = bufferinfo_from_ycbcr(&ycbcr, hnd, out);
   if (ret)
      return ret;

   /*
    * Since this is EGL_NATIVE_BUFFER_ANDROID don't assume that
    * the single-fd case cannot happen.  So handle eithe single
    * fd or fd-per-plane case:
    */
   if (num_fds == 1) {
      out->fds[1] = out->fds[0] = fds[0];
      if (out->num_planes == 3)
         out->fds[2] = fds[0];
   } else {
      assert(num_fds == out->num_planes);
      out->fds[0] = fds[0];
      out->fds[1] = fds[1];
      out->fds[2] = fds[2];
   }

   return 0;
}

/* Vendor metadata route: MTK's gralloc_extra plain-C API -- the same metadata
 * core the vendor mapper4 impl links against (libgralloc_extra.so is in its
 * DT_NEEDED), reachable from an untrusted app because it is a plain function
 * call, no HIDL/binder/ioctl.  Semantics pinned by the panvk.24 GED-DIAG
 * device dump on a real BGRA_8888 640x454 swapchain buffer: attr 12 = 640
 * (stride/width px), 13 = 454 (height), 14 = 1162240 == stride*height*bpp,
 * proving plain linear.  When the size matches, emit exact metadata (fourcc +
 * LINEAR + stride) so the buffer import no longer relies on a blind guess;
 * a size mismatch means vendor compression or a multi-plane layout, which
 * the linear guess cannot represent.  drm_fourcc and stride are the
 * caller-validated values derived from the AHB/ANB descriptor.
 * Returns 0 on accepted metadata, -EINVAL for a non-linear (unrepresentable)
 * layout, -ENOTSUP when the vendor API is unavailable. */
static int
panvk_v19_query_gralloc_extra(struct u_gralloc_buffer_handle *hnd,
                              uint32_t drm_fourcc, int stride,
                              struct u_gralloc_buffer_basic_info *out)
{
   static int (*ge_query)(const native_handle_t *, unsigned int, void *);
   static bool ge_tried = false;

   if (!ge_tried) {
      ge_tried = true;
      /* Preload the one vendor dependency so its soname resolves. */
      dlopen("/vendor/lib64/libged.so", RTLD_NOW | RTLD_LOCAL);
      void *so = dlopen("/vendor/lib64/libgralloc_extra.so", RTLD_NOW | RTLD_LOCAL);
      if (so) {
         ge_query = (int (*)(const native_handle_t *, unsigned int, void *))
            dlsym(so, "gralloc_extra_query");
      }
      if (!ge_query) {
         mesa_logw("[P0A-V19-FULLPLANE] vendor gralloc_extra unavailable: %s",
                   dlerror() ? dlerror() : "gralloc_extra_query absent");
         return -ENOTSUP;
      }
   }

   int32_t width = 0, height = 0, size = 0;
   if (!ge_query || ge_query(hnd->handle, 12, &width) ||
       ge_query(hnd->handle, 13, &height) || ge_query(hnd->handle, 14, &size) ||
       width <= 0 || height <= 0 || size <= 0)
      return -ENOTSUP;

   int bpp = get_hal_format_bpp(hnd->hal_format);
   if (bpp <= 0 || stride <= 0)
      return -ENOTSUP;

   int64_t linear = (int64_t)stride * height;
   if ((int64_t)size != linear) {
      mesa_logw("[P0A-V19-FULLPLANE] vendor metadata: size=%d != linear %lld "
                "(attr12=%d bpp=%d h=%d) -- layout not plain linear",
                size, (long long)linear, width, bpp, height);
      return -EINVAL;
   }

   out->drm_fourcc = drm_fourcc;
   out->modifier = DRM_FORMAT_MOD_LINEAR;
   out->num_planes = 1;
   out->fds[0] = hnd->handle->data[0];
   out->strides[0] = stride;
   out->offsets[0] = 0;
   mesa_logi("[P0A-V19-FULLPLANE] vendor metadata accepted fourcc=0x%08x modifier=LINEAR stride=%d (size=%d h=%d)",
             drm_fourcc, stride, size, height);
   return 0;
}

static int
fallback_gralloc_get_buffer_info(struct u_gralloc *gralloc,
                                 struct u_gralloc_buffer_handle *hnd,
                                 struct u_gralloc_buffer_basic_info *out)
{
   int num_planes = 0;
   int drm_fourcc = 0;
   int stride = 0;

   if (hnd->handle->numFds == 0)
      return -EINVAL;

   if (is_hal_format_yuv(hnd->hal_format)) {
      int ret = fallback_gralloc_get_yuv_info(gralloc, hnd, out);
      /*
       * HACK: https://issuetracker.google.com/32077885
       * There is no API available to properly query the
       * IMPLEMENTATION_DEFINED format. As a workaround we rely here on
       * gralloc allocating either an arbitrary YCbCr 4:2:0 or RGBX_8888, with
       * the latter being recognized by lock_ycbcr failing.
       */
      if (ret != -EAGAIN)
         return ret;
   }

   /*
    * Non-YUV formats could *also* have multiple planes, such as ancillary
    * color compression state buffer, but the rest of the code isn't ready
    * yet to deal with modifiers:
    */
   num_planes = 1;

   drm_fourcc = get_fourcc_from_hal_format(hnd->hal_format);
   if (drm_fourcc == -1) {
      mesa_loge("Failed to get drm_fourcc");
      return -EINVAL;
   }

   stride = hnd->pixel_stride * get_hal_format_bpp(hnd->hal_format);
   if (stride == 0) {
      mesa_loge("Failed to calcuulate stride");
      return -EINVAL;
   }

   out->drm_fourcc = drm_fourcc;
   out->modifier = DRM_FORMAT_MOD_INVALID;
   out->num_planes = num_planes;
   out->fds[0] = hnd->handle->data[0];
   out->strides[0] = stride;

#ifdef HAS_FREEDRENO
   uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
   if (hnd->handle->numInts >= 2 && hnd->handle->data[hnd->handle->numFds] == gmsm) {
      /* This UBWC flag was introduced in a5xx. */
      bool ubwc = hnd->handle->data[hnd->handle->numFds + 1] & 0x08000000;
      out->modifier = ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
   }
#endif



   /* Precise metadata via the vendor AIMapper when available. If the mapper
    * is unavailable (e.g. sphal mapper blocked by SELinux on some vendors),
    * fall back to the guessed RGB layout above instead of failing: failing
    * here breaks every swapchain, including plain RGBA_8888 ones. Vendor
    * formats that cannot be guessed are rejected earlier in this function.
    */
   int stable_ret = panvk_v19_query_mapper(hnd->handle, out);
   if (stable_ret != 0)
      stable_ret = panvk_v19_query_gralloc_extra(hnd, drm_fourcc, stride, out);

   if (stable_ret == -EINVAL) {
      /* Vendor metadata proved the physical layout is not plain linear; the
       * LINEAR guess would render garbage (doc side-effect: AFBC detected).
       * Refuse under the escape hatch, warn loudly otherwise. */
      if (debug_get_bool_option("PANVK_KBASE_NO_GUESS", false)) {
         mesa_logw("[P0A-V19-FULLPLANE] vendor metadata says NOT linear and "
                   "PANVK_KBASE_NO_GUESS=1: refusing import (fourcc=0x%08x stride=%d)",
                   drm_fourcc, stride);
         return -ENOTSUP;
      }
      mesa_logw("[P0A-V19-FULLPLANE] guessing LINEAR on a non-linear buffer "
                "(fourcc=0x%08x stride=%d); set PANVK_KBASE_NO_GUESS=1 to refuse instead",
                drm_fourcc, stride);
   } else if (stable_ret != 0) {
      mesa_logi("[P0A-V19-FULLPLANE] no mapper/vendor metadata (rc=%d); using guessed RGB layout fourcc=0x%08x stride=%d",
                stable_ret, drm_fourcc, stride);
   }

   return 0;
}

static int
destroy(struct u_gralloc *gralloc)
{
   struct fallback_gralloc *gr = (struct fallback_gralloc *)gralloc;
   if (gr->gralloc_module) {
      dlclose(gr->gralloc_module->common.dso);
   }

   FREE(gr);

   return 0;
}

struct u_gralloc *
u_gralloc_fallback_create()
{
   struct fallback_gralloc *gr = CALLOC_STRUCT(fallback_gralloc);
   int err = 0;

   err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                       (const hw_module_t **)&gr->gralloc_module);

   if (err) {
      mesa_logw(
         "No gralloc hwmodule detected (video buffers won't be supported)");
   } else if (!gr->gralloc_module->lock_ycbcr) {
      mesa_logw("Gralloc doesn't support lock_ycbcr (video buffers won't be "
                "supported)");
   }

   gr->base.ops.get_buffer_basic_info = fallback_gralloc_get_buffer_info;
   gr->base.ops.destroy = destroy;

   mesa_logi("Using fallback gralloc implementation");

   return &gr->base;
}
