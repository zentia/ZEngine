# 去 SSBO 方案：可行性验证清单（Unity 侧最小 `.shader` + 对拍项）

> **状态：草案，未拍板。** 对应 `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` §5.4、§13.1 第 17 条。
>
> ⚠️ **2026-10-26 路线更新**：OSGame 侧已选定**路线 B「申请特性例外」**（§5.6），
> 即申请 `#pragma target 3.5` + `texelFetch`。本清单的**主轨是 `texelFetch`**；
> `tex2Dlod` 是**例外被驳回时的兜底轨**（G2 第 11 项专测它）。
>
> **目的：在改容器格式之前**，用一份最小的 Unity `.shader` 证明"没有 SSBO 也能跑 §6 那条公式"，
> 并把代价量化出来。任一门不过 ⇒ **No-Go**，回退 §5.1 的 SSBO 默认（只在 PC / 编辑器走 GPU 路径）。
>
> 触发条件：目标后端无 SSBO —— GLES3 / ES 3.0 没有 SSBO；Unity 的 `StructuredBuffer` /
> `ByteAddressBuffer` 要 SM5.0+，GLES3 上**编译失败**（不是降级）。

---

## 0. 这份清单要回答的三个问题

| 门 | 问题 | 不过会怎样 |
|---|---|---|
| **G0** | 目标机型到底支持什么（UBO 上限 / 纹理格式 / array texture 层数） | 连通道分配都定不下来 |
| **G0'** | **例外申请的两项在该机型真的可用**（target 3.5 不降级、`texelFetch` 真生成） | 申请批了也白批 |
| **G1** | 最小 `.shader` 能否在目标机型编译并画出正确的第一帧 | 方案不成立 |
| **G2** | 能否与 CPU 参考**逐位**一致 | 对拍体系失效 ⇒ §17 那套"两端同一条公式"的保证没了 |
| **G3** | 代价是多少（显存 / 每帧上传 / draw call / CPU） | 省了内存却赔了别的，等于没省 |

**先 G0 → G0'，再 G1，G2/G3 并行。任何一步 No-Go 就停，不要往下推。**

---

## 1. 前置：把"必须随机访问"的最小集钉住

| 绑定 | 内容 | 无 SSBO 时落点 | 依据 |
|---|---|---|---|
| b0 `vertex_words` | u32 视图（rigid 8 B/顶点；weighted 8 B = `(offset, count)`） | 纹理 `texelFetch`（R32UI） | UBO 装不下（见下） |
| b1 `infl_bind` | float2/影响 | 纹理（RG32F 或 RGBA32F） | 同上 |
| b2 `infl_bone` | u32/影响（`boneIdx \| weight<<16`） | 纹理（R32UI），**必须 u32 原值** | 位运算不能被 fp 归一化破坏 |
| b3 `uvs` | float2 | 纹理 | 同上 |
| b4 `bones` | 6 f32/骨 | **UBO**（41 骨 × 24 B ≈ 1 KB） | 远低于 ES3.0 保证的 16 KB |
| b5 `draws` | 48 B/项 | UBO 或纹理（3 texel） | 每帧量小 |
| b6 `insts` | 96 B/实例（mat4 + vec4 + vec4） | UBO（6 texel 或 24 float） | 每帧量小 |
| b7 `deform` | float/条目 | 纹理（每帧 `TexSubImage`） | 动态 |
| b8 `cpu_verts` | vec4（CPU 裁剪顶点） | 纹理（每帧 `TexSubImage`） | 动态 |

**为什么静态几何不能走 UBO**：实测静态数据中位 **33.7 KB / p90 97.7 KB**（§17），
远超 ES3.0 保证的 `MAX_UNIFORM_BLOCK_SIZE = 16 KB`（实机常见 64 KB，p90 仍超）。
**为什么不能走顶点属性定长展开**：每顶点影响数最大 **15**（§1.5）—— 定长 4 槽卡死 297 个资产。

---

## 2. G0 平台能力探针（真机，一次性）

输出一张表：**机型 / GPU / GL 版本 / 各项上限**。至少覆盖目标最低配机型 + 一台中端。

