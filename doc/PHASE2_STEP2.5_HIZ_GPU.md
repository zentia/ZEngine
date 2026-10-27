# Phase 2 · Step 2.5 收尾 —— Nanite HiZ GPU 侧落地计划（摸底结论）

> 2026-10-27。承接 `PHASE2_PLAN.md` §2.5：页池 GPU 路径已闭环（A/B 13/13 逐像素一致），
> Nanite（C3，★★★）只剩最后一块——HiZ 的 GPU 侧（compute 生成深度金字塔 + GPU 逐簇遮挡剔除）。
> 本文是动手前的**只读摸底**，把"能不能做、卡在哪、怎么切"查清楚。

---

## 0. 一句话结论

**GPU HiZ 的全部前置能力都已就位，无架构阻塞。** 深度源、compute 管线模板、采样图描述符、
验证 harness 四样都现成，且 CPU 侧 HiZ（28/28 全绿）可直接当 GPU 结果的对照基线。
这是一条**低风险、可 headless 端到端验证**的收尾，与页池同一套打法。

---

## 1. 摸底事实（逐条已核实）

### 1.1 深度源：现成，无需新建
- Nanite 离屏光栅 target 已经有真实深度附件：`NaniteGPUResources::target_depth`
  （`D32_SFLOAT`，`DEPTH_STENCIL_ATTACHMENT_BIT`，finalLayout
  `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`），render pass 第 4 个附件、framebuffer 已绑
  （`NaniteRenderer.cpp:1844-1861, 1896-1904, 1948-1953`）。
- **唯一要改的**：`target_depth` 目前 usage 只有 `DEPTH_STENCIL_ATTACHMENT_BIT`，compute
  读它（或读它的金字塔）需要补 `SAMPLED` / `TRANSFER_SRC` usage。这是几行改动。

### 1.2 compute 管线模板：现成，加 shader 即自动编译
- shader 构建是**全自动 glob**：`engine/shader/CMakeLists.txt:3-14` glob `glsl/*.comp` →
  `cmake/ShaderCompile.cmake` 用 `glslangValidator -V100` 编成 `.spv` → 嵌成
  `generated/cpp/<name>.h`，全局变量名 = 文件名点替换下划线再全大写
  （`nanite_hiz.comp` → `Z::Shader::NANITE_HIZ_COMP`）。
- **加一个 HiZ compute shader 零 CMake 改动**：把 `nanite_hiz.comp` 丢进 `glsl/` 即可，
  与 `nanite_cluster_cull.comp` 同一套生成路径。
- 本机 `VULKAN_SDK=C:\VulkanSDK\1.4.363.0` 有 `glslangValidator`，`ZENGINE_USE_VULKAN=ON`。

### 1.3 采样图描述符：RHI 全套支持，且写路径已被验证
- `RenderType.h:1178-1180`：`RHI_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER / SAMPLED_IMAGE /
  STORAGE_IMAGE` 都有；`RHI.h:381` 有 `CreateSampler`。
- Vulkan 侧 `pImageInfo`（sampler + imageView + imageLayout）写路径**已被
  `VulkanBindlessTextureManager` + `VulkanBindlessSmokeTest` 跑通**，不是纸面声明。
- **但 Nanite 现有 descriptor set 全是 storage buffer**（`NaniteRenderer.cpp:1068-1081`：
  binding 0 = UBO，1..6 = SSBO，`NaniteGPUBinding` 枚举 `kBindingCount=7`）。GPU HiZ 需要
  **扩这个 set**：加一个 `COMBINED_IMAGE_SAMPLER`（或 `SAMPLED_IMAGE`）binding 给 HiZ 金字塔。

### 1.4 验证 harness：现成，直接复用页池那套
- `NanitePagePoolABSmokeTest` 已经证明：headless `VulkanRHI` + `NaniteRenderer` 离屏渲染 +
  GBuffer/深度 readback + A/B 逐像素比对，在本机 RTX 5080 上端到端能跑。
- HiZ 的验证天然是 A/B：**HiZ 关（全画）vs HiZ 开（遮挡剔除）**，判据是
  "被遮挡簇确实没画、可见簇一个不少、且无破洞"。深度 readback 已有（页池测试读过 GBuffer，
  深度图 readback 同理，只是 aspect 换 DEPTH）。
