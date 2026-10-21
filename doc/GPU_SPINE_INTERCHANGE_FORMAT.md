# GPU-Spine 中间格式规格（ZEngine ⇄ Unity 共用）

> **状态：草案 v0.1 —— 先定规格，再写代码。**
> 本文只定义**数据格式与 shader 数学**，不涉及任何一端的实现。
> 目的：ZEngine（C++ 导入器）与 Unity（C# 导入器）产出**同一个容器**，
> 两端 shader 跑**同一条公式**，从而视觉一致、可互相对拍。
>
> **版本锁定：Spine 3.8**（已决策，见 §1.1）。
> 所有引用 spine-cpp 行为的地方均已核对
> `G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp`，行号见文末「事实来源」。

---

## 0. 为什么要有这一层

CPU 蒙皮下**每个实例必须持有自己的一份顶点缓冲**，因为各实例动画状态不同、顶点结果不同 ——
这是 N 份动态 VB 的根本原因。把顶点变换搬到 GPU 后，VB 里存的是**绑定姿势**（与实例无关），
于是可以按 SkeletonData **共享一份**，N 份塌缩成 1 份。

> **GPU 化不是"省算力"，而是"使共享成为可能"。** 只做 GPU 蒙皮但每个实例仍保留自己的 Mesh，
> 内存几乎不变甚至略增（静态 VB 多了骨骼槽）。收益的来源是 **共享 + 实例化**。

因此本规格的核心不是"怎么把矩阵传到 GPU"，而是**如何把"与实例无关"的部分和"与实例有关"的部分切干净**。

### 范围
- 静态几何（绑定姿势 + UV + 骨骼槽 + 权重）的布局
- 每帧每实例数据（骨骼表 + 槽位染色）的布局
- 两端必须一致的蒙皮公式
- 绘制项 / 混合模式 / 裁剪的处置

### 非范围（本规格刻意不解决）
- 动画解算（`AnimationState::update` + `Skeleton::updateWorldTransform` **永远在 CPU**。
  它是带约束的顺序遍历，IK / 物理 / path 是迭代求解器，并行要分层 barrier，收益微秒级）
- VAT 烘焙（**明确否决**：frames × verts × 3 张纹理，1000 顶点 × 100 帧 ≈ 1.2 MB/动画/资产。
  本项目的痛点是内存，VAT 会让内存暴涨，且换皮 / 动画混合 / IK / 挂点 / 裁剪全废）
- ClippingAttachment 的 GPU 化（多边形裁剪 + 三角化会**产生新三角形**，VS 做不了）

---

## 1. 术语

| 词 | 含义 |
|---|---|
| **asset** | 一份 `SkeletonData` + `Atlas` 的构建产物，按资产共享，**唯一一份静态 VB** |
| **instance** | 一个活着的骨架（有自己的 `AnimationState` / 骨骼表 / 槽位染色） |
| **draw item** | 一次可绘制的最小单位 = 一个附件的可见部分（4 顶点四边形，或一段网格） |
| **bone table** | 每帧每实例上传的骨骼世界变换数组，6 float/骨 |
| **rigid** | `_bones.size() == 0` 的附件：整块由**槽位骨骼**单一矩阵变换 |
| **weighted** | `_bones.size() != 0` 的附件：每顶点多个影响，每个影响**有自己的绑定坐标** |

---

## 1.1 版本锁定：Spine **3.8**（已决策 2026-10-21）

**决策**：GPU 化在 **3.8** 内做，升 4.x **单独排期**。理由不是"3.8 更好"，而是**可归因** ——
同一版本内做，回归能归因到"GPU 化"本身；版本升级 + 算法替换同时上，出问题无法二分。

**判据（本机已核对，`G:/OSGame/unity-engine-source/Modules/Spine/spine-cpp`）**

| 判据 | 出处 |
|---|---|
| `Vector<unsigned short> _bones`，源码注释原话：*"unsigned short is ample; a skeleton cannot reach 65535 bones."* | `VertexAttachment.h:79-81` |
| 带 `applyDeform(VertexAttachment*)` 与 `getId()` / `const int _id`（3.8 的 deform 重做：附件带 id，deform 可跨 skin 复用） | `VertexAttachment.h:66,69,86` |
| `computeWorldVertices` 同时提供 2 参与 6 参（start/count/offset/stride）两种重载 | `VertexAttachment.h:53-63` |

