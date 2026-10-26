# 补丁系列说明 / Patch series notes

本仓库的 Mesa 源码树 = 基座提交 `5a07217f034b3e50d8c7c7794f97a2df1742613b`
(26.3.0-devel) + **222 个补丁**。补丁已在当前树中全部应用，`patches/` 目录保留完整
系列，用于复现、审阅和回退。

应用顺序由 `scripts/apply-patches.sh` 固定：

```
common → android → kbase-common → app-loader → wsi → csf → csf-v11 → jm-v9 → jm-v7 → perf → local
```

* 每个家族的 `*.patch` 按文件名顺序应用；`git apply --check` 失败时依次回退到
  `--recount`、`-C1`（一行上下文），仍失败则 `PATCH-DRIFT` 硬失败退出。
* 家族目录下若存在 `files/`，该家族的补丁应用完后会整目录覆盖到 Mesa 树
  （`app-loader`、`kbase-common` 使用这种方式替换基线文件）。
* 正常输出为 `OK applied=222 profile=g615-v11-csf`。

补丁头里的 `biblioklept` 标记表示该改动由 AI 辅助生成（遵循上游 Mesa `AGENTS.md`
的标注约定），`Generated-by: LLM` 同理。

---

## 1. common（1）

| 补丁 | 作用 |
| --- | --- |
| `019-nir-load-attr-pan-io-offset` | NIR 中 `load_attr_pan` 的 I/O 偏移是第 3 个源操作数（src 2），修正索引，避免属性读取偏移错位导致顶点属性取错数据。 |

## 2. android（4）

Android 图形栈接入层：让 PanVK 在 Android 上拿到窗口缓冲和 gralloc 元数据。

| 补丁 | 作用 |
| --- | --- |
| `008-ahb-dma-buf-finder` | AHB（`AHardwareBuffer`）到 dma-buf fd 的健壮查找与 `allocationSize` 查询，兼容只有 `AIMapper` 或只有 HIDL mapper 的设备。 |
| `013-vendor-mapper-metadata` | gralloc 供应商 mapper 元数据读取，取不到时直接拒绝猜测，避免把错误的 stride/modifier 带进驱动。 |
| `014-vendor-neutral-gralloc-mapper-and-hidl-mapper4` | 与厂商无关的 mapper 发现流程 + HIDL mapper 4.0 后端。 |
| `015-android-stub-static-hardware` | Android stub 的静态 hardware 链接修正，保证 `android-stub=true` 构建能解析 `AHardwareBuffer` 符号。 |

## 3. kbase-common（10）

把 PanVK 从 DRM/panthor 改成走 **kbase**（`/dev/mali0`）的基础设施，是整个移植的地基。

| 补丁 | 作用 |
| --- | --- |
| `001-meson-kmds-option` | 新增 `panfrost-kmds` 构建选项，允许选择 `panthor` / `kbase` 后端。 |
| `002-kmod-build-kbase` | 选中 kbase 时把 `kbase_kmod.c` 编入 kmod 层。 |
| `003-pan-kmod-backend-hooks` | `pan_kmod` 增加后端 ops 钩子，让非 DRM 后端（kbase）挂接 BO 分配/映射/提交。 |
| `004-physical-device-kbase` | `vkCreateInstance`/物理设备初始化走 kbase 路径（`/dev/mali0` 探测、GPU 属性读取）。 |
| `005-instance-enumerate` | 枚举 `/dev/mali*` kbase 节点。 |
| `006-device-queue-wsi-kbase` | kbase 的设备/队列/WSI 集成。 |
| `010-bo-munmap-hook` | BO 解除映射通过后端钩子完成（kbase 没有 DRM mmap 语义）。 |
| `012-device-init-csif` | 设备初始化时按 kbase 情况计算 CSIF。 |
| `014-fd-props-no-import` | 查询 fd 属性时不触发 GPU 导入。 |
| `017-disable-unvalidated-extensions` | 隐藏未经真机验证的扩展，避免应用误用。 |

## 4. app-loader（1）

| 补丁 | 作用 |
| --- | --- |
| `001-android-icd-exports` | 在导出 HMI 符号的同时导出 Vulkan ICD 符号，使 so 能被 Android Vulkan loader 当作 ICD 热加载。 |

## 5. wsi（3）

Android 窗口系统集成层：`VK_KHR_android_surface` + AHB 交换链。

| 补丁 | 作用 |
| --- | --- |
| `016-android-surface-swapchain` | 引入 `VK_KHR_android_surface` 与基于 AHB 的交换链实现。 |
| `017-android-swapchain-owned-ahb` | 标记交换链自带的 AHB 为驱动所有，避免应用侧提前回收。 |
| **`018-android-present-fast-copy`（自研）** | 裁剪 Android WSI 呈现路径：present 时直接做一次拷贝，去掉多余的中转与同步，降低每帧呈现的 CPU 开销。该项与 perf 系列同属自研优化，也是 XenDroid+ 黑屏问题排查（`local/002`、`local/003`）所在链路的相邻改动。 |

## 6. csf（2）

CSF 前端接入：把 CSF 的队列提交与 csif 调用点切换到 kbase 助手。

| 补丁 | 作用 |
| --- | --- |
| `007-gpu-queue-kbase` | kbase 的 CSF 队列提交通路（queue group 创建、ring buffer、kick）。 |
| `009-csif-helper-callsites` | csif/flush-id 调用点统一改走 kbase 助手函数。 |

## 7. csf-v11（155）

面向 **v10–v14 CSF** 的功能与稳定性主体，也是数量最大的一族。按主题分组：

* **架构/构建**：`027` v11 架构 switch 补全、`083` Android X11 WSI 改为 `dlopen`
  X11/XCB、`099` driverInfo 中报告 PanVK-kbase 发行版本、`118` INFO 级日志输出设备决策。