- CPU 侧 `nanite_hiz::` 核心（`NaniteHiZ.h`，28/28 全绿）是**GPU 结果的黄金参照**：
  同一张深度图，CPU 金字塔 vs GPU 金字塔逐 texel 比对。

---

## 2. 需要新建的东西（清单）

| # | 产物 | 类型 | 依赖 |
|---|---|---|---|
| A | `engine/shader/glsl/nanite_hiz.comp` | 新 compute shader | `nanite_common.h`；downsample max 归约 |
| B | HiZ 金字塔 GPU 资源（mip 链 storage image 或 texel buffer） | `NaniteGPUResources` 加字段 | 1.2 自动编译 |
| C | descriptor set 扩一个 sampled-image binding | 改 `EnsureComputePipeline` | 1.3 |
| D | `target_depth` 补 `SAMPLED`/`TRANSFER_SRC` usage | 改 `EnsureRasterTarget` | 1.1 |
| E | 深度→金字塔的 barrier + dispatch 驱动 | `Render` 里加一步 | A/B/C/D |
| F | cull shader 加 HiZ 遮挡分支（或独立 pass） | 改 `nanite_cluster_cull.comp` | E |
| G | `NaniteHiZABSmokeTest`（GPU 金字塔 vs CPU 黄金参照 + 遮挡 A/B） | 新测试 | 全部 |

---

## 3. 建议切法（风险递增、各自可验）

**切法一：只做"深度 → GPU 金字塔"，先不接剔除。**
- 新增 `nanite_hiz.comp`：从 `target_depth`（D32）downsample 出 max 归约 mip 链，写进一个
  storage buffer（复用 CPU 侧 `HiZPyramid` 的平铺布局，便于逐 texel 比对）。
- 验证：readback GPU 金字塔，与 `nanite_hiz::BuildHiZPyramid`（喂同一张 readback 深度图）**逐
  texel 比对**，允许量化容差。这一步**不碰 culling、不碰 draw**，风险最低，且直接证明
  "compute 能读深度附件 + 生成正确金字塔"。
- 这是页池"先簿记后接线"的翻版：先把最不确定的一环（compute 读深度图）单独验掉。

**切法二：cull shader 加 HiZ 遮挡分支。**
- 把 CPU 侧 `TestOccluded` 的保守判据逐行镜像进 `nanite_cluster_cull.comp`：每簇 AABB 投影到
  屏幕矩形 → 采金字塔 max → `objectNearestDepth < regionMax` ⇒ 写空命令。
- 验证：A/B——HiZ 关（全画）vs 开（遮挡剔除），比对可见簇集合。判据：
  ①被遮挡簇确实从 draw_commands 消失；②可见簇一个不少；③**无破洞**（GBuffer 覆盖率不因误剔
  而下降）。保守判据的正确性底线是"误剔比不剔更糟"，所以③是硬门槛。

**切法三（可选）：把金字塔从 storage buffer 升级成真正的 mip 纹理 + 采样器。**
- 只有切法二验证通过、且需要硬件双线性/宽采样才做。第一版用 storage buffer 足够，避免过早
  引入 image layout 管理。

---

## 4. 风险与边界（诚实）

- **深度图 layout**：`target_depth` finalLayout 是 `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`，compute
  读它前要 barrier 到 `SHADER_READ_OPTIMAL`（或读它的拷贝）。这是页池 GBuffer readback 同款
  barrier，已知怎么写。
- **reversed-Z 一致性**：CPU 侧约定 1.0=最近、max 归约。GPU 侧必须逐位同约定，否则遮挡判据
  反向。切法一的逐 texel 比对会直接抓出来。
- **D32 采样精度**：storage buffer 路径读的是原始 texel（无过滤），与 CPU 完全对齐，最稳。
- **cull shader 已是共享 set**：加 binding 会改 descriptor set layout，compute 与 vertex 共用
  （`NaniteRenderer.cpp:1072,1079`）。加的 binding 要标 `COMPUTE_BIT` only，别动 vertex 的绑定。
- **Metal 侧**：`EnsureComputePipeline` 不再按后端硬挡（`NaniteRenderer.cpp:1053-1063`），Metal
  理论上也能走；但本机无 Metal，只能验 Vulkan。Metal 的 HiZ 正确性留真机。