**这条锁定实际钉死了什么**

| 项 | 3.8 下的取值 | 出处 |
|---|---|---|
| 骨骼索引元素类型 | **`uint16`**（不是 uint32） | `VertexAttachment.h:81` |
| 单骨架骨骼数上限 | 65535（`uint16` 索引的天然上限，与源码注释同义） | 同上注释 |
| `_vertices` 每影响三元组步长 | 3 float `(vx, vy, weight)` | `VertexAttachment.cpp:97-112` |
| deform 偏移 | 每顶点 2 float，**与影响数无关**（`f = skip << 1`） | `VertexAttachment.cpp:115-131` |
| 混合模式枚举 | `Normal=0 / Additive / Multiply / Screen`，共 4 种 | `BlendMode.h:35-38` |
| 裁剪 | **只有 CPU 实现** | 见下 |

**顺带解决一项待拍板**：3.8 里裁剪**只有 CPU 实现** —— `SkeletonClipping::clipTriangles()` 会
**重写顶点 / 三角形 / UV**（`SkeletonClipping.h:47-49` 的 `clipTriangles`，`:52-54` 的 `getClippedVertices/Triangles/UVs`），
即裁剪会**产生新的顶点和新的三角形**，VS 无法表达。
所以 §13 的"Clip 走 CPU fallback"不是折中方案，而是 **3.8 下的唯一选项** —— 它从"待拍板"降级为**格式约束**：

> **带裁剪的槽位不进 GPU 路径**，走 CPU 生成的动态 VB（§7）。同一帧内两种路径可混跑。

**迁移风险（写在这里，因为以后一定会有人问）**
- 4.x 的 `_bones` 是 `Vector<int>` ⇒ 索引类型 uint16→uint32，顶点格式、骨骼表寻址、导入器全要动。
  ⚠️ **这一条未在本机核实**（本机无 4.x 源码），升级时必须重新核对，不要照抄。
- OSGame 项目里的 spine-unity 运行时是 **3.7**
  （`Client_proj/Project/Assets/OSGame/3rd-Party/Spine/version.txt` 已核对：
  *"works with data exported from Spine Editor version: 3.7.xx"*）。
  若将来要让 ZEngine 吃 OSGame 的导出资源，`.skel` 二进制版本与 3.8 不同，导入器需要版本分支。

---

## 2. 顶层：一个 asset = 一个容器

```
SpineGpuAsset
├─ Header              魔数 / 版本 / spine runtime 版本 / 各段偏移
├─ BoneTable           骨骼名（仅调试与挂点，不参与渲染）
├─ VertexBuffer        静态：绑定姿势 + UV + 骨骼槽 + 权重   ★ 按资产共享
├─ IndexBuffer         静态：uint16
├─ DrawItemTable       静态：按 draw order 排好的绘制项
├─ SlotTable           槽位 → 附件列表（换装 / 显隐用）
├─ AtlasPages          图集页引用（纹理句柄或路径）
└─ ClipData            裁剪多边形数据（保留给 CPU fallback）
```

运行时每帧每实例只有两块：

```
SpineGpuInstance
├─ BoneTable     6 float × boneCount        （每帧上传；多实例打包进同一个大 buffer）
└─ InstanceList  CPU 每帧紧凑化的可见绘制项列表（16 B/项）
```

---

## 3. 静态顶点缓冲

### 3.1 两种顶点格式（**不要统一成一种**）

spine-cpp 有两个完全不同的分支，GPU 端必须分别对应：

**rigid（`VertexAttachment.cpp:71-87`）** —— `_bones` 为空，
`_vertices` 是 `(x, y)` 对，整块由 `slot._bone` 一个矩阵变换：

```
worldVertices[w]     = vx * a + vy * b + x;
worldVertices[w + 1] = vx * c + vy * d + y;
```

**weighted（`VertexAttachment.cpp:97-112`）** —— 每顶点 `n` 个影响，`_vertices` 里
**每个影响一份三元组 `(vx, vy, weight)`**（步长 3），权重**不在 `_bones` 里**：

```
wx += (vx * bone._a + vy * bone._b + bone._worldX) * weight;
wy += (vx * bone._c + vy * bone._d + bone._worldY) * weight;
```

