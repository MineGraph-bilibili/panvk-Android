# 补丁系列说明 / Patch series notes

本仓库的 Mesa 源码树 = 基座提交 `5a07217f034b3e50d8c7c7794f97a2df1742613b`
(26.3.0-devel) + **223 个补丁**。补丁已在当前树中全部应用，`patches/` 目录保留完整
系列，用于复现、审阅和回退。

应用顺序由 `scripts/apply-patches.sh` 固定：

```
common → android → kbase-common → app-loader → wsi → csf → csf-v11 → jm-v9 → jm-v7 → perf → local
```

* 每个家族的 `*.patch` 按文件名顺序应用；`git apply --check` 失败时依次回退到
  `--recount`、`-C1`（一行上下文），仍失败则 `PATCH-DRIFT` 硬失败退出。
* 家族目录下若存在 `files/`，该家族的补丁应用完后会整目录覆盖到 Mesa 树
  （`app-loader`、`kbase-common` 使用这种方式替换基线文件）。
* 正常输出为 `OK applied=223 profile=g615-v11-csf`。

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

## 5. wsi（4）

Android 窗口系统集成层：`VK_KHR_android_surface` + AHB 交换链。

| 补丁 | 作用 |
| --- | --- |
| `016-android-surface-swapchain` | 引入 `VK_KHR_android_surface` 与基于 AHB 的交换链实现。 |
| `017-android-swapchain-owned-ahb` | 标记交换链自带的 AHB 为驱动所有，避免应用侧提前回收。 |
| **`018-android-present-fast-copy`（自研）** | 裁剪 Android WSI 呈现路径：present 时直接做一次拷贝，去掉多余的中转与同步，降低每帧呈现的 CPU 开销。该项与 perf 系列同属自研优化，也是 XenDroid+ 黑屏问题排查（`local/002`、`local/003`）所在链路的相邻改动。 |
| **`019-android-zero-copy-present`（自研）** | 交换链直接渲染进窗口缓冲：创建时预 dequeue 并把每个窗口的 AHB 经延迟 AHB 路径导入为 `VkImage`，acquire 只 dequeue + 等 fence，present 只发一次 `queueBuffer`，去掉每帧整帧 CPU 拷贝与冷缓存 AHB 读。缺符号、几何/格式不匹配、dma-buf 导入失败或 `PANVK_WSI_ZERO_COPY=0` 时整体回退旧拷贝路径并打印一行日志。构建通过，尚未真机验证；仅对直接使用 Android surface 的原生应用生效（Winlator 走自带 X11 WSI，不会触发）。 |

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
| `007-kraid-rc-upgrade` | kraid 编译器整体对齐本地 rc66：ISA 描述切到 `isa-v9-v14.xml`，新增 `opt_exec_units` / `opt_fuse` / `opt_normalize_consts` 三个 pass，编解码与指令集扩到 v14。 |
| `008-compiler-tex` | NIR 增加 Panfrost 专用 tex 源与 `TEX_DUAL`，mediump 走 16-bit，Bifrost 常量排序/IDVS/repair-SSA 对齐本地，`bi_ra.c` 回到本地旧版求解器。 |
| `009-perf-lifecycle` | 命令池自动裁剪窗口、TLS 稀疏化/收缩、kbase 稀疏缓冲与回读镜像、tiler heap 槽位与终止策略对齐本地。 |
| `010-android-afbc-gralloc` | 绕过 ARM gralloc 对 GPU-only 缓冲区强制 AFBC 的布局，补 dma-buf fd 查找与 BGRA/RGBA 处理；`panvk_wsi.c` 改回本地同步 WSI（覆盖仓库 zero-copy 实现）。 |
| `011-kbase-backend` | kbase 后端对齐本地：丢弃无效的 tiler OOM CSI handler、`pan_kmod` 增加直写 logcat 的诊断输出、物理设备设置 gralloc usage 位并放开 v10 `robustBufferAccess2`。 |
| `012-panvk-cmd-path` | 命令流路径对齐本地：clean-tile 跳过 preload、BC 转码管线后台预热、blend 描述符脏标记、null descriptor 修正。 |

### 本地树同步 `007`–`013`

