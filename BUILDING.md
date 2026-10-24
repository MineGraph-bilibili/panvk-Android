# BUILDING — PanVK Kbase Android driver (v1 branch)

This repository is an Android-only **PanVK** Vulkan driver fork for Mali GPUs that are
driven through the **kbase** kernel interface (`/dev/mali0`) instead of the DRM/panthor
path. It ships as one universal `libvulkan_panfrost.so` that contains every supported
GPU generation; the actual architecture is picked at runtime from the kbase GPU
properties.

| Item | Value |
| --- | --- |
| Mesa base | `5a07217f034b3e50d8c7c7794f97a2df1742613b` (26.3.0-devel, branch `main`) |
| Patch series | 222 patches, `patches/` (order: `common android kbase-common app-loader wsi csf csf-v11 jm-v9 jm-v7 perf local`) |
| Profile | `profiles/g615-v11-csf.json` — Mali-G615 MC6 / MT6897, panArch 11, CSF, kbase uAPI 1.21 |
| GPU generations | v6/v7 (Bifrost JM, experimental), v9 (Valhall JM), v10–v14 (CSF) — no v8 |
| ABI | `android-aarch64-bionic`, hot-loadable Vulkan ICD |
| Package | `.adpkg.zip` (flat ZIP: `libvulkan_panfrost.so` + `meta.json` + manifest) |
| `minApi` | 35 (Android 15+) — hard-coded in `scripts/package-android-adpkg.sh` |
| Kernel interface | `mali_kbase`, uAPI 1.21 (1.18/1.14/1.6 layouts handled at runtime) |

The checked-out tree **already contains every patch applied** (`patches/` is kept so the
series can be reproduced and reviewed). It is the pruned source import used for this
Android build, not a complete desktop-Mesa checkout: desktop-only drivers/files that the
Android build never compiles are not part of it.

---

## 1. Prerequisites

Host: x86_64 Linux (Ubuntu 24.04 or WSL2 Ubuntu 24.04). Windows cannot build this.

```sh
sudo apt update
sudo apt install -y build-essential git python3 python3-pip pkg-config \
    meson ninja-build bison flex ccache zip unzip curl \
    llvm-dev libclang-dev \
    libdrm-dev libexpat1-dev zlib1g-dev \
    libxcb1-dev libx11-dev libxshmfence-dev glslang-tools
```

* `meson >= 1.4` and `ninja` are used for both the host codegen tools and the
  Android cross build.
* `llvm-dev` is required by the **host tools** build (`mesa_clc`, `vtn_bindgen2`,
  `panfrost_compile`); the target `.so` does not link LLVM.
* `libdrm-dev` / `libexpat1-dev` are host-side probes only (the kbase backend does not
  link libdrm, and expat only exists so meson does not try to download
  `subprojects/expat.wrap`).
* The X11/XCB headers are used for the Android **X11 WSI** (the libraries themselves are
  `dlopen()`ed at runtime by `csf-v11/083`, so no target X11 libraries are needed).

Android NDK (r27 family, tested with **r27d**):

```sh
# e.g. https://dl.google.com/android/repository/android-ndk-r27d-linux.zip
unzip android-ndk-r27d-linux.zip -d /opt
export ANDROID_NDK_ROOT=/opt/android-ndk-r27d
```

Disk: ~10 GB free (host tools build + Android build + objects).

---

## 2. Get the sources

```sh
git clone -b v1 https://github.com/MineGraph-bilibili/panvk-Android.git
cd panvk-Android
```

Everything below is run from the repository root. The tree is already patched, so you
can build it in place.

### 2b. Optional: rebuild from a pristine Mesa checkout

To reproduce the patched tree from upstream instead of using the checked-in result:

```sh
./scripts/fetch-mesa.sh                      # clones the pinned commit into work/mesa
./scripts/apply-patches.sh --profile g615-v11-csf --mesa work/mesa
# must print: OK applied=222 profile=g615-v11-csf
MESA=work/mesa ./scripts/build-android.sh --profile g615-v11-csf --ndk /opt/android-ndk-r27d
```