> ⚠️ **最容易写错的一点**：weighted 的绑定坐标是**每影响一份**，不是每顶点一份。
> 影响 0 和影响 1 的 `(vx, vy)` 分别在各自骨骼的局部空间里，**不相等**。
> 只给每顶点存一份 bindPos 会得到能编译、能跑、但形状全错的网格。

| 格式 | 布局（按偏移顺序） | 步长 |
|---|---|---|
| `kVtxRigid` | `bindPos f32×2` (0-7) + `uv f32×2` (8-15) | **16 B** |
| `kVtxWeighted4` | `bindPos f32×2 ×4` (0-31) + `uv f32×2` (32-39) + `boneIdx u16×4` (40-47) + `weight unorm8×4` (48-51) + `_pad` (52-55) | **56 B** |

> **为什么 bindPos 放在最前面、步长补到 56**：`ByteAddressBuffer::Load2`（一次取一个
> `float2`）要求偏移 **8 字节对齐**。步长 52 时第 k 个顶点在 `52k`，`52 % 8 == 4` ⇒
> 第二个及以后的顶点其 bindPos 无法用对齐的 Load2 取，只能退化成 8 次标量 Load。
> `bindPos` 放首位 + 步长 56（`56 % 8 == 0`）⇒ 每个顶点的 bindPos 都能一次 Load2。
> 代价 4 B/顶点（约 7%），换来的是 GPU 侧对齐的向量化读取 —— 这个取舍是由
> `doc/GPU_SPINE_INTERCHANGE_FORMAT.md` 的验收夹具实测出来的，不是拍的。

- **每段 VB 只有一种格式**，绘制项记录 `vertex_format`。合批只能在同一格式内进行。
- `MAX_INFLUENCES = 4`。导入器遇到 `n > 4` 的顶点必须**报错并报出附件名**，
  不允许静默截断（静默截断 = 形状悄悄错）。可选 `--reduce-influences` 保留权重最大的 4 个并重新归一化。
- 未使用的槽位：`weight = 0`、`boneIdx = 0`、`bindPos = 0`。
- **权重归一化**：导入期把 4 个 unorm8 调整到**和恰好 255**（余量补给权重最大的那个），
  运行时统一 `/ 255.0`。不要在 shader 里做除法归一化（每个顶点一次除法，且引入与 CPU 不一致的舍入）。
- **顶点色不在静态 VB 里** —— Spine 的颜色是**槽位色**（`slot.getColor()`），每实例可变，属于实例数据。

### 3.2 索引

`uint16`。Spine 附件极小，单个骨架超过 65535 顶点的情况用 `flags` 里的
`kIndex32` 打开 uint32 变体 —— 默认 uint16 是因为 `std::vector<uint32>` 索引在
Unity 原生模块里是实打实的浪费（~8 B/顶点）。

---

## 4. 绘制项表（静态）

按 `skeleton.getDrawOrder()` 的**槽位顺序**排好。每项：

```
DrawItem
  index_offset  u32
  index_count   u32
  vertex_format u8    (kVtxRigid / kVtxWeighted4)
  blend_mode    u8    (见 §8)
  atlas_page    u16
  slot_index    u16
  attachment_id u16
  kind          u8    (kStatic / kFallback)
  _pad          u8
```

合批键 = `(atlas_page, blend_mode, vertex_format, kind)`。
连续同键的绘制项可合成一次实例化绘制。

> **DrawOrderTimeline 例外**：运行时改绘制顺序的动画无法用静态表表达。
> 导入器检测到该时间轴就在 `flags` 置 `kHasDrawOrderTimeline`，
> 该资产**整个走 CPU 路径**（不要试图在 shader 里重排，代价远大于收益，且 UI Spine 极少用）。

---

## 5. 每帧每实例数据

### 5.1 骨骼表（核心）

每骨 6 个 `f32`，顺序固定：

```
struct Bone { float a, b, c, d; float worldX, worldY; };   // 24 B 紧凑
```

- 来自 `Bone::getA()/getB()/getC()/getD()/getWorldX()/getWorldY()`（`Bone.h:189-209`），
  即 `Skeleton::updateWorldTransform()` 之后的世界变换。
- **索引 = `skeleton.getBones()` 的下标**，必须与导入器一致。
- `bone_stride` 记录在 Header 里：
  - `24` —— 紧凑（structured buffer / SSBO / instance buffer）**默认**
  - `32` —— std140 UBO 兼容（vec4 + vec4，第二组后两分量留空）