这一段不是新增功能，而是把 `work/mesa`（rc66 状态）与本仓库树的真实差异按功能分组回写：
逐组用本地版本覆盖同名文件，冲突处**一律以本地行为为准**（见 `010` 的 `panvk_wsi.c`、
`008` 的 `bi_ra.c`）。仓库独有的打包设施（`patches/`、`scripts/`、`meson/`、`profiles/`、
`dist/`、`sources.lock`、`validation/`、`BUILDING.md`、`PATCHES.md`）保持不变，本地没有的
仓库文件也不删除。每组补丁由 `git diff --cached -- <该组文件>` 生成，可
`git apply --check --reverse` 回退单组。注意：诊断宏 `panvk_diag_result` /
`PANVK_DIAG_PRINTF` 定义在 `013` 才落地的 `panvk_macros.h`，因此 `007`–`012` 单独检出
不可编译，完整序列（含 `013`）才是自洽状态。

#### `007-kraid-rc-upgrade`（39 文件）

* **机制**：kraid 目录 36 个既有文件 + 3 个新增文件整体替换为本地 rc66 版本。ISA 描述由
  `isa-v9-v14.xml` 显式指定（`--cfg kraid_isa_xml_path`），新增 `opt_exec_units`、
  `opt_fuse`、`opt_normalize_consts` 三个 pass 并在 `lib.rs` 注册；`ops.rs` 增补
  `OpCmp` / `OpHAdd` / `OpICmpMulti` / `OpIDpAdd` / `OpIToF16` / `OpF16ToI32` 等指令与
  相应折叠，`encode_v9` / `decode` 扩展到 v14 编码，`ra.rs` 重写、`stats.rs` 增加编译统计
  （`PANVK_KRAID_DEBUG=stats`），反汇编器由 `kraid_disasm` cfg 打开（bindgen 同时放行
  `fwrite|fflush` 与 `FILE`）。
* **收益**：编译器覆盖 v9–v14 指令集，常量归一化/指令融合带来的冗余被消除，寄存器分配与
  上游 rc66 对齐，减少后端生成错误指令的机会。
* **风险/注意**：kraid 是 Rust 组件，必须用 rustc + bindgen 环境验证；`meson.build` 里
  `isa_path` 由目录改为 `isa-v9-v14.xml` 文件名，若本地只保留 `isa-v15.xml` 会直接构建失败；
  该组是本系列最大的一组（约 5.7k 增 / 1.8k 删），回退时按整组反向应用。
* **来源**：rc66 本地树（对齐本地行为）。

#### `008-compiler-tex`（16 文件）

* **机制**：NIR 侧新增 Panfrost 专用纹理源 `nir_tex_src_var_handle_pan` /
  `nir_tex_src_var_buf_offset_pan` 与 `nir_texop_dual_pan`（`TEX_DUAL`），`nir_print.c`
  补上打印；`pan_nir_lower_tex.c` 增加 TEX_DUAL 的双纹理合并且共享 sampler、cube 先降成
  array、`VAR_*TEX*` 变体采样路径；`va_lower_constants.c` 让 FAU 常量提升按 64-bit LUT
  预算记账；`pan_compiler.c` 把 `USE_KRAID_ALL` 扩到全部阶段、v11+ 的 kraid 不再
  `lower_txp`，并加入寄存器/管线占用统计；`float16.rs` 增加 `F16::ONE`。
  Bifrost 后端：`bifrost_compile.c` 常量排序改按数值降序比较（FAU 阈值才正确）、
  `bifrost_nir.c` 调整 mem-vectorize / IDVS 特化 / lower 顺序、`bi_repair_ssa.c` 用稀疏
  map 重写。PanVK 侧 `panvk_vX_shader.c` 默认开启 mediump 16-bit
  （`PANVK_NO_MEDIUMP=1` 关闭）并写进 shader hash，`panvk_vX_meta_desc_copy.c` 与
  `panvk_vX_nir_lower_descriptors.c` 改为带位置信息的返回码与无条件 `memcpy`。
* **收益**：mediump 的 ALU/纹理结果/颜色输出真正跑在 16 bit（Mali-G57 上 mediump 纹理
  填充快约 20%）；`TEX_DUAL` 让两张窄句柄纹理共用一次发射；常量排序修好了 FAU 容量阈值。