* **图形特性补齐**（v10+ 大量扩展）：`028/029` incremental present 与
  swapchain colorspace、`030–037` AFBC/modifier/图像压缩相关、
  `042–048` 几何着色器 + gpu_prerast、`049–054` 管线统计查询、
  `065–068` 曲面细分与 transform feedback、`069–078` 内存优先级/多绘制/
  图元生成查询/顶点阶段存储、`087–097` 条件渲染、深度边界仿真、
  大绘制分块、次命令缓冲、`109–121` 视口/IDVS/深度细节。
* **kbase 生命周期关键补丁**：`041` 分配时清零 BO、
  `043/098/102/104/107` tiler heap 的记账、续期、预创建与退休堆管理、
  `075/077` sync_file 围栏与系统级同步域、
  `091` 从 dma-heap 分配可放置主机内存、`100` gpu_prerast arena 提前提交、
  `101/105` CSF trace buffer 映射与复位。
* **BC 转码（BCn → ASTC/ETC2/EAC）**：`022/039/040/108` 基础路径与默认开关，
  `124–130`、`172–176`、`179/180`、`212–219` 为 1.1.3/1.1.4/1.1.5-FC 系列的性能
  与正确性迭代（BC1–5 → ASTC 直映射、BC7/BC6H 单块解码、BC2/3 → AFRC 影子、
  BC4/5 → EAC、BC1 → ETC2 等），默认开启，可用 `PANVK_BC_ETC=0`、
  `PANVK_BC_EAC=0` 单独关闭。
* **DX12/vkd3d**：`123` vkd3d DX12 essentials、`125` descriptor buffer 内嵌不可变
  采样器、`175` `vkGetDescriptorEXT` 写结构化 texel buffer 描述符。
* **寄存器分配与编译期优化（自研 `biblioklept` 组，152–164）**：只遍历活跃节点做
  LCRA 干涉、每轮为所有失败节点溢出、SSA 溢出目标预留空间、约束集一次构建两套
  寄存器文件、溢出后增量更新约束、平坦定义表替代每块哈希表、跳过无法推进的优化
  循环轮次、约束行稀疏化。目标是缩短着色器编译时间。
* **渲染通道合并与屏障等待（自研 188–210）**：`197` 悬挂遮挡查询链清理、
  `199/200/201` 连续兼容 render pass 合并及调试计数、`204/205/208` 延迟
  顶点/tiler/计算等待、`209` 对 DXVK 也套用翻译层写规则、`210` 默认开启惰性
  片段等待与 render pass 合并。配套 `198/202/203/206/207` 为带
  `PANVK_PASS_STATS` / `PANVK_RP_TRACE` / `PANVK_BAR_TRACE` / `PANVK_DROP_WAITS`
  开关的调试探针。
* **kbase 稳定性**：`186` MTK SLC 元数据查询的 ENOTTY 重试（对应上游
  `099dd7cb` 的 gralloc 元数据回退，但仅在 `ENOTTY` 触发，对
  `KBASE_IOCTL_MEM_IMPORT` 返回 `ENOMEM` 的 AFBC 场景无效）、
  `183` tiler heap 改为 8 MiB 块、`188` 空 polygon list 保护。

## 8. jm-v9（28）

**v9（Valhall JM，非 CSF）** 后端与通用 ICD 支持。`001` 建立 kbase JM job atom
提交通路，`002` 引入 v9 JM 后端，其余按能力补齐：

* 自研/移植能力：`003` v9 multiview 与分层渲染并报告 Vulkan 1.3、
  `004/005` EXEC_VA/JIT 区域在 JIT 前初始化并按内核页计算大小、
  `007` 命令缓冲重提时复位 MALLOC_VERTEX 顶点数组、
  `008` `vkCmdSetEvent` 在 GPU 完成后生效、
  `009` multiDrawIndirect / drawIndirectCount / gl_DrawID、
  `011–016` transform feedback、几何着色器、clip/cull distance、
  曲面细分、多视口、管线统计与 XFB 查询全部走 gpu_prerast、
  `017/028` 间接与大绘制分块、`018` 深度边界/逐采样着色/VMR、
  `019` fillModeNonSolid、`020` 分块截断告警一次、
  `022` JM atom 布局探测与故障传播、`023` tiler heap 按 2 MiB 步进提交并
  为 overlap 两个半区各用一份堆、`024` 非一致 dma-buf 导入提交前 clean、
  `026` 实例属性补 firstInstance、`027` 空着色器跳过前统计 invocation、
  `029` primitive index 使能与覆盖。
* 机型表：`006` 增加 Mali-G77、`021` 增加 G78/G78AE。

## 9. jm-v7（5）

**v6/v7（Bifrost JM）** 实验性支持，用于让老 Bifrost 设备能跑起来：

| 补丁 | 作用 |
| --- | --- |
| `001` | `pan_get_model` 在 Bifrost 上忽略 GPU variant（避免匹配失败）。 |
| `002` | 去掉 `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER` 环境变量门槛，允许 Bifrost 在 kbase 上启动。 |
| `003` | 跳过 Bifrost JM 的 `MEM_JIT_INIT`，tiler heap 用 GROW_ON_GPF 以 1 MiB 步进提交。 |
| `004` | Bifrost 的 JM atom 步长探测，增加 64 字节（seq_nr）候选。 |
| `005` | 可放置映射 dma-buf 失败时回退到 kbase ION 路径。 |

> v6/v7/v9 从未真机验证，属于实验性路径。

## 10. perf（8）— 全部自研

面向 CPU 侧提交开销的性能补丁，全部不改变 GPU 侧语义。