- [ ] `GL_MAX_UNIFORM_BLOCK_SIZE`（参考：ES3.0 保证 16384）
- [ ] `GL_MAX_VERTEX_UNIFORM_BLOCKS`（参考：ES3.0 保证 12）
- [ ] `GL_MAX_TEXTURE_SIZE`、`GL_MAX_TEXTURE_IMAGE_UNITS`
- [ ] `GL_MAX_ARRAY_TEXTURE_LAYERS`（参考：ES3.0 保证 **256**；781 个资产 ⇒ 必须分桶）
- [ ] `GL_MAX_VERTEX_ATTRIBS`（参考：16）
- [ ] **RGBA32F / RG32F / R32F / R32UI 是否可 `texelFetch`** —— ⚠️ **实测，不要看文档**
      （ES3.0 core 里 RGBA32F 可采样、NEAREST/`texelFetch` 合法；线性滤波才需要
      `OES_texture_float_linear`；**采样**不需要 `EXT_color_buffer_float`，那是渲染要的）
- [ ] Unity 侧反证：写一句 `StructuredBuffer<float4> _Bones;` 在 GLES3 下编译，
      **确认它是硬失败**而不是被静默降级（这条决定"我们是不是真的必须去 SSBO"）
- [ ] `SystemInfo` 侧的 `maxTextureSize` / `graphicsShaderLevel`（Unity 报的与 GL 查询对齐一次）

⇒ 产出：**这台机器走 A（`texelFetch` 单纹理 + 骨骼 UBO）还是 B（array texture）**。

### 2.1 G0' 例外项探针（路线 B 专属，批准与否都要先测）

- [ ] `#pragma target 3.5` 在该机型**编译通过且不降级**（抓 Unity 编译日志 / 产物确认）
- [ ] `SV_VertexID` / `SV_InstanceID` 在 target 3.5 下真的可用（Unity 口径需实测，不要假定）
- [ ] ⚠️ **交叉编译产物确认**：Unity HLSL 侧写 `Texture2D.Load(int2)`（或 `SAMPLE_TEXTURE2D_LOD`），
      抓 **GLES3 生成的 GLSL** 看它是不是真的 **`texelFetch`** —— 若被退化成 `texture2DLod`
      （归一化坐标 + 采样），则 §5.1 否决的那条舍入路径又回来了，主轨不成立
- [ ] `Texture2D<uint4>`（`usampler2D` + `texelFetch` 返回 `uvec4`）在该机型可用
      —— b2 的位运算依赖它；退成 float 会破坏 `(bw >> 16) & 0xFF`
- [x] ~~`precise` 在 Vulkan 后端是否保留~~ —— **已判定，不用测**：Android = **OpenGLES3**、iOS = **Metal**，
      **移动端没有 Vulkan 后端**（`ProjectSettings.asset:709-719`，§5.5）⇒ `precise` 在移动端两个后端都拿不到
      ⇒ bit-exact 只在 PC（D3D11）上维持，移动端一律按 G2 第 10 项的**误差阈值**判

---

## 3. G1 最小 Unity `.shader`

### 3.1 选 3 个验证资产（不要一上来全量 781）

| 用途 | 选法 | 工具 |
|---|---|---|
| 纯 rigid（UI 头像类） | 影响数全为 0 | `doc/scripts/measure_zspg_chunks.py` |
| weighted 且**影响数达到最大 15** | 取全量里每顶点影响数最大的那个 | 同上 / `SpineGpuBenchmark --csv` |
| **带 deform + 槽位暗色** | deform 覆盖 93% 资产（§14.1），必须验 | 同上 |

### 3.2 骨架（ShaderLab，单文件；是骨架不是可编译终版）