> **不要用纹理传骨骼。** 纹理会额外吃显存，而本项目的痛点正是内存；
> 且纹理采样路径（归一化坐标 + 滤波）会引入与 CPU 不一致的舍入。
> 用 structured buffer / instance data。

- 所有实例的骨骼表**打包进同一个每帧大 buffer**，实例只带 `bone_base`（以骨为单位的下标）。
  这样不同骨架实例也能合进同一次绘制 —— 这是"十万个角色少数几个 draw call"的前提。

### 5.2 实例项（每可见绘制项一项，**20 B**）

```
DrawInstance
  draw_item_index u32     // 索引静态绘制项表
  bone_base       u32     // 该实例骨骼表在大 buffer 里的起始骨下标
  tint            u32     // RGBA8，槽位色（直 alpha，非预乘）
  dark_tint       u32     // RGBA8，tintBlack（slot.hasDarkColor() 才有意义）
  root_index      u32     // 索引每帧 root 表（单位是「根变换条目」，不是字节）
```

- 5 个 u32 = 20 B，**无尾部填充**（对齐刚好是 4）。
- `root_index` **追加在末尾**而不是紧跟 `bone_base` —— 保持既有四个字段的偏移不变，
  序列化代码不用整体挪位。
- 同一实例的多个绘制项（一个骨架有多个）共享同一个 `root_index` 与 `bone_base`。

**CPU 每帧紧凑化（compact）**：只把**当前可见**的绘制项追加进列表。
换装、槽位隐藏、裁剪走 fallback 的项都由 CPU 决定去留 ——
**shader 里不需要任何"是否可见"分支**。

### 5.3 根变换

实例的世界变换（UI 里的位置/缩放/旋转）作为额外的 2×3 仿射 `root`，
在蒙皮**之后**施加：`p = root.apply(skinned)`。
（不能预乘进骨骼表 —— 骨骼表存的是骨骼世界变换，乘进去会二次缩放。）

**存放位置（已拍板，§13.1 第 6 项）**：每实例一个 root，放进**每帧 root 表**，
由实例项的 `root_index` 索引。

```
RootTransform { float a, b, c, d; float tx, ty; }   // 24 B 紧凑
  x' = a*x + b*y + tx
  y' = c*x + d*y + ty
```

- 步长 24 B；若走 std140 UBO 变体则跟 `bone_stride` 一致改成 32。
- 与骨骼**完全同构**（都是 2×3 仿射、都是 24 B），但**语义不同**：
  一个是骨骼世界变换、一个是实例根变换。`SpineGpuFormat.h` 里保持成两个类型，
  防止互相换用 —— 换用了能编译、能跑，只是角色整体位移/缩放悄悄错。

---

## 6. Shader 数学（唯一权威公式）

以下伪代码是**两端必须逐字实现**的公式。ZEngine 与 Unity 的视觉一致性完全依赖它。

```hlsl
// bones: 全局骨骼 buffer（所有实例打包）
// inst : 当前实例项
float2 SkinVertex(float2 uvIgnored, uint drawItemIndex)
{
    float2 p = 0;

    if (vertex_format == kVtxRigid)
    {
        // 整块由槽位骨骼单一矩阵变换（对应 VertexAttachment.cpp:80-85）
        Bone B = bones[inst.bone_base + drawItem.slot_bone_index];
        p.x = bindPos.x * B.a + bindPos.y * B.b + B.worldX;
        p.y = bindPos.x * B.c + bindPos.y * B.d + B.worldY;
    }
    else  // kVtxWeighted4
    {
        // 逐影响累加，顺序 = 影响存储顺序（对应 VertexAttachment.cpp:102-110）
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float w = weight[i] / 255.0;
            Bone B = bones[inst.bone_base + boneIdx[i]];
            p.x += (bindPos[i].x * B.a + bindPos[i].y * B.b + B.worldX) * w;
            p.y += (bindPos[i].x * B.c + bindPos[i].y * B.d + B.worldY) * w;
        }
    }

    return RootApply(p);   // §5.3
}
```

三条硬性约束：

1. **混合的是"变换后的位置"，不是矩阵。**
   先 `Σ wᵢ · (Bᵢ · vᵢ)`，绝不能先 `Σ wᵢ · Bᵢ` 再变换 —— 两者数学上不等价，
   后者是新手最常见的错，而且不报错、只是形状微妙地不对。