| 补丁 | 作用 |
| --- | --- |
| `001-csf-merge-cache-flush` | 同一 subqueue 同一批次只发一次 cache clean，合并重复的缓存刷新。 |
| `002-descriptor-bind-noop` | 重复绑定的描述符集不再触发 `DESC_STATE` 失效（去重 set bind）。 |
| `003-ringbuf-256k` | kbase CSF ring buffer 由 64 KB 提升到 256 KB，避免一个 pass 的命令流写满 ring 后 CPU 在中途阻塞等 GPU seqno。**老内核若拒绝该大小会在 `CS_QUEUE_REGISTER` 失败 → 回退本补丁单独重建即可。** |
| `004-kbase-import-log-gate` | 逐次 dma-buf 导入日志改为 `PANVK_KBASE_DEBUG_IMPORT=1` 才输出，去掉稳定路径上的格式化开销。 |
| `005-kbase-bo-wrapper-cache` | 缓存 `kbase_kmod_bo` 包装结构体（每设备上限若干个），消除每个 BO 一次分配器往返；GPU 内存本身仍由内核分配/释放。 |
| `006-async-tiler-heap-destroy` | 退休 tiler heap 的 `CS_TILER_HEAP_TERM`（单次约 3.2 ms）移出提交线程，由 heap worker 经 SPSC 环形队列异步执行，seqno 校验仍在提交线程保证槽位复用安全。 |
| `007-cmd-end-skip-empty-finish-cs` | `EndCommandBuffer` 对空 builder 跳过 `finish_cs`，省掉无效的 CS 等待、错误检查与 cache clean。 |
| `008-pool-flush-written-prefix` | 池 BO 记录写入高水位，只 flush 已写入的前缀而不是整块 slab（实测 5.6–7.3 µs → 1–2 µs）。 |

## 11. local（5）— 全部自研

本机（MT6897 / MT6985 + Android 15）实测暴露出来、又不适合放进通用系列的改动。

| 补丁 | 作用 |
| --- | --- |
| `001-bc-decode-chunked-dispatch` | BC 解码由“整块一次 compute dispatch”改为按 `PANVK_BC_CHUNK`（默认 256×256 纹素）分块循环。超大单 job（>100 万 invocation）会触发 Mali CSF 组超时被固件终止，这是 D3D11/魔方测试里 CS 组 fatal（0x72/0x41）的主要诱因；`PANVK_BC_TRACE` 用于调试输出。 |
| `002-tolerate-unseekable-dmabuf` | `pan_kmod.c` 中 `lseek` 探测返回 `-1/0` 时按 `size=0` 继续，而不是直接报错——MTK AFBC gralloc dma-buf 不实现 `llseek`。 |
| `003-skip-lseek-fd-probe` | `GetMemoryFdPropertiesKHR` 去掉 `lseek(fd,0,SEEK_END)` 有效性探测，只保留 `fd < 0` 检查，避免窗口缓冲导入被误杀导致交换链永不上屏。 |
| `004-kbase-page-size-helper` | 重新引入 `kbase_kmod_page_size()`（`sysconf(_SC_PAGESIZE)`），修复 jm-v9/023 等补丁引用该助手而参考基线已删除所导致的编译失败。 |
| `005-driver-identity` | `VK_KHR_driver_properties` 上报的身份从 `panvk` / `PanVK-kbase beta.18-rc2 (Mesa 26.3.0-devel …)` 改为 `Panvk` / `Mesa 26.3.0-devel`，即 DXVK HUD、DXVKHUB 里显示的驱动名与版本。仅改字符串，不影响 GPU 行为。 |

---

## 12. 与参考仓库同步时移除的补丁

同步目标是 `GunaCharanTeja/panvk-kbase-android`（CSF/v11 分支）的整体补丁系列，
自研补丁全部保留。以下补丁因参考仓库已用其它方式实现或属于 v9/v10 专属而删除
（备份于 `/root/panvk-build/patches.bak-sync`）：

| 补丁 | 移除原因 |
| --- | --- |
| `csf-v11/110`（16K 页 + 布局选择） | 布局选择部分已被参考仓库的 kbase 布局实现覆盖。 |
| `csf-v11/122`、`181`、`184`（v10 robust*） | v10 专属，参考仓库已改同文件；对本机 v11 无影响。 |
| `jm-v9/010`、`jm-v9/030` | v9 专属（robustness2 / 条件渲染 / 时间戳），参考仓库已改同文件。 |

另有 3 个补丁（`jm-v9/023`、`jm-v7/003`、`perf/005`）因参考仓库的
`kbase-common/files` 基线不同而无法直接 `git apply`，已用 `patch -F3` 模糊应用后
基于快照 diff 重新生成，语义不变。

---

## 13. 附录：完整补丁清单（222）

见下方按家族列出的 `文件名 | 上游 subject` 全表。
### common (1)

| Patch | Subject |
| --- | --- |
| 019-nir-load-attr-pan-io-offset.patch | nir: load_attr_pan io offset is src 2 |

### android (4)

| Patch | Subject |
| --- | --- |
| 008-ahb-dma-buf-finder.patch | panvk: robust dma-buf fd discovery and allocationSize query for AHB |
| 013-vendor-mapper-metadata.patch | gralloc: vendor mapper metadata, refuse guesses |
| 014-vendor-neutral-gralloc-mapper-and-hidl-mapper4.patch | gralloc: vendor-neutral mapper discovery, HIDL mapper4 backend |
| 015-android-stub-static-hardware.patch | (no subject) |

### kbase-common (10)

| Patch | Subject |
| --- | --- |
| 001-meson-kmds-option.patch | build: add panfrost-kmds option and |
| 002-kmod-build-kbase.patch | kmod: compile kbase_kmod.c when kbase KMD |
| 003-pan-kmod-backend-hooks.patch | kmod: backend ops hooks for non-DRM backends |
| 004-physical-device-kbase.patch | panvk: kbase physical-device init path |
| 005-instance-enumerate.patch | panvk: enumerate /dev/mali* kbase nodes |
| 006-device-queue-wsi-kbase.patch | panvk: kbase queue/device/WSI integration |
| 010-bo-munmap-hook.patch | panvk: unmap BOs via backend hook |
| 012-device-init-csif.patch | panvk: kbase-aware csif in device init |
| 014-fd-props-no-import.patch | panvk: query fd properties without GPU import |
| 017-disable-unvalidated-extensions.patch | panvk: hide unvalidated extensions |