`apply-patches.sh` fails hard (`PATCH-DRIFT: ... exit 1`) if any patch does not apply, and
falls back from `git apply --check` to `--recount` and then `-C1` (one line of context)
before giving up. It also tars each family's `patches/<family>/files/` overlay onto the
tree when that directory exists (`app-loader`, `kbase-common`).

Apply patches **before** building the host tools: `csf-v11/062` adds a new NIR pass file,
and a host build made from an unpatched tree cannot find it.

---

## 3. Build the driver

```sh
# in-place build of the checked-out tree
MESA=$PWD ./scripts/build-android.sh --profile g615-v11-csf --ndk /opt/android-ndk-r27d
```

Output:

```
dist/android-g615-v11-csf/libvulkan_panfrost.so     (~27 MB, universal v6..v14)
```

Notes:

* `build-android.sh` bootstraps the host codegen tools automatically
  (`build/host-tools-*/bin` → `mesa_clc`, `vtn_bindgen2`, `panfrost_compile`) unless
  `HOST_TOOLS` is already set. Host tools must come from a **native** build; the script
  derives their build directory from `$MESA`.
* Default target API is **29** (`--api` / `ANDROID_API` to change) — bionic's
  `<threads.h>` before API 30 is inlined, so the ICD still loads on older systems even
  though the package advertises `minApi=35`.
* `android-stub=true` is required: it provides the AIDL `nativewindow`/`sync` stubs.
* Useful overrides: `BDIR` (build dir), `DDIR` (output dir), `MESA` (source tree),
  `HOST_TOOLS` (prebuilt tools), `NPROC`.
* Full meson setup + compile log: `build/android-bionic.log`.

The configure line (for reference):

```
meson setup build/android-bionic $MESA --cross-file build/android-bionic.cross.ini \
  -Dbuildtype=release -Dplatforms=android,x11 -Dandroid-stub=true -Dandroid-strict=false \
  -Dgallium-drivers= -Dvulkan-drivers=panfrost -Dpanfrost-kmds=kbase \
  -Dmesa-clc=system -Dprecomp-compiler=system -Dxlib-lease=disabled \
  -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
  -Dglx=disabled -Dgbm=disabled -Dlibunwind=disabled -Dzstd=disabled \
  -Dcpp_link_args=-static-libstdc++
```

The cross file is `meson/android-aarch64.ini` with the API level substituted in; if
`work/android-deps/lib/pkgconfig` exists it is prepended to `pkg_config_libdir` so host
`.pc` files can never leak host paths into the link.

---

## 4. Package as an `.adpkg`

```sh
bash scripts/package-android-adpkg.sh --profile g615-v11-csf --version v0.1.0
```

Produces `dist/PanVK-Kbase-Android-g615-v11-csf-v0.1.0-5a07217f.adpkg.zip` containing:

| Entry | Meaning |
| --- | --- |
| `libvulkan_panfrost.so` | the ICD |
| `meta.json` | minApi 35, abi, backend `panvk-kbase`, gpuId/panArch/frontend/kbaseUapi, `sourceCommit`, `patchSeriesId` |
| `MANIFEST.json` / `SOURCE.json` | build provenance (mesa commit, patch-series hash) |
| `SHA256SUMS.txt` | so checksum |
| `runtime-feature-matrix.json`, `extension-gap.json` | Vulkan feature/extension coverage snapshot |

`runtime-feature-matrix.json` (from `validation/g615-v11-csf/`) and
`dist/extension-gap.json` are checked in as the inputs of the last on-device capture; they
can be regenerated on a device with `scripts/capture-runtime-features.py` and
`scripts/collect-validation.py`. `package-android-adpkg.sh` exits if they are missing.

`patchSeriesId` is a SHA-256 over the mesa commit, the profile and `patches/**`
(`scripts/compute-patch-series-id.py`), so two packages with the same id were built from an
identical patch set.

---

## 5. Install and verify

* **Winlator / Mali-Plus containers:** import the `.adpkg.zip` with the container's
  "install driver package" action and create a fresh container (do not reuse a container
  that was created with a different driver).
* **Manual:** `adb push dist/android-g615-v11-csf/libvulkan_panfrost.so /data/local/tmp/`
  and let the Vulkan loader pick it up via `VK_ICD_FILENAMES` for a test app.

Quick checks:

```sh
adb shell dumpsys SurfaceFlinger | head          # sanity
adb logcat | grep -iE "panvk|kbase|vulkan"       # "Failed to bind a kbase CS queue" must not appear
```

---

## 6. Runtime switches (no rebuild needed)

| Variable | Default | Effect |
| --- | --- | --- |
| `PANVK_KBASE_IMPORT_CLEAR` / `PANVK_KBASE_IMPORT_SET` | import flags = `BASE_MEM_IMPORT_SHARED \| BASE_MEM_COHERENT_SYSTEM` (0x400) | add/remove kbase import flags bit masks for dma-buf imports (hex or dec). Clearing `0x400` and setting `0x200000` gives the old `BASE_MEM_UNCACHED_GPU` behaviour |
| `PANVK_KBASE_DEBUG_IMPORT` | off | log every dma-buf import (fd, size, flags) |
| `PANVK_BC_CHUNK` | 256 | texel chunk edge for the BC decode compute dispatch (`0` = old single-block dispatch) |
| `PANVK_BC_TRACE` | off | trace BC transcode dispatches (use with `MESA_LOG_FILE`) |
| `PANVK_BC_ETC` / `PANVK_BC_EAC` | on | `=0` disables BC1–3 → ETC2 / BC4–5 → EAC transcode |
| `PANVK_KBASE_TILER_CHUNK_KB` | 8192 | kbase tiler heap chunk size in KiB (`2048` = old behaviour) |
| `PANVK_KBASE_HEAP_RENEW_INTERVAL` / `PANVK_KBASE_HEAP_RENEW_WORK` | built-in | tiler heap renewal pacing |
| `PANVK_RP_MERGE`, `PANVK_PASS_STATS`, `PANVK_RP_TRACE`, `PANVK_BAR_TRACE`, `PANVK_DROP_WAITS` | on/off per patch | render-pass merge and barrier-wait tuning (see `PATCHES.md`) |
| `MESA_LOG_FILE` | stderr | write MESA logs to a file (recommended on vivo/MTK, logd rate-limits high-frequency processes) |

---

## 7. Supported hardware and known limits

* The `.so` is universal: `libpanvk_v6 … v14` are all compiled in (no v8). The
  architecture is selected from kbase GPU properties at `vkCreateDevice` time; no device id
  is hard-coded.
* v9 (Valhall JM) and v6/v7 (Bifrost JM) are **experimental**: they have never been
  validated on real devices.
* `minApi=35` — the package is refused on Android 14 and older even though the binary is
  built for API 29.
* `perf/003` raises the CSF ring buffer to 256 KB. Kernels that reject that size fail at
  `CS_QUEUE_REGISTER`, which surfaces as "Failed to bind a kbase CS queue". If that happens
  on an old kbase (uAPI 1.6–1.17), drop `patches/perf/003-ringbuf-256k.patch` and rebuild;
  nothing else depends on it.
* `kbase_kmod_import_dmabuf()` derives the import size with a fixed 4096-byte page size.
  Correct on 4 KB-page kernels (MT6897, MT6985); 16 KB-page kernels would compute a wrong
  size and need that site changed to the sysconf page size.
* Desktop/GL is not built: this target is Vulkan-only (`-Dgallium-drivers=`), Android +
  X11 WSI, AHB (dma-buf) path.

---

## 8. Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `BUILD-FAIL: meson setup` | read `build/android-bionic.log`; usually a missing host dep (`llvm-dev`, `libdrm-dev`, XCB headers) or `mesa_clc` built from an unpatched tree |
| meson hangs downloading `expat.wrap` | install `libexpat1-dev` (or keep `work/android-deps/lib/pkgconfig/expat.pc`) |
| `PATCH-DRIFT` from `apply-patches.sh` | the mesa checkout is not the pinned commit, or was modified; run `./scripts/fetch-mesa.sh` again |
| `Program 'mesa_clc' not found` | run `./scripts/bootstrap-host-tools.sh --mesa <tree> --out build/host-tools` first |
| queue bind failure on old kernels | see `perf/003` in §7 |
| no logs on device | `MESA_LOG_FILE=/data/local/tmp/mesa.log` (logd rate-limits) |