* **风险/注意**：`bi_ra.c` 用本地**旧版**线性约束求解器（单 affinity、`spill_node` +
  snapshot/restore + coalesce）覆盖仓库里**较新**的 lcra（forced/pending/failed 与 32/64
  两次尝试），这是按“本地行为为准”的决定，上游后续修复将不会自动带入；mediump 16-bit 会
  改变 shader 代码生成，遇到精度问题用 `PANVK_NO_MEDIUMP=1` 回退。
* **来源**：rc66 本地树（对齐本地行为）。

#### `009-perf-lifecycle`（13 文件）

* **机制**：
  * `panvk_cmd_pool.c/h`：命令池按两次 reset 窗口（`PANVK_CMD_POOL_AUTOTRIM_WINDOW`，
    默认 64）统计各 free list 的最大回收量并留 25% 余量做自动裁剪
    （`PANVK_CMD_POOL_AUTOTRIM=0` 关闭），避免 DXVK 每帧 reset 后池无限增长。
  * `csf/panvk_vX_cmd_buffer.c`：设备 TLS 改为按存在核数稀疏化
    （`PANVK_TLS_SPARSE`，kbase 默认开）、超龄 BO 可释放（`PANVK_TLS_FREE_OLD`）、
    空闲超过 `PANVK_TLS_SHRINK_MS`（默认 10 s）收缩；新增 `cmd_copy_tls_to_tsd()` 统一
    把 TLS 指针与栈大小写进 per-job TSD；`BeginCommandBuffer` 顺手预热 BC 转码管线。
  * `panvk_buffer.c/h`：kbase 没有原生 sparse VM，稀疏 Buffer 用一块可增长的
    `sparse_backing` BO 承载（`sparse_backing_full` 记录是否已全部提交并可 CPU 映射）。
  * `panvk_async_bind.c`：kbase 路径上的稀疏绑定改为“CPU 镜像 → 后备 BO 上传 + GPU 结果
    回读”，并在 `panvk_queue_vm_bind` 处补 sparse sync/统计。
  * `csf/panvk_vX_gpu_queue.c` + `csf/panvk_queue.h`：tiler heap 槽位数
    （`PANVK_KBASE_HEAP_SLOTS`）、异步 TERM 并发上限（`PANVK_KBASE_HEAP_TERM_MAX`）、
    每提交检测退休堆（`PANVK_KBASE_HEAP_EAGER_TERM`）与 `PANVK_KBASE_HEAP_CHUNKS`
    几何参数均可配；等待 seqno 时解码三路 command stream；SAFE_SUBMIT 前向声明在此。
  * `panvk_priv_bo.c/h`：新增 `alias_size`，kbase `MEM_ALIAS` 稀疏 TLS 别名区在
    destroy 时按 SAME_VA 语义 `munmap`。
* **收益**：长时间运行时命令池/私有 BO 不再无界增长；TLS 只按实际存在的核占地址空间；
  稀疏 Buffer 在 kbase 上可用；退休 tiler heap 更早回收。
* **风险/注意**：这一组是行为最多的改动，依赖 kbase BO 语义（`MEM_ALIAS`、SAME_VA）与
  多线程 worker；`PANVK_TLS_SPARSE=0` / `PANVK_TLS_FREE_OLD=0` / `PANVK_TLS_SHRINK_MS=0`
  / `PANVK_KBASE_SAFE_SUBMIT=1` 是逐项回退开关。稀疏镜像目前只是 CPU 侧影子拷贝，
  真实 sparse 语义仍需在 kbase 上验证。
* **来源**：rc66 本地树（对齐本地行为；部分条目源自 csf-v11 226–232）。

#### `010-android-afbc-gralloc`（8 文件）