2. **累加顺序必须与 spine-cpp 一致**（按影响下标升序遍历）。
   f32 加法不满足结合律，换序会引入 ulp 级差异，破坏与 CPU 路径的对拍。
3. **FMA**：编译器可能把 `vx*a + vy*b` 收缩成 `fma`，与 CPU 的"先乘后加"舍入不同。
   要求与 CPU 路径**逐位一致**时，必须关掉收缩（HLSL：`precise` 或拆成独立 mul/add；
   GLSL/Cg 侧同理）。只要求视觉一致时可放宽，容差见 §12。

---

## 7. 三类附件的处置

| 附件 | 静态 VB | 说明 |
|---|---|---|
| **Region** | 烘焙 4 顶点 | `RegionAttachment::computeWorldVertices` 用 `_vertexOffset[8]` 的 br/bl/ul/ur 四点，由**槽位骨骼**单一矩阵变换 ⇒ 天然是 rigid。UV 由图集页（可能带 90° 旋转）算好，烘焙进静态 UV。 |
| **Mesh（刚性）** | 烘焙，rigid | `_bones` 为空，整块由槽位骨骼变换。16 B/顶点。 |
| **Mesh（蒙皮）** | 烘焙，weighted4 | **56 B/顶点**（不是 52，见 §3.1 的对齐说明）。 |
| **Clipping** | `_ClipData` 保留 | `SkeletonClipping::clipTriangles()` 重写顶点/三角形/UV（`SkeletonClipping.h:47-54`）⇒ **产生新三角形，VS 做不了** ⇒ **CPU fallback**：该项走 `kFallback`，顶点由 CPU 算好上传每帧动态 VB。这是 3.8 的唯一选项，不是折中（§1.1）。 |
| **FFD / deform** | 同左 | `vx = vertices[b] + deform[f]`（`VertexAttachment.cpp:122-123`）。技术上可 GPU（deform 进纹理），但那样每帧又要传顶点数据，**收益被吃掉** ⇒ 只对真正在动 deform 的槽位走 CPU fallback。 |

Region 的 4 个顶点**烘焙进静态 VB**（而不是用共享单位四边形 + 每实例 offset/uv），
因为静态部分按资产共享、成本一次性，而每实例数据才是内存大头。

---

## 8. 混合模式与预乘 Alpha

`BlendMode.h:35-38`：`BlendMode_Normal = 0, Additive, Multiply, Screen`。

Spine 图集默认是 **PMA（预乘 Alpha）**，对应 Unity 侧 `Blend One OneMinusSrcAlpha`。
本规格规定：**图集按 PMA 导入，混合模式按上表映射**，`tint` 在 shader 里乘到 PMA 颜色上。

| Spine | ZEngine / ShaderLab | Unity |
|---|---|---|
| Normal (PMA) | `Blend One OneMinusSrcAlpha` | 同 |
| Additive | `Blend One One` | 同 |
| Multiply | `Blend DstColor Zero` | 同 |
| Screen | `Blend One OneMinusSrcColor` | 同 |

混合模式不同 ⇒ 必须分批（合批键的一部分）。

---

## 9. 容器二进制布局

```
Header (64 B)
  0x00  magic            u32   'ZSPG'  (0x5A535047)
  0x04  version          u32   本规格版本，当前 1
  0x08  spine_runtime    u32   产生它的 spine-cpp 版本（3.8.xx → 30800）
        ⚠️ 必须记录：3.8 的 _bones 是 u16，4.x 是 int，布局不同
  0x0C  flags            u32   kIndex32 / kHasDrawOrderTimeline / kUsesTintBlack / kHasClipping
  0x10  bone_count       u32
  0x14  slot_count       u32
  0x18  draw_item_count  u32
  0x1C  vertex_count     u32
  0x20  index_count      u32
  0x24  atlas_page_count u32
  0x28  bone_stride      u32   24 或 32
  0x2C  chunk_count      u32   由 writer 填（内存中未烘焙的 asset 里是 0）
  0x30  rigid_vertex_count      u32
  0x34  weighted_segment_offset u32   weighted 段在 vertex_buffer 里的字节起点，8 B 对齐
  0x38  weighted_vertex_count   u32
  0x3C  reserved        u32   0

ChunkTable  (chunk_count × 16 B)  { id u32, offset u32, size u32, _pad u32 }
Chunks:
  id=1  strings        骨骼名 → 槽位名 → 附件名 → 图集页名，每项 UTF-8 且 '\0' 结尾
  id=2  slot_bone      每槽位的骨骼下标（u16 × slot_count）—— rigid 用
  id=3  vertex_buffer  按 vertex_format 分段，段首 8 B 对齐
  id=4  index_buffer   uint16（或 flags 打开时 uint32）
  id=5  draw_items     DrawItem[]，20 B/项
  id=6  attachments    (slot_index u16, attachment_id u16) × attachment_count，换装用
  id=7  atlas_pages    图集页路径，每项 '\0' 结尾
  id=8  clip_data      裁剪多边形（CPU fallback 用）
```