---

## 5. 下一步

从**切法一**开始：写 `nanite_hiz.comp` + GPU 金字塔资源 + 逐 texel 比对测试。这一步不碰
culling/draw，风险最低，且把"compute 读深度附件"这个最不确定的环节单独验掉。通过后再进切法二。

---

## 6. 切法一落地记录（commit 9c57cbef，A/B 14/14 全绿）

**结论等级：A（已验证，逐 texel 全绿，零 validation error）。**

新增 `nanite_hiz.comp` 两阶段 compute kernel + `NaniteRenderer` 侧 GPU 金字塔资源
（`EnsureHiZResources` / `BuildHiZPyramidGPU` / `DestroyHiZResources`）+ headless A/B
测试 `NaniteHiZABSmokeTest`。渲染一帧 → 读回 GPU 金字塔 + D32 深度 → 与 CPU
`BuildHiZPyramid` 逐 texel 比对：**14/14，total_mismatch=0 max_abs_delta=0**。

### 三个踩坑（都已在代码注释里留痕）

1. **深度图 usage 缺 TRANSFER_SRC_BIT**：A/B 测试要把 `target_depth` 拷回 host 做对比，
   `CmdCopyImageToBuffer` 要求 `TRANSFER_SRC_BIT`。`EnsureRasterTarget` 建图时补上
   （`DEPTH_STENCIL_ATTACHMENT | SAMPLED | TRANSFER_SRC`）。

2. **深度图 layout 不回转**：`BuildHiZPyramidGPU` 采样前把深度图
   `DEPTH_STENCIL_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`，采完不转回去，下一帧
   render pass（和测试读回 barrier）拿到 `SHADER_READ_ONLY` 就报
   「expected DEPTH_STENCIL_ATTACHMENT_OPTIMAL」。采样结束后补一道 barrier 转回
   `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`。

3. **header 立即模式写入被覆盖（最隐蔽，也是 GPU 全 0 的真根因）**：原实现每个 dispatch 前
   `UploadBytes(hiz_header, ...)` 重写 header。但 `UploadBytes` 是**立即模式 host memcpy**
   （Map→memcpy→Unmap），录制命令流期间多次写同一 buffer 只留最后一次——GPU 真正执行时
   **所有 dispatch 都读到同一份（最后一份 downsample）header**，mip0 拷贝阶段拿错 header，
   金字塔全 0（`gpu=0.0` 全场）。修复：所有 dispatch 的 header 一次写进 staging buffer，
   每个 dispatch 前 `CmdCopyBuffer` 把对应份拷进 `hiz_header` + 一道 transfer→compute
   barrier。`CmdCopyBuffer` 在命令流内、按 dispatch 顺序生效，根除覆盖。

> **教训**：RHI 没有 push constant（只有单 32 位 bindless index 的 `CmdSetBindlessIndexPFN`），
> 也没有 `CmdUpdateBuffer`。要在**同一命令流内给多次 dispatch 喂不同小常量**，唯一干净的路子是
> staging buffer + `CmdCopyBuffer`。凡是"每个 dispatch 参数不同"的 compute，都不能用立即模式
> `UploadBytes` 写常量 buffer。

### 现状

- `hiz_data` 仍是 host-visible storage buffer（`kNaniteHostMemory`），布局与 CPU
  `HiZPyramid.data` 逐字节一致，测试直接读回比对。切法三再升级成 device-local + 真 mip 纹理。
- HiZ 有自己独立的 descriptor set / layout / pipeline（3 binding：header UBO / depth
  combined image sampler / data storage buffer），不与剔除那套 7-binding set 共用，避免动
  compute+vertex 共享 layout。
- 金字塔建在 `DrawClustersIndirect()` 之后（深度是那一遍光栅写出来的），`Render` 里
  `target_depth != nullptr` 时触发。

### 下一步

进**切法二**：把 CPU 侧 `TestOccluded` 保守判据逐行镜像进 `nanite_cluster_cull.comp`，A/B
验证「被遮挡簇消失 / 可见簇完整 / 无破洞（GBuffer 覆盖率不降）」。③无破洞是硬门槛。