### app-loader (1)

| Patch | Subject |
| --- | --- |
| 001-android-icd-exports.patch | android: export ICD symbols alongside HMI |

### wsi (3)

| Patch | Subject |
| --- | --- |
| 016-android-surface-swapchain.patch | panvk: Android VK_KHR_android_surface + AHB swapchain |
| 017-android-swapchain-owned-ahb.patch | panvk: mark Android WSI swapchain AHBs as driver-owned |
| 018-android-present-fast-copy.patch | panvk: trim the Android WSI present path |

### csf (2)

| Patch | Subject |
| --- | --- |
| 007-gpu-queue-kbase.patch | panvk: kbase CSF queue submit path |
| 009-csif-helper-callsites.patch | panvk: route csif/flush-id sites via kbase helpers |

### csf-v11 (155)

| Patch | Subject |
| --- | --- |
| 018-gpu-prerast-contracts.patch | panvk: GPU prerast VS records to IDVS/FS slice |
| 019-crc-invalidate-undefined-clear.patch | panvk: invalidate CRC on CLEAR/DONT_CARE |
| 020-gpu-prerast-tls-io-barrier.patch | panvk: gpu_prerast TLS descriptor, record IO, compute barrier |
| 021-gpu-prerast-variant-gate-passthrough-base.patch | panvk: gate gpu_prerast VS variants, fix passthrough output base |
| 022-bc-gpu-decode-emulation.patch | panvk: BC1-BC7 via GPU compute decode (PANVK_DEBUG=bc_emul) |
| 023-clip-cull-distance-fs-discard.patch | panvk: shaderClipDistance/shaderCullDistance via FS discard |
| 024-multi-viewport-index0.patch | panvk: expose multiViewport (maxViewports = 16) |
| 025-fill-mode-non-solid-gpu-polygon.patch | panvk: fillModeNonSolid via GPU polygon kernel on gpu_prerast |
| 026-fau-flush-before-indirect-draw.patch | panvk/csf: flush the FAU stores before indirect draws read them |
| 027-v11-arch-switch-cases.patch | panfrost: add the missing v11 cases to the arch switches |
| 028-khr-incremental-present.patch | panvk: Advertise VK_KHR_incremental_present |
| 029-ext-swapchain-colorspace.patch | panvk: Advertise VK_EXT_swapchain_colorspace |
| 030-afbc-x-component-formats.patch | pan/afbc: Support X-component format variants in AFBC and blend tables |
| 031-mod-layout-caps-query.patch | pan/mod: Introduce modifier layout capabilities query |
| 032-mod-caps-format-props.patch | panvk: Query modifier capabilities for format property advertisement |
| 033-afbc-wsi-default.patch | panvk: Enable AFBC for WSI images by default |
| 034-zero-query-images.patch | panvk: Zero images built for device-level queries |
| 035-image-compression-props.patch | panvk: Report image compression properties |
| 036-honor-image-compression-control.patch | panvk: Honor VkImageCompressionControlEXT |
| 037-ext-image-compression-control.patch | panvk: Advertise VK_EXT_image_compression_control |
| 038-crc-init-kmod-munmap.patch | panvk: unmap CRC init mapping through pan_kmod_bo_munmap |
| 039-bc-gpu-decode-default.patch | panvk: BC GPU decode on by default when HW lacks BC (no_bc_emul opts out) |
| 040-bc-zero-initialized-decode.patch | panvk: decode BC emulated images leaving ZERO_INITIALIZED layout |
| 041-kbase-zero-bo-pages.patch | panfrost/kmod: zero BO pages at allocation |
| 042-geometry-shaders-gpu-prerast.patch | panvk: geometry shaders on gpu_prerast |
| 043-kbase-tiler-heap-renewal.patch | panvk/csf: account tiler work for heap renewal |
| 044-gpu-prerast-vs-const.patch | panvk: fix gpu_prerast VS pointer qualifier |
| 045-gpu-prerast-compute-consumer.patch | panvk: order compute after gpu_prerast |
| 046-gpu-prerast-split-draws.patch | panvk: handle GS split and indirect draws |
| 047-gpu-prerast-nonfill.patch | panvk: route non-fill polygon draws through GPU prerast |
| 048-geometry-default-on.patch | panvk: enable geometry shaders by default |
| 049-query-result-blocking-wait.patch | panvk: block instead of busy-spinning for query results |
| 050-implement-pipeline-statistics-queries-on-CSF.patch | panvk: implement pipeline statistics queries on CSF |
| 051-count-GS-and-clipping-statistics-in-gpu_prerast.patch | panvk: count GS and clipping statistics in gpu_prerast |
| 052-expose-pipelineStatisticsQuery-on-v10plus.patch | panvk: expose pipelineStatisticsQuery on v10+ |
| 053-synchronize-and-read-complete-pipeline-statistics-reports.patch | panvk: synchronize and read complete pipeline statistics reports |
| 054-order-pipeline-statistics-copy-before-reset.patch | panvk: order pipeline statistics copy before reset |
| 055-use-2d-dimension-for-layered-cube-attachment-and-storage.patch | panvk: use 2D dimension for layered cube attachment and storage views |
| 056-cache-the-memory-budget-query-for-100-ms.patch | panvk: cache the memory budget query for 100 ms |
| 057-report-the-timestamp-frequency-from-cntfrq.patch | panfrost/kbase: report the timestamp frequency from cntfrq |
| 058-skip-texture-cache-invalidation-for-non-texel-buffer.patch | panvk/csf: skip texture cache invalidation for non-texel buffer barriers |
| 059-add-opt-in-gpu-waits-for-same-queue-kbase-semaphores.patch | panvk/csf: add opt-in GPU waits for same-queue kbase semaphores |
| 060-lower-minstoragebufferoffsetalignment-to-4-on-v9.patch | panvk: lower minStorageBufferOffsetAlignment to 4 on v9+ |
| 061-use-intersect-zs-preload-on-v11.patch | panvk: use INTERSECT ZS preload on v11 |
| 062-add-a-robust-ssbo-load-vectorizer-behind-panvk-debug-robust.patch | nir,panvk: add a robust SSBO load vectorizer behind PANVK_DEBUG=robust_ssbo_vec |
| 063-allow-robust-merges-that-cannot-overflow.patch | nir/opt_load_store_vectorize: allow robust merges that cannot overflow |
| 064-select-all-layers-of-full-tiler-descriptors-in-dynamic.patch | panvk/csf: select all layers of full tiler descriptors in dynamic fullscreen runs |
| 065-implement-tessellation-on-the-gpu-prerast-path.patch | panvk: implement tessellation on the gpu_prerast path |
| 066-implement-transform-feedback-on-the-gpu-prerast-path.patch | panvk: implement transform feedback on the gpu_prerast path |
| 067-pad-the-tiler-geometry-buffer-allocation-by-one-page.patch | panvk/csf: pad the tiler geometry buffer allocation by one page |
| 068-expose-vk-ext-transform-feedback-on-v10.patch | panvk: expose VK_EXT_transform_feedback on v10+ |
| 069-expose-vk-ext-memory-priority-and-pageable-device-local-memory.patch | panvk: expose VK_EXT_memory_priority and VK_EXT_pageable_device_local_memory on v10+ |
| 070-expose-alphatoone-on-v10.patch | panvk: expose alphaToOne on v10+ |
| 071-raise-maxgeometryshaderinvocations-to-64.patch | panvk: raise maxGeometryShaderInvocations to 64 on v10+ |
| 072-implement-vk-ext-multi-draw-on-v10.patch | panvk: implement VK_EXT_multi_draw on v10+ |
| 073-implement-vk-ext-primitives-generated-query-on-v10.patch | panvk: implement VK_EXT_primitives_generated_query on v10+ |
| 074-expose-variablemultisamplerate-on-v10.patch | panvk: expose variableMultisampleRate on v10+ |
| 075-export-kbase-sync-file-fences-through-a-kcpu-queue.patch | panvk: export kbase sync_file fences through a KCPU queue |
| 076-honour-the-geometry-shader-viewport-index.patch | panvk: honour the geometry shader viewport index on v10+ |
| 077-use-system-scope-for-subqueue-sync-signals-on-kbase.patch | panvk: use system scope for subqueue sync signals on kbase |
| 078-expose-vertexpipelinestoresandatomics-on-v10-v12-via-gpu-prerast.patch | panvk: expose vertexPipelineStoresAndAtomics on v10-v12 via gpu_prerast |
| 079-emit-gl-pointsize-for-point-mode-tes.patch | panvk: emit a real gl_PointSize output for point-mode TES before poly lowering |
| 080-allocate-the-gpu-prerast-arena-for-vertexpipelinestoresandatomics.patch | panvk: allocate the gpu_prerast arena when vertexPipelineStoresAndAtomics is enabled |
| 081-derive-idvs-flags-from-the-bound-vs-variant.patch | panvk: derive IDVS flags from the bound VS variant for gpu_prerast draws |
| 082-keep-gpu-prerast-cross-subqueue-syncs-in-csf-event-memory.patch | panvk: keep gpu_prerast cross-subqueue syncs in CSF event memory |
| 083-dlopen-the-x11-xcb-libraries-for-the-android-x11-wsi.patch | build: dlopen the X11/XCB libraries for the Android X11 WSI |
| 084-complete-present-ids-on-the-x11-software-present-path.patch | panvk: complete present ids on the X11 software present path |
| 085-keep-descriptor-ring-and-vkevent-syncs-in-csf-event-memory.patch | panvk: keep the descriptor ring and VkEvent syncs in CSF event memory |
| 087-honour-conditional-rendering-in-the-tessellation-compute-loop.patch | panvk: honour conditional rendering in the tessellation compute loop |
| 088-use-tes-primitive-id-in-tess-fed-geometry-shaders.patch | panvk: use TES primitive ID in tess-fed geometry shaders |
| 089-draw-gs-viewport-selections-as-ordered-runs.patch | panvk: draw GS viewport selections as ordered runs |
| 090-select-viewports-for-vs-and-tes-viewportindex-writes.patch | panvk: select viewports for VS and TES ViewportIndex writes |
| 091-allocate-placeable-host-memory-from-the-dma-heap.patch | panvk: allocate placeable host memory from the dma-heap |
| 092-emulate-the-depth-bounds-test-in-the-fragment-shader.patch | panvk: emulate the depth bounds test in the fragment shader |
| 093-do-not-predicate-tessellation-state-emission-on-conditional-rendering.patch | panvk: do not predicate tessellation state emission on |
| 094-chunk-large-and-indirect-gpu_prerast-draws-on-the-gpu.patch | panvk: chunk large and indirect gpu_prerast draws on the |
| 095-give-attachment-less-secondaries-a-context-with-their-sample-count.patch | panvk: give attachment-less secondaries a context with |
| 096-plan-restart-strip-chunks-with-the-whole-workgroup.patch | panvk: plan restart strip chunks with the whole workgroup |
| 097-share-tls-and-fault-back-gpu-prerast-arenas.patch | panvk: share TLS across command buffers and fault-back gpu_prerast arenas |
| 098-emit-only-vertex-tiler-started-heap-ops-on-kbase.patch | panvk: emit only VERTEX_TILER_STARTED heap ops on kbase |
| 099-report-the-panvk-kbase-release-in-driverinfo.patch | panvk: report the PanVK-kbase release in driverInfo |
| 100-commit-gpu-prerast-arenas-up-front-on-kbase.patch | panvk: commit gpu_prerast arenas up front on kbase |
| 101-map-the-csf-tracebuf-at-a-kbase-assigned-va.patch | panvk: map the CSF trace buffer at a kbase-assigned VA |
| 102-renew-the-kbase-tiler-heap-without-a-graphics-drain.patch | panvk: renew the kbase tiler heap without a graphics drain |
| 103-wait-for-same-queue-kbase-semaphores-on-the-gpu-by-default.patch | panvk: wait for same-queue kbase semaphores on the GPU by default |
| 104-pre-create-the-next-kbase-tiler-heap-on-a-worker-thread.patch | panvk: pre-create the next kbase tiler heap on a worker thread |
| 105-reset-the-cs-trace-buffers-before-each-kbase-submit.patch | panvk: reset the CS trace buffers before each kbase submit |
| 106-wait-for-sw-present-fences-on-the-x11-present-thread.patch | wsi/x11: wait for sw present fences on the present thread |
| 107-keep-up-to-three-retired-kbase-tiler-heaps-in-flight.patch | panvk: keep up to three retired kbase tiler heaps in flight |
| 108-emulate-bc-unless-every-bc-format-is-native.patch | panvk: emulate BC unless every BC format is native |
| 109-port-viewport-depth-runs-to-v12-and-lift-gates.patch | panvk: port viewport depth runs to v12+ and lift |
| 111-never-return-device-lost-from-set-reset-event.patch | panvk: never return VK_ERROR_DEVICE_LOST from |
| 112-split-prerast-fans-cap-instance-grids-skip-conditional-chunks.patch | panvk: split prerast triangle fans, cap fan/tess instance |
| 114-cut-attachment-less-secondaries-per-sample-count.patch | panvk: cut attachment-less secondaries per sample count, end |
| 115-keep-the-fs-primitive-id-across-viewport-runs.patch | panvk: keep the FS primitive ID across VS viewport runs |
| 116-keep-fpk-and-early-zs-while-depth-bounds-is-off.patch | panvk: update ZS after the FS for EarlyFragmentTests with |
| 117-x11-sw-present-via-mit-shm-attach-fd-and-fifo-pacing.patch | wsi/x11: sw present via MIT-SHM AttachFd, pace sw FIFO |
| 118-log-kbase-and-device-decisions.patch | panvk/kbase: log device decisions at INFO for tester uploads |
| 119-enable-vertex-pipeline-stores-on-v13-v14.patch | panvk: enable vertexPipelineStoresAndAtomics on v13 and v14 |
| 120-build-a-strip-table-for-gpu-prerast-gs-restart-draws.patch | panvk: build a strip table for gpu_prerast GS restart draws |
| 121-keep-the-depth-flat-for-zero-depth-range-viewports.patch | panvk: keep the depth flat for minDepth == maxDepth viewports |
| 123-vkd3d-dx12-essentials.patch | (no subject) |
| 124-panvk-astc-bcn-transcode.patch | panvk: transcode BC1-BC7 textures to ASTC 4x4 |
| 125-bind-descriptor-buffer-embedded-samplers.patch | panvk: bind descriptor-buffer embedded immutable samplers |
| 126-faster-bc1-bc5-to-astc-transcode.patch | panvk: map BC1-BC5 blocks to ASTC directly, per-format pipelines |
| 127-decode-bc7-once-per-block-for-astc.patch | panvk: decode BC7 once per block for the ASTC transcode |
| 128-decode-bc6h-once-per-block-for-astc.patch | panvk: decode BC6H once per block for the ASTC transcode |
| 129-faster-bc1-alpha-bc2-bc5-to-astc.patch | panvk: faster BC1A/BC2/BC3/BC4/BC5 to ASTC transcode |
| 130-cache-bc7-texels-for-astc.patch | panvk: cache decoded BC7 texels, refine single-subset modes directly |
| 130-run-v13-vertex-stores-through-gpu-prerast.patch | panvk: run v13 vertex stores through gpu_prerast biblioklept |
| 131-name-immortalis-and-small-core-gpus-by-core-count.patch | panvk: name Immortalis and small GPUs by core count biblioklept |
| 132-wait-for-the-prerast-producer-before-viewport-runs.patch | panvk: wait for the prerast producer before viewport runs biblioklept |
| 140-add-the-mali-g710-model-row.patch | panfrost: add the Mali-G710 model row biblioklept |
| 152-ra-interference-over-live-nodes-and-one-pass-solve.patch | pan/bi: walk only live nodes in LCRA interference and solve each node in one pass biblioklept |
| 153-ra-spill-for-every-failing-node-per-pass.patch | pan/bi: spill for every node that fails in an LCRA pass, not just the first biblioklept |
| 154-ra-leave-room-for-spill-reservation-in-ssa-spill-target.patch | pan/bi: leave room for the spill reservation in the SSA spill target biblioklept |
| 160-ra-build-lcra-constraints-once-per-pass-cheaply.patch | pan/bi: build LCRA constraints once for both register file sizes, with global-only liveness and a sorted queue biblioklept |
| 161-ra-update-lcra-constraints-after-spilling.patch | pan/bi: update LCRA constraints after spilling instead of rebuilding them, spill in one scan biblioklept |
| 162-repair-ssa-flat-definition-map.patch | pan/bi: repair SSA with one flat definition map instead of a hash table per block biblioklept |
| 163-nir-skip-opt-loop-passes-that-cannot-progress.patch | pan/bi: skip optimization loop passes that cannot make progress biblioklept |
| 164-ra-keep-lcra-rows-sparse-up-to-a-quarter-of-nodes.patch | pan/bi: keep LCRA constraint rows sparse up to a quarter of the nodes biblioklept |
| 170-keep-the-shader-disk-cache-on-by-default-on-android.patch | util/disk_cache: keep the shader disk cache on by default on Android biblioklept |
| 171-look-each-android-property-option-up-once.patch | util/os_misc: look each Android property option up once biblioklept |
| 172-decode-bc4-bc5-unorm-exactly-to-r8-rg8.patch | panvk: decode BC4/BC5 UNORM exactly to R8/RG8 |
| 173-cache-bc6h-lns-texels-for-astc.patch | panvk: convert BC6H texels to LNS once per block |
| 174-decode-bc2-bc3-into-afrc-shadows.patch | panvk: decode BC2/BC3 into AFRC-compressed RGBA8 shadows |
| 175-write-structured-texel-buffer-descriptors.patch | panvk: write structured texel-buffer descriptors in vkGetDescriptorEXT |
| 176-tile-decoded-bcn-planes.patch | panvk: lay decoded BCn planes out U-interleaved |
| 179-faster-bc1-alpha-and-bc7.patch | panvk: drop the single-plane retry for BC1 punch-through and BC7 |
| 180-drop-raw-bc-plane.patch | panvk: drop the raw BC plane of emulated BCn images where possible |
| 183-tiler-chunk-8mib.patch | panvk/kbase: 8 MiB tiler heap chunks with byte-based heap limits |
| 185-hide-present-timing.patch | panvk: hide VK_EXT_present_timing unless PANVK_PRESENT_TIMING=1 |
| 188-empty-tiler-null-polygon-list.patch | panvk/csf: don't walk a NULL polygon list when no draw reached the tiler |
| 197-drain-oq-chain-after-suspend.patch | panvk/csf: drain occlusion query chain left by suspended render passes |
| 198-debug-pass-stats.patch | panvk/csf: debug PANVK_PASS_STATS per-second render pass summary |
| 199-render-pass-merge.patch | panvk/csf: merge consecutive compatible render passes (PANVK_RP_MERGE) |
| 200-debug-rp-merge-stats.patch | panvk/csf: debug render pass merge reason counters |
| 201-debug-rp-trace.patch | panvk/csf: debug PANVK_RP_TRACE pass sequence trace |
| 202-debug-barrier-stats.patch | panvk/csf: debug barrier wait-edge stats |
| 203-debug-drop-waits.patch | panvk/csf: debug PANVK_DROP_WAITS toggle |
| 204-lazy-vt-wait.patch | panvk/csf: defer vertex/tiler waits on fragment work |
| 205-cond-render-no-frag-wait.patch | panvk/csf: no fragment waits for the conditional rendering stage |
| 206-debug-bar-trace.patch | panvk/csf: debug PANVK_BAR_TRACE barrier edge trace |
| 207-debug-dispatch-stats.patch | panvk/csf: debug shader writes_memory flag and dispatch stats |
| 208-lazy-compute-wait.patch | panvk/csf: defer compute waits on fragment work too |
| 209-d3d-write-rule-dxvk.patch | panvk/csf: apply the translation-layer write rule to DXVK too |
| 210-enable-by-default.patch | panvk/csf: enable lazy fragment waits and render pass merging by default |
| 212-bc-astc-narrow-decode.patch | panvk: decode transcoded BC ASTC planes at 8 bits per channel |
| 213-bc45-eac.patch | panvk: transcode BC4/BC5 to EAC R11/RG11 |
| 214-bc1-etc2.patch | panvk: transcode BC1 to ETC2 RGB8/RGB8A1 |
| 215-bc23-etc2.patch | panvk: transcode BC2/BC3 to ETC2 RGBA8, faster BC1 -> ETC2 |
| 216-bc7-fast.patch | panvk: faster BC7 -> ASTC transcode |
| 217-bc6h-fast.patch | panvk: faster BC6H -> ASTC HDR transcode |
| 218-bc6h-prerefit.patch | panvk: least-squares pre-refit in the BC6H transcode |
| 219-eac-exact-ends.patch | panvk: keep exact 0/255 levels in the BC4/EAC table |