**为什么 header 里要有 0x30..0x38 这三个字段**：顶点缓冲是 rigid 段 + weighted 段拼起来的，
光有 `vertex_count` 和 `draw_items` 无法反推出分界点 —— 读回来就切不开两段，
而 `SpineRigidVertex`(16 B) 与 `SpineWeightedVertex`(56 B) 步长不同，切错就全错。
所以段信息必须由 header 显式给出，不能靠消费端重算。

布局规则：
- 所有 chunk 起点 4 B 对齐；`vertex_buffer`(id=3) 起点 **8 B 对齐**（§3.1 的 `Load2` 前提）。
- 对齐填充插在 chunk 之间，**因此容器总长 ≠ header + 表 + 净载荷之和**。
  要校验容器，应校验 **chunk 表里的 `size` 之和**（与填充无关的精确不变量），
  而不是写死一个总字节数 —— 后者会在对齐规则一变时就假失败。
- 最后一个 chunk 的 `offset + size` 必须正好等于文件长度：不留尾部冗余，也没被截断。

---

## 10. 精度与坐标系

- **骨骼表、绑定坐标、UV 一律 `f32`。默认不启用 f16。**
  理由：本项目已在别处吃过 fp16 的亏；Spine 坐标量级是像素（可达 1e3~1e4），
  图集可达 4096²，f16 在 2048 附近就有 ~2 px 的 UV 误差。
  （如有明确收益，可作为 `flags` 里的可选变体另议，不在 v0.1 范围。）
- 坐标系：**骨架空间，y 向上，1 单位 = 1 像素**（Spine 原生约定）。
  不要为了减少一次运算而在导入期翻转 y —— 那会让 ZEngine 与 Unity 的 UV 对不上。
- 顶点最终位置 = `root.apply(skinned)`，再乘视图投影。

---

## 11. 版本与兼容

- `version` 变化 ⇒ 导入器必须拒绝旧容器并给出明确错误，不做静默兼容。
- `spine_runtime` 不匹配 ⇒ 警告但允许（布局差异只在 `_bones` 宽度，导入器已按 3.8 读；
  4.x 资产需要另一条导入路径，**不在本版本范围**）。
- 任何"导入器静默调整数据"的行为（截断影响数、重排顶点、自动降精度）都**必须报错**，
  不允许悄悄做。

---

## 12. 如何证明两端一致（验收清单）

1. **CPU ↔ GPU 对拍**：固定骨架 + 固定时间 `t`，
   把 `computeWorldVertices` 的结果与 GPU 回读结果逐顶点比较。
   - 逐位一致：需关 FMA 收缩（§6 约束 3）
   - 否则：容差 `1e-3` 相对误差，且坐标量级 < 1e4
2. **ZEngine ↔ Unity 对拍**：同一个容器、同一 `t`，两端截图 / 回读比对。
   骨架与图集必须同源（同一份 `.skel` + `.atlas`）。
3. **回归夹具**：至少一份含 **Region + 刚性 Mesh + 蒙皮 Mesh + tintBlack + 四种混合模式**
   的骨架，作为永久对拍资产。
4. **导入器负向用例**：`n > 4` 的顶点必须报错；`weights` 和不为 255 必须报错。
5. **两种输入必须对拍**：`.skel`（编辑器默认导出）与 json 是两条独立解析路径，
   同一副骨架经两条路径 Bake 后必须产出**逐字节相同**的容器。
   另：对 `.skel` 夹具的**每个真前缀**都调一次解析，必须全部失败 —— 它既没魔数也没长度字段，
   截断是最容易触发越界读的场景。

---

## 13. 决策状态

### 13.1 已拍板