* **机制**：
  * `u_gralloc_fallback.c`：直接从 ARM gralloc 的 native_handle 解析布局（Exynos
    `data[15]`、MediaTek `data[21]` 两套 magic 偏移），按“足够大的 dma-buf fd”定位实际
    buffer fd，从 `u64 alloc_format` 取出 HAL format 与 AFBC 位，识别 allocator 自建的
    AFBC 布局；非 ARM handle 返回 `-ENOENT` 让上层回退。
  * `vk_android.c/h`：新增 `vk_android_native_handle_dma_buf_fd()`（不再假设 0 号 fd 就是
    dma-buf，MediaTek 的 handle 带三个 fd）与 `vk_android_ahb_dma_buf_fd()`；AHB 探测/分配
    统一叠加 `vk_physical_device::android_ahb_image_usage`，并为 GPU-only BGRA 实际存成
    RGBA 的 allocator 做格式回落。
  * `panvk_physical_device.c`（在本组只补字段使用侧）、`panvk_android.h`：定义
    `MALI_GRALLOC_USAGE_NO_AFBC`(1<<29) 与 `GRALLOC_USAGE_HW_COMPOSER`(1<<11)，让
    AHardwareBuffer 请求避开驱动无法导入的 AFBC 布局。
  * `panvk_image.c`：opt-in 的 `PANVK_AFRC=16|24|32` 固定率压缩（默认 0 关闭，跳过 WSI /
    CRC 图像），以及 BC 图像 raw plane 可丢弃判定的修正。
  * `pan_fb.h`：新增 `discard_candidate`，loadOp=DONT_CARE/NONE 且无 resolve 时允许省掉
    preload（最终是否省还要看 tile 与 AFBC 块大小）。
  * `panvk_wsi.c`：用本地**同步 WSI** 覆盖仓库原有的 zero-copy 实现（移除 ANativeWindow
    ops 解析、`panvk_android_zero_copy_*`、AHB 直接导入与 damage 计算，回到“每张 image 自持
    AHB + present 时同步提交”的路径，114 增 / 606 删）。
* **收益**：ARM gralloc 机型上 AHB 能被正确导入（拿到对的 fd / stride / modifier），不再因为
  GPU-only 缓冲被强制 AFBC 而导入失败；AFRC 提供了按需实验入口。
* **风险/注意**：`panvk_wsi.c` 是**行为级回退**（zero-copy → 同步拷贝），present 路径的
  延迟/带宽特性与仓库版本不同，这是按“本地行为为准”的决定；gralloc 布局解析依赖具体
  allocator 的 handle 约定（Exynos / MediaTek 已验证），其它厂商会走 `-ENOENT` 回退；
  `PANVK_AFRC` 为实验特性，默认关闭。
* **来源**：rc66 本地树（对齐本地行为）。

#### `011-kbase-backend`（7 文件）

* **机制**：
  * `kbase_kmod.c/h`：`kbase_kmod_csf_group_create()` 去掉 `tiler_oom_handler` 参数与
    `BASE_CSF_TILER_OOM_EXCEPTION_FLAG`——本地实测该 handler 在本机内核上无效（未处理 OOM
    仍会终止 group），因此不再申请 CSI 异常 handler，只保留 uAPI ≥1.25 的
    `cs_fault_report_enable`；group 创建日志从 info 降到 debug。
  * `pan_kmod.c`：新增 `pan_kmod_diag_printf()`（stderr + `__android_log_write`），让诊断
    在被 abort 丢掉 stderr 管道前仍能落到 logcat。
  * `panvk_gpu_prerast.c`：OOM 返回改走 `panvk_diag_result()`，带文件/行号。
  * `panvk_physical_device.c/h`：kbase 初始化时设置
    `vk.android_ahb_image_usage = MALI_GRALLOC_USAGE_NO_AFBC | GRALLOC_USAGE_HW_COMPOSER`；
    新增 `panvk_bc_format_native()`，`PANVK_BC_NATIVE=1` 时只对 GPU 自报支持的 BC 格式走
    原生解码，其余仍模拟。
  * `panvk_vX_physical_device.c`：默认隐藏 `robustImageAccess` / `robustImageAccess2`
    （`PANVK_NO_ROBUST_IMAGE=0` 恢复上游声明），用于排查按“restricted core”表选
    robustness 模式的模拟器路径；同时把 `robustBufferAccess2` 下放到 v10。