```c
Shader "ZEngine/SpineGpuNoSsbo"
{
    Properties
    {
        _Atlas ("Atlas", 2D) = "white" {}
    }
    SubShader
    {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }
        Pass
        {
            ZWrite Off  Cull Off
            Blend One OneMinusSrcAlpha          // 按混合模式出 4 份材质实例（不是 4 个 Pass）

            HLSLPROGRAM
            #pragma vertex   vert
            #pragma fragment frag
            #pragma target   3.5                // ⚠️ 路线 B 的申请项之一（§5.6），待批准

            // ---- 静态几何：texelFetch（fp32 / u32 原值，绝不能退到 fp16）----
            //      主轨 = Texture2D.Load → GLSL 的 texelFetch（G0' 要抓产物确认）
            //      兜底轨（例外被驳回时）= tex2Dlod + Point 采样 + W/H 取 2 的幂
            //         tex2Dlod(_InflBind, float4(((i % W) + 0.5) / W, ((i / W) + 0.5) / H, 0, 0))
            //         ⚠️ 逐位性更脆弱，必须过 G2 第 11 项
            Texture2D<float4>  _InflBind;       // b1  bindPos（float2）
            Texture2D<uint4>   _InflBone;       // b2  boneIdx | weight<<16 —— 必须 u32
            Texture2D<uint4>   _VertexWords;    // b0
            Texture2D<float4>  _Uvs;            // b3
            Texture2D<float4>  _Deform;         // b7，每帧 TexSubImage
            SamplerState       sampler_Point_Clamp;   // texelFetch 不用采样器，占位

            // ---- 骨骼表：UBO（std140）----
            CBUFFER_START(Bones)
            float4 _Bones[256];                 // 每骨 1.5 texel → 按 2×vec4 存，手动取分量
            CBUFFER_END

            // ---- 绘制项 / 实例：UBO 或纹理 ----
            CBUFFER_START(Draws)
            float4 _Draws[...];                 // 48 B/项 = 12 u32 = 3×vec4
            float4 _Insts[...];                 // 96 B/实例 = 6×vec4
            CBUFFER_END

            // ---- 无顶点输入流 ----
            //   SV_VertexID     → gl_VertexID    （ES3.0 有，Unity 需 target 3.5）
            //   SV_InstanceID   → gl_InstanceID  （ES3.0 有；⚠️ D3D12 不含 StartInstanceLocation，见 §15）
            //   ⚠️ 若 SV_VertexID 申请不下：退化为顶点属性自带本地下标 vi（+4 B/顶点），
            //      同一段顶点被多个绘制项共用时 vi 序列相同 ⇒ §3.2 的去重仍成立

            float4 _StaticSize;   // (W, H, _, _) —— texel 坐标换算；**W 取 2 的幂**用位运算

            // ⚠️ 蒙皮算术必须防 FMA 收缩：
            //    GLSL 用 `precise`；**GLSL ES 3.00 没有 precise** ⇒ 申请不到（§5.6）
            //    ⇒ 与 CPU 的逐位一致不能假定，必须实测（G2 第 10 项）
            ENDHLSL
        }
    }
}
```

### 3.3 编译/运行门（一个不过就停）

- [ ] 目标机型**能编译**（不是只在 PC 上编过）
- [ ] 能画出第一帧，且**形状正确**（不是"像" —— 与 CPU 参考叠图，错一格都算不过）
- [ ] `SV_VertexID` / `SV_InstanceID` 在该机型真的可用
- [ ] 4 种混合模式（normal / additive / multiply / screen）各能出一版材质
      （**4 个材质实例，不是 4 个 Pass** —— 4 个 Pass 会变成 4 次 draw）

---

## 4. G2 对拍项（逐项，锚点已存在）

统一判据：**与 CPU 参考逐位相同（bit-exact）**，第 10 项除外（给误差阈值）。
CPU 参考直接用现成的：

| 锚点 | 位置 | 覆盖 |
|---|---|---|
| `SpineGpuEmulateVertex(asset, batch, draw, vi, out)` | `SpineGpuBatch.h:140` | VS 全路径（rigid / weighted / deform / root / uv），输出 `clip_x/clip_y/u/v` |
| `SpineGpuShadeReference(...)` | `SpineGpuTintBlack.h:58` | 片元（预乘 / 槽位色 / tint black / additive） |
| `SpineGpuRefParityTest` S6 四格 | `Render/Spine/Test/SpineGpuRefParityTest.cpp` | (rigid/weighted) × (有/无 deform) 全量 781 资产口径 |
| `SpineGpuDx12RasterTest` | `Render/Interface/DX12/Test/` | **GPU 侧真寻址**的样板（去 SSBO 版照抄这个结构） |