| # | 问题 | 结论 | 落地处 |
|---|---|---|---|
| 1 | Spine 版本 | **3.8**，升 4.x 单独排期（理由：可归因，不是"3.8 更好"） | §1.1 |
| 3 | Clip 策略 | **从"策略"降级为"约束"**：3.8 只有 CPU 实现 ⇒ 带裁剪的槽位走 CPU 动态 VB，同帧可混跑 | §1.1、§7 |
| 6 | root 变换存在哪？ | **方案 A：实例项加 `root_index`（16 B → 20 B）**，root 存每帧 root 表，24 B/项。选它而不是 B/C 是因为它不改 `bone_base` 的语义，代价只是每绘制项 4 B | §5.2、§5.3 |
| 2 | `MAX_INFLUENCES = 4` 够吗？ | **定为 4**。导入器遇 `n > 4` **报错并报出附件名**，不静默截断 | §3.1 |
| 4 | 骨骼通道 structured 还是 UBO？ | **默认 structured / SSBO，24 B 紧凑**；仅当目标后端只有 UBO 时才用 32 B std140 变体（记录在 `bone_stride`） | §5.1 |

### 13.2 仍待拍板

**无。** 全部六项已拍板；后续若再开新议题写在这里。

是否需要给容器加 **skin 维度**？（见 §14）

---

## 14. 已知限制

### 14.1 容器里没有 skin 维度

导入器把**所有 skin 合并成一份**附件表（同槽位同名时**先出现的胜出**），
容器只有 `(slot_index, attachment_id)`，没有"这个附件属于哪个 skin"。
后果：

- 同名跨 skin 的附件（含最常见的 linked mesh 用法：skin2 里的 "eye" 链接
  到 default 里的 "eye"）会**塌成一个**，换装时拿不到另一份。
- 参考实现按 `(skin, slotIndex, parent)` 找父附件；我们按 `(槽位, 名字)` 找。

要不要修是**规格级**的决定（容器要加 skin 表 / 附件表要多一列），不是导入器
能自己定的。当前行为有测试钉住，不是意外。

### 14.2 linked mesh 的几何是复制的，不是共享的

linked mesh 继承父附件的顶点 / 骨骼 / 三角形 / regionUV，但用**自己的**图集
region 重算 UV。我们的顶点格式把 UV 编在顶点里 ⇒ UV 不同就没法共用顶点，
只能再来一份。一个典型的换装角色会有 N(skin) × M(mesh) 份几何。

想省这块要么把 UV 拆成独立通道（顶点只存位置 + 骨骼，UV 按 draw item 索引），
要么让导入器按"顶点 + UV 完全相同的附件"去重 —— 两者都是另一项改动。

（历史：#1 版本、#2 影响数、#3 Clip、#4 骨骼通道、#5 root 是否 per-instance、#6 root 存放方式。）

---

## 附：事实来源（已核对）

| 断言 | 出处 |
|---|---|
| rigid 分支：整块由 `slot._bone` 单矩阵变换 | `spine-cpp/src/spine/VertexAttachment.cpp:71-87` |
| weighted 分支：`_bones` 每顶点 `[n, idx…]`，`_vertices` 每影响 `(vx, vy, weight)` 步长 3 | 同文件 `:89-113` |
| deform：`vx = vertices[b] + deform[f]` | 同文件 `:114-131` |
| Region 四点 br/bl/ul/ur 来自 `_vertexOffset[8]` | `spine-cpp/src/spine/RegionAttachment.cpp` |
| 混合模式 4 种 | `spine-cpp/include/spine/BlendMode.h:35-38` |
| 骨骼世界变换 getter | `spine-cpp/include/spine/Bone.h:189-209` |
| 槽位顺序 / 骨骼顺序 | `spine-cpp/include/spine/Skeleton.h:174-180` |
| `_bones` 声明为 `Vector<unsigned short>`（3.8 判据，注释："a skeleton cannot reach 65535 bones"） | `VertexAttachment.h:79-81` |
| 3.8 deform 重做：`applyDeform(VertexAttachment*)` + `getId()` / `const int _id` | `VertexAttachment.h:66,69,86` |
| `computeWorldVertices` 两种重载（2 参 / 6 参带 stride） | `VertexAttachment.h:53-63` |
| rigid 公式精确行号（`:83` = x，`:84` = y） | `VertexAttachment.cpp:71,83-84` |
| weighted 非 deform 块 `:97-112`；deform 块 `:115-131`，`vx` 在 `:122` | `VertexAttachment.cpp` |
| 裁剪是 CPU 几何重写 | `SkeletonClipping.h:47-49`（`clipTriangles`）、`:52-54`（`getClippedVertices/Triangles/UVs`） |
| OSGame 侧 spine-unity 运行时是 **3.7** | `Client_proj/Project/Assets/OSGame/3rd-Party/Spine/version.txt` |