* **收益**：去掉一个实测无效的 tiler OOM 协商，少一次误导性日志与 uAPI 分支；关键 OOM
  诊断在 logcat 里可见；ARM gralloc 机型图像 AHB 走线性格局；v10 多拿到
  `robustBufferAccess2`。
* **风险/注意**：去掉 `csi_handlers` 后 tiler 堆耗尽仍由内核直接终止 group（与 panfork 的
  CSF 路径一致，但没有应用层兜底）；`robustImageAccess2` 在 v10 仍受开关控制，
  `robustBufferAccess2` 放宽到 v10 属于能力声明变更，需按 GPU 实测。
* **来源**：rc66 本地树（对齐本地行为）。

#### `012-panvk-cmd-path`（14 文件）

* **机制**：
  * `panvk_vX_cmd_draw.c`：新增 `PANVK_CLEAN_TILE_WRITE=1` 路径，在 tile 尺寸最终确定后
    判断某个 target 能否整块省掉 preload；interleaved Z/S 只 store 单平面时不再误踩另一
    平面；渲染状态与 sysvals 相应更新。
  * `csf/panvk_vX_cmd_draw.c`：删除 `PANVK_KBASE_TILER_IR`（kbase incremental rendering）
    开关与相关注释——本地树不再走这条路径；`fn_addr` 由 `uint32_t` 改 `uint64_t`，避免
    地址截断；补 draw/render 计数与带位置的 OOM 返回。
  * `panvk_vX_cmd_meta.c` + `panvk_device.h` + `panvk_vX_device.c`：新增
    `bc_pipeline_mtx` 与后台预热（`PANVK_BC_PREWARM`，默认开），首个 BC 上传不再在
    提交线程上编译转码管线；设备销毁时 `bc_prewarm_finish()` 收尾。
  * `panvk_cmd_draw.h` + `panvk_vX_blend.c`：新增 `blend_descs_dirty`，blend 描述符重建后
    只让 `cmd_prepare_draw_sysvals` 重新拷贝一次；`panvk_resolve_attachment` 去掉不再使用的
    `flags`。
  * `panvk_vX_descriptor_set.c`：null descriptor 改写为 `get_sampler_subdesc_info()`，
    原来用 tex 子描述符填 sampler 槽会写坏描述符。
  * `panvk_instance.c`：`GetInstanceProcAddr` 统一传 `&instance->vk`（不再依赖
    `vk_instance_get_proc_addr(NULL)` 的空实例路径）。
  * `csf/panvk_vX_cmd_precomp.c`：复用 `cmd_copy_tls_to_tsd()`，并简化 SPD 地址计算。
  * `panvk_vX_query_pool.c` / `panvk_vX_cmd_push_constant.c` / `panvk_vX_cmd_frame_shaders.c`
    / `panvk_vX_cmd_desc_state.c`：OOM 统一走 `panvk_error()` / `panvk_diag_result()`。
* **收益**：clean-tile 路径省掉无用的 preload 带宽；BC 转码管线在首次上传前就绪，避免首帧
  卡顿；重复 blend 绑定不再触发 sysvals 全量重拷；null descriptor 修正与 64-bit 地址修正
  消除两类真实缺陷。
* **风险/注意**：删除 `PANVK_KBASE_TILER_IR` 意味着不再有“FINISH_FRAGMENT 回收 chunk”
  的兼容路径，v14 上原先会 DEVICE_LOST 的场景没有开关可回退；`PANVK_CLEAN_TILE_WRITE`
  默认关闭，打开后需确认 AFBC 块大于 tile 时仍会保留 preload（`discard_candidate` 逻辑）；
  BC 预热会在设备创建后占用一个后台线程。
* **来源**：rc66 本地树（对齐本地行为）。

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

## 13. 附录：完整补丁清单（223）

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

### wsi (4)

| Patch | Subject |
| --- | --- |
| 016-android-surface-swapchain.patch | panvk: Android VK_KHR_android_surface + AHB swapchain |
| 017-android-swapchain-owned-ahb.patch | panvk: mark Android WSI swapchain AHBs as driver-owned |
| 018-android-present-fast-copy.patch | panvk: trim the Android WSI present path |
| 019-android-zero-copy-present.patch | panvk/android: zero-copy present into the window buffer |

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