| # | 对拍项 | 通过判据 |
|---|---|---|
| 1 | rigid 蒙皮（槽位骨骼单矩阵） | `clip_x/clip_y` 逐位 = `SpineGpuEmulateVertex` |
| 2 | weighted 蒙皮，影响数 **1..15 全覆盖** | 同上；§6 的"按影响存储顺序累加"必须验到 15（均值 2.20 只覆盖常见情况） |
| 3 | deform 四格：rigid **替换** / weighted **逐影响相加** | 与 `SpineGpuRefParityTest` S6 同口径；错了是成百上千单位，不是噪声 |
| 4 | `uv_offset` 回绕（§3.2 v13 语义） | 取 119 737 个绘制项里 `vertex_base != 0` 的那 112 562 个；`uv_offset + index` uint 回绕后 = `uv_unit + 本地下标` |
| 5 | root 变换在蒙皮**之后**（§5.3） | 预乘进骨骼表会二次缩放 ⇒ 单独出一项断言 |
| 6 | 槽位色 + tint black（bit4） | 与 `SpineGpuShadeReference` 逐位；暗色是 float 不截断，负值也要过 |
| 7 | bit0 直 alpha 预乘 / bit1 additive 写 alpha=0 / bit5 世界层 sRGB→线性 | 与 `SpineGpuShadeReference` 逐位；bit5 必须在 bit1 清 alpha **之前**做 |
| 8 | 多实例拼包偏置 | "实例 k 在合包里 == 它单独成包"逐位（`SpineGpuBatch.h:17-19` 的同款断言） |
| 9 | CPU 裁剪顶点（b8） | **最小验证阶段标 out of scope**；G1 阶段用 `flags bit3` 全 0 的资产绕开 |
| 10 | ⚠️ **精度 / FMA**（移动端必过，PC 可免） | `precise` 申请不到（§5.6）⇒ 判据**降级为误差阈值**：量化最大绝对与相对误差，**不得大于同口径下 SSBO 路径的底噪**（§17 的取法：底噪由"无 deform"那格给出）。Vulkan 后端若保留 `precise`，则只在 Vulkan 上仍要求 bit-exact |
| 11 | ⚠️ **兜底轨 `tex2Dlod` 取 texel 的逐位性**（仅当 `texelFetch` 例外被驳回时启用） | 与 `texelFetch` 结果**逐位一致**；覆盖 `i = 0 .. W*H-1` 全量 + 边界（`W-1`、`H-1`）；必须满足：W/H 是 **2 的幂**、sampler 是 **Point**、纹理格式 fp32/u32。任一条件不成立即判 No-Go |

---

## 5. G3 代价量化（要出数）

基线用现成的实测值，不要重新发明：

| 项 | 现在（SSBO） | 去 SSBO 后要出的数 |
|---|---|---|
| 静态显存 | 中位 33.7 KB / p90 97.7 KB / 781 资产合计 **35.1 MB**（§17） | 纹理化 + padding 后同上三项；**阈值建议 ≤ 1.5×** |
| 每帧上传 | 中位 5.53 KB / p90 17.80 KB（§17） | `TexSubImage` 版；deform 现均值 122 B/条 |
| draw call | 4 管线 ×（跨资产可合批） | 去 SSBO ⇒ **每实例一 draw**；要实测典型界面同屏 Spine 数。⚠️ 对 OSGame 增量应为 0（§0.2：本来就是一骨架一 MeshRenderer，没有合批可丢）—— **这条必须实测确认，不能靠推理** |
| CPU | —— | texel 坐标换算的**整数除法**（`i % W`、`i / W`）；**W 取 2 的幂**改成移位与掩码，实测前后差 |

---

## 6. 落地顺序（含例外申请）

| 步 | 内容 | 产出 |
|---|---|---|
| S0 | **提例外申请**（§5.6：target 3.5 + `texelFetch`） | 批准 / 驳回结论 |
| S1 | G0 探针 + **G0' 例外项探针**（≥2 台真机） | 机型能力表 + 走 A 还是 B（array texture） |
| S2 | G1 最小 `.shader` + 3 个资产 | 能画出正确第一帧 |
| S3 | G2 第 1-8、10 项（驳回时加第 11 项） | bit-exact / 误差阈值结果 |
| S4 | G3 代价量化 | 四项数 vs 基线 |
| S5 | 决策 | Go ⇒ §5.4 转正并排期；No-Go ⇒ 回退路线 A（只做 ZEngine / PC） |

---

## 7. Go / No-Go 判据（写死，避免事后找理由）

- [ ] **例外**：§5.6 两项已批准，或驳回后兜底轨（第 11 项）验证通过
- [ ] **编译**：目标机型能编译、能跑（不是只在 PC 过）
- [ ] **逐位**：G2 第 1-8 项**全部 bit-exact**（走兜底轨时第 11 项也须过）
- [ ] **精度**：第 10 项最大相对误差 **≤ SSBO 路径同口径底噪**（否则对拍体系失效，比慢更糟）
- [ ] **内存**：纹理化后 **≤ 现在的 1.5×**
- [ ] **draw call**：不高于现状（OSGame 预期增量 0，要实测）

**任一不过 ⇒ No-Go。** 回退到 §5.1：SSBO 保持默认，GPU 路径只上 PC / 编辑器；
移动端维持 CPU 蒙皮现状，等 SSBO 可用或换方案。