### 二进制 `.skel` 的出处（导入器两种输入之一）

`.skel` 是 Spine 编辑器的**默认**导出格式，所以导入器必须支持它，不能只读 json。
字段顺序全部照抄 `spine-cpp/src/spine/SkeletonBinary.cpp`：

| 断言 | 出处 |
|---|---|
| 顶层：hash 串 → version 串 → width/height → `nonessential` | `:132-148` |
| ⚠️ **文件没有魔数**，开头就是 hash 串 | `:132`（传错文件不会立刻失败，会顺着 varint 读飞） |
| bones：父下标**只在 `i != 0` 时**读，且指向已读过的骨骼 | `:150-171` |
| slots：dark 四分量**无条件读**，不全是 `0xff` 才算有 dark color | `:173-193` |
| ⚠️ region 字段顺序 = rotation,x,y,scaleX,scaleY,width,height | `:530-536`（**与 json 的 x,y,width,height,rotation 不同**） |
| mesh 顺序 = path,color,vertexCount,regionUVs,triangles,vertices,hull | `:553-582` |
| `readVertices`：先一个 bool 决定 weighted；weighted 时每顶点 `[n, (idx,x,y,w)×n]` | `:658-691` |
| varint = LEB128，最多 5 字节；`optimizePositive=false` 时是 zigzag | `:467-489` |
| 字符串：varint 长度**含**结尾 `'\0'`，但结尾不落盘；长度 0 = 空串 | `:413-424` |
| 大端：`readInt` / `readFloat`（int32 按位重解释）/ `readShortArray` | `:449-458`、`:708-717` |
| 附件类型枚举 0..6（region,bbox,mesh,linkedmesh,path,point,clipping） | `include/spine/AttachmentType.h:34-42` |
| 动画里 7 段顺序：slot/bone/IK/transform/path/deform/**drawOrder**/event | `:725-1107` |
| ⚠️ 事件时间轴每帧末尾是否多 2 个 float，**取决于对应事件有没有 audioPath** | `:1098-1101` ⇒ events 段不能跳过 |
| curve：0=linear、1=stepped 无数据；2=bezier 带 4 个 float | `:1112-1127` |
| linked mesh：`setParentMesh` 拷父的 _vertices/_bones/_regionUVs/_triangles/_hullLength | `MeshAttachment.cpp:226-239` |
| linked mesh 的 UV = **父的 regionUVs × 自己的 region** | `MeshAttachment.cpp:64-92`（`updateUVs` 读自己的 `_regionU/V/…`） |
| ⚠️ `configureAttachment` 是**空函数**，region 在 `newMeshAttachment` 里就设好了 | `AtlasAttachmentLoader.cpp:120-122`、`:73-98` |
| json 里 linked mesh 有两种写法：`type:"linkedmesh"`，或 `type:"mesh"` 带 `"parent"` | `SkeletonJson.cpp:462-465`、`:538` |
| linked mesh 解析：按 skin 找父附件，找不到报 "Parent mesh not found" | `SkeletonBinary.cpp:302-323`、`SkeletonJson.cpp:649-669` |
| `inheritDeform` 是运行时概念（deform 时间轴是否作用于继承方） | `MeshAttachment.cpp:94-96` |

**`.skel` 与 json 必须产出同一个容器。** 这一条有一个硬测试钉住
（`SpineGpuImportTest` 第 9 节）：手写的 `.skel` 字节流夹具与 json 夹具内容等价，
两条解析路径各自 Bake 后**顶点缓冲逐字节相同**。二进制那边的字段顺序、大端、varint、
weighted 扁平编码只要错一处，两边就会分叉 —— 这是唯一能自动发现这类错误的手段。

**越界读的守卫**：`.skel` 既没有魔数也没有长度字段，截断文件是最容易触发越界读的场景。
测试对夹具的**每一个真前缀**都调一次解析，要求全部失败（不能"假装成功"，更不能越界读）。