### jm-v9 (28)

| Patch | Subject |
| --- | --- |
| 001-kbase-jm-job-atom-submission.patch | kbase: JM job atom submission |
| 002-panvk-v9-valhall-jm-backend.patch | panvk: Valhall v9 JM backend |
| 003-report-vulkan-1.3-on-v9.patch | panvk/jm: v9 multiview and layered rendering, report Vulkan 1.3 biblioklept |
| 004-init-exec-va-before-jit.patch | kbase: init EXEC_VA before JIT, full 4G zone |
| 005-size-exec-jit-zones-and-imports-in-kernel-pages.patch | kbase: size the EXEC_VA/JIT zones and host imports in kernel |
| 006-add-the-mali-g77-model-row.patch | panfrost: add the Mali-G77 model row biblioklept |
| 007-v9-reset-malloc-vertex-array-on-resubmit.patch | panvk/jm: reset the MALLOC_VERTEX vertex array when a command buffer is resubmitted biblioklept |
| 008-jm-apply-cmd-set-event-on-gpu-completion.patch | panvk/jm: apply vkCmdSetEvent when the GPU work before it completes biblioklept |
| 009-v9-multi-draw-indirect-and-draw-indirect-count.patch | panvk/jm: v9 multiDrawIndirect, drawIndirectCount and gl_DrawID biblioklept |
| 011-v9-transform-feedback-via-gpu-prerast.patch | panvk: v9 transform feedback through the gpu_prerast path biblioklept |
| 012-v9-geometry-shaders-and-streams-via-gpu-prerast.patch | panvk: v9 geometry shaders and vertex streams through gpu_prerast biblioklept |
| 013-v9-clip-cull-distance-and-vertex-stores.patch | panvk: v9 clip/cull distances and vertex pipeline stores biblioklept |
| 014-v9-tessellation-via-gpu-prerast.patch | panvk: v9 tessellation via gpu_prerast biblioklept |
| 015-v9-multi-viewport-and-viewport-index.patch | panvk: v9 multiViewport and gl_ViewportIndex biblioklept |
| 016-v9-pipeline-statistics-and-xfb-queries.patch | panvk: v9 pipeline statistics, primitives generated and XFB queries biblioklept |
| 017-v9-split-large-indirect-gpu-prerast-draws.patch | panvk: v9 split indirect gpu_prerast draws over the arena biblioklept |
| 018-v9-depth-bounds-per-sample-shading-vmr.patch | panvk: v9 depth bounds emulation, per-sample shading, alpha-to-coverage and variableMultisampleRate biblioklept |
| 019-v9-fill-mode-non-solid.patch | panvk: v9 fillModeNonSolid via the gpu_prerast polygon-mode kernel biblioklept |
| 020-v9-warn-on-indirect-chunk-truncation.patch | panvk: v9 warn once when indirect prerast/tess draws exceed the chunk caps biblioklept |
| 021-add-the-mali-g78-g78ae-model-rows.patch | panfrost: add the Mali-G78 and G78AE model rows, match v9 ids by product biblioklept |
| 022-kbase-jm-atom-layout-fault-propagation-teardown.patch | panvk/kbase: JM atom layout probe, fault propagation within a submit, teardown wait biblioklept |
| 023-v9-tiler-heap-incremental-commit.patch | panvk/jm: v9 tiler heap committed in 2 MiB steps, one heap per overlap half biblioklept |
| 024-v9-clean-non-coherent-dmabuf-imports.patch | panvk/kbase: v9 clean non-coherent dma-buf imports before submit biblioklept |
| 025-v9-rt128-bisect-knob.patch | panvk: v9 PANVK_V9_RT128 bisect knob for 128-bit render targets biblioklept |
| 026-v9-instance-attribs-add-first-instance-for-every-divisor.patch | panvk: v9 instance-rate attributes add firstInstance for every divisor biblioklept |
| 027-v9-count-compute-invocations-before-empty-shader-skip.patch | panvk: v9 count compute invocations before the empty-shader skip biblioklept |
| 028-v9-chunk-direct-restart-strips-and-gpu-sized-tess-chunks.patch | panvk: v9 chunk direct restart strips, size tess chunks on the GPU biblioklept |
| 029-v9-primitive-index-enable-and-override.patch | panvk: v9 enable and override the primitive index when shaders use it biblioklept |

### jm-v7 (5)

| Patch | Subject |
| --- | --- |
| 001-ignore-the-gpu-variant-on-bifrost.patch | panfrost: ignore the GPU variant on Bifrost (v6/v7) in pan_get_model biblioklept |
| 002-allow-bifrost-on-kbase-without-the-env-gate.patch | panvk: allow Bifrost v6/v7 on kbase without PAN_I_WANT_A_BROKEN_VULKAN_DRIVER biblioklept |
| 003-skip-jit-init-and-grow-the-tiler-heap-on-bifrost.patch | kbase: skip MEM_JIT_INIT on Bifrost JM, GROW_ON_GPF tiler heap with 1 MiB commit biblioklept |
| 004-bifrost-atom-stride-check-with-64-candidate.patch | kbase: check the JM atom stride on Bifrost, add stride 64 (seq_nr) candidate biblioklept |
| 005-kbase-ion-fallback-for-placed-map-dma-buf.patch | (no subject) |

### perf (8)

| Patch | Subject |
| --- | --- |
| 001-csf-merge-cache-flush.patch | panvk/csf: emit the cache clean once per subqueue per batch |
| 002-descriptor-bind-noop.patch | panvk: skip DESC_STATE invalidation on redundant set binds |
| 003-ringbuf-256k.patch | panvk/csf: raise the kbase CS ring buffer to 256KB |
| 004-kbase-import-log-gate.patch | kbase: gate the per-import log lines behind a debug option |
| 005-kbase-bo-wrapper-cache.patch | kbase: cache the kbase_kmod_bo wrapper structs |
| 006-async-tiler-heap-destroy.patch | panvk/csf: destroy retired tiler heaps on the heap worker |
| 007-cmd-end-skip-empty-finish-cs.patch | panvk/csf: skip finish_cs for empty command streams |
| 008-pool-flush-written-prefix.patch | panvk: flush only the written prefix of pool BOs |

### local (5)

| Patch | Subject |
| --- | --- |
| 001-bc-decode-chunked-dispatch.patch | bc: chunk the BC decode compute dispatch |
| 002-tolerate-unseekable-dmabuf.patch | tolerate-unseekable-dmabuf |
| 003-skip-lseek-fd-probe.patch | skip-lseek-fd-probe |
| 004-kbase-page-size-helper.patch | kbase: keep the host page-size helper |
| 005-driver-identity.patch | panvk: report "Panvk" / "Mesa 26.3.0-devel" as the driver identity |

---

## 14. gpu（1）— 归档，不应用

GPU 侧实验补丁，仅做归档：**不在 `scripts/apply-patches.sh` 的 SERIES 中，构建永不应用**，
源码树仍等于基座 + 222 个补丁。

| 补丁 | 作用 |
| --- | --- |
| `001-afrc-opt-in.patch` | `PANVK_AFRC=16\|24\|32` 时，应用自建彩色图像在 AFBC 优先的自动 modifier 列表之前尝试 AFRC 固定率压缩；默认 0（关闭）时 modifier 选择路径不变，WSI/CRC、sparse、mutable、cube、block-texel、storage、host-transfer 图像均排除。实验性，尚未真机验证。 |

