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
- 静态几何（绑定姿势 + 骨骼槽 + 权重）的布局，以及**独立 UV 通道**（§3.2）
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

**⚠️ 2026-10-22 修订：导入器同时接受 3.7.x**（`ClassifySpineVersion()`，4.x 仍明确拒绝）。
理由不是改了决策，而是**3.7 与 3.8 的 `.skel` 字节布局逐段比对完全一致**
（骨骼 / 槽位 / IK / transform / path / 皮肤 / 附件 / 事件 / 动画），
唯一的差别是槽位暗色判据：

| | 判据 | 出处 |
|---|---|---|
| 3.7 | `int darkColor = ReadInt(input); if (darkColor != -1) hasSecondColor = true;` | 3.7 `spine-csharp/src/SkeletonBinary.cs:190-192` |
| 3.8 | 读 4 字节，`!(r==ff && g==ff && b==ff && a==ff)` 才算有暗色 | `spine-cpp/src/spine/SkeletonBinary.cpp:186-189` |

⇒ **同一个解析器 + 一个版本分支**，不是两套。容器的 `spine_runtime` **写实** 30700 / 30800
（把 3.7 记成 30800 会让读端按 3.8 的判据解释暗色，而那个差别**不报错**）。
本工程 OSGame 的 782 个资产**全是 3.7.83**，此前 0 个能导入。

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
| deform 长度 | weighted **每影响 2 float**、rigid 每顶点 2 float（`deformLength = verts.size()/3*2`） | `SkeletonJson.cpp:1071`、`SkeletonBinary.cpp:976` |
| 混合模式枚举 | `Normal=0 / Additive / Multiply / Screen`，共 4 种 | `BlendMode.h:35-38` |
| 裁剪 | **只有 CPU 实现** | §1.1 下文、§7.2 |

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
├─ VertexBuffer        静态：绑定姿势 + 骨骼槽 + 权重       ★ 按资产共享、逐字节去重
├─ UvBuffer            静态：float2 × uv_count              ★ 独立通道，见 §3.2
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
| `kVtxRigid` | `bindPos f32×2` (0-7) | **8 B** |
| `kVtxWeighted4` | `bindPos f32×2 ×4` (0-31) + `boneIdx u16×4` (32-39) + `weight unorm8×4` (40-43) + `_pad` (44-47) | **48 B** |

> ⚠️ **UV 不在这里。** 顶点只存"几何 + 蒙皮"，UV 在独立的 chunk 4 里
> （`float2` × `uv_count`，8 B/条），由 `DrawItem::uv_offset` 索引。见 §3.2。

> **为什么 bindPos 放在最前面、步长补到 48**：`ByteAddressBuffer::Load2`（一次取一个
> `float2`）要求偏移 **8 字节对齐**。步长 44 时第 k 个顶点在 `44k`，`44 % 8 == 4` ⇒
> 第二个及以后的顶点其 bindPos 无法用对齐的 Load2 取，只能退化成标量 Load。
> `bindPos` 放首位 + 步长 48（`48 % 8 == 0`）⇒ 每个顶点的 bindPos 都能一次 Load2。
> 代价 4 B/顶点（约 9%），换来的是 GPU 侧对齐的向量化读取 —— 这个取舍是由验收夹具
> 实测出来的，不是拍的。

- **每段 VB 只有一种格式**，绘制项记录 `vertex_format`。合批只能在同一格式内进行。
- `MAX_INFLUENCES = 4`。导入器遇到 `n > 4` 的顶点必须**报错并报出附件名**，
  不允许静默截断（静默截断 = 形状悄悄错）。可选 `--reduce-influences` 保留权重最大的 4 个并重新归一化。
- 未使用的槽位：`weight = 0`、`boneIdx = 0`、`bindPos = 0`。
- **权重归一化**：导入期把 4 个 unorm8 调整到**和恰好 255**（余量补给权重最大的那个），
  运行时统一 `/ 255.0`。不要在 shader 里做除法归一化（每个顶点一次除法，且引入与 CPU 不一致的舍入）。
- **顶点色不在静态 VB 里** —— Spine 的颜色是**槽位色**（`slot.getColor()`），每实例可变，属于实例数据。

### 3.2 UV 是独立通道（**不在顶点里**）

UV 存在单独的 `uv_buffer`（chunk 4）：`float2` × `uv_count`，8 B/条。
绘制项带 `uv_offset`（单位是**UV 条目**，不是字节）；第 k 个顶点的 UV 在
`uv_buffer[uv_offset + vertexIndex]`，`vertexIndex` 由索引缓冲给出。

**为什么要拆开**：让**几何相同的附件共用同一段顶点**。两类最常见的重复：

- **linked mesh**（附属网格）：继承父附件的顶点 / 骨骼 / 三角形 / regionUV，
  但用**自己的**图集 region 重算 UV。
- **换装变体**：不同皮肤往往只换贴图 region，网格一模一样。

UV 编在顶点里时，UV 不同 ⇒ 顶点不能共用 ⇒ 只能整份复制（旧 §14.2）。
拆开之后顶点池按**逐字节完全相同**去重，UV 各取各的一份。

- **顶点数 ≠ UV 条目数**（顶点被共用时 UV 更多）。不要拿 `vertex_count` 去算 UV 数组长度，
  用 chunk 4 的大小 / 8。
- 代价：shader 里多一次间接读取（先取 `indexBuffer[...]` 得到顶点下标，再取 UV）。

**去重只认"完全相同"，绝不做近似匹配**：近似意味着"我觉得这两个差不多"，
差一点点形状就静默错，且没有任何报错。宁可少共享，不可错共享。
（实现用 FNV-1a 哈希**缩小比较范围**，判定始终是逐字节比较 —— 哈希撞了也不会出错。）

### 3.3 索引
`uint16`。Spine 附件极小，单个骨架超过 65535 顶点的情况用 `flags` 里的
`kIndex32` 打开 uint32 变体 —— 默认 uint16 是因为 `std::vector<uint32>` 索引在
Unity 原生模块里是实打实的浪费（~8 B/顶点）。

---

## 4. 绘制项表（静态）

按 (皮肤, `skeleton.getDrawOrder()` 的**槽位顺序**) 排好 —— 先 default 皮肤的全部附件，
再下一个皮肤；皮肤内按槽位顺序。每项：

```
DrawItem                     28 B（v6 起；v5 是 24 B）
  index_offset  u32   @0
  index_count   u32   @4
  uv_offset     u32   @8   （单位：**UV 条目**，不是字节；见 §3.2）
  vertex_format u8    @12  (kVtxRigid / kVtxWeighted4)
  blend_mode    u8    @13  (见 §8)
  atlas_page    u16   @14
  slot_index    u16   @16
  attachment_id u16   @18
  skin_id       u16   @20
  kind          u8    @22  (kStatic / kFallback)
  _pad          u8    @23
  clip_index    u16   @24  chunk 9 的行号；**没被裁时是 kNoClipIndex = 0xFFFF**（见 §7.2）
  _pad2         u16   @26
```

**没有 `skin_id` 就没法换装**：同一个 (槽位, 附件名) 在不同皮肤下是两个绘制项，
光靠 `slot_index + attachment_id` 分不出该画哪一个。
**没有 `uv_offset` 就没法共用顶点**：几何相同的附件共用同一段顶点，UV 各取各的（§3.2）。

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
float2 SkinVertex(uint vertexIndex, uint drawItemIndex)   // UV 不参与，见 §3.2
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
| **Region** | 烘焙 4 顶点 | `RegionAttachment::computeWorldVertices` 用 `_vertexOffset[8]` 的 br/bl/ul/ur 四点，由**槽位骨骼**单一矩阵变换 ⇒ 天然是 rigid。UV 由图集页（可能带 90° 旋转）算好，写进独立 UV 通道（§3.2）。 |
| **Mesh（刚性）** | 烘焙，rigid | `_bones` 为空，整块由槽位骨骼变换。**8 B/顶点**。 |
| **Mesh（蒙皮）** | 烘焙，weighted4 | **48 B/顶点**（不是 44，见 §3.1 的对齐说明）。 |
| **Clipping** | `_ClipData` 保留 | `SkeletonClipping::clipTriangles()` 重写顶点/三角形/UV（`SkeletonClipping.h:47-54`）⇒ **产生新三角形，VS 做不了** ⇒ **CPU fallback**：该项走 `kFallback`，顶点由 CPU 算好上传每帧动态 VB。这是 3.8 的唯一选项，不是折中（§1.1）。**回退的粒度是槽位**（不是整个资产），作用域见 §7.2。 |
| **FFD / deform** | 同左 | 顶点每帧都变 ⇒ 静态 VB 里那份**绑定姿势**不算数 ⇒ 该项走 `kFallback`。见 §7.1。 |

Region 的 4 个顶点**烘焙进静态 VB**（而不是用共享单位四边形 + 每实例 offset/uv），
因为静态部分按资产共享、成本一次性，而每实例数据才是内存大头。

### 7.1 deform（FFD）

deform 时间轴命中的附件，顶点**每帧都不一样**。静态 VB 存的是绑定姿势，
所以这些绘制项必须走 **CPU fallback**（`SpineDrawItem::kind = kFallback`），
并在 `flags` 置 `kHasDeform`。

技术上可以把 deform 传进纹理、在 VS 里加，但那样每帧都要传顶点数据 ——
**"共享一份静态 VB"这个唯一收益正好被它吃掉**（§0），所以不做。

**只回退被命中的绘制项，不是整个资产。** 与 `kHasDrawOrderTimeline`（那个是整体走 CPU）
是两回事。

#### 两种附件的 deform 语义**不一样**（照错就是静默错形状）

| | 存的是什么 | 运行时怎么用 | 长度 |
|---|---|---|---|
| **rigid** | **绝对**局部坐标（setup + 偏移） | 整体**替换** `_vertices` | 每顶点 2 float |
| **weighted** | **偏移** | 逐影响**相加**：`vx = vertices[b] + deform[f]` | 每**影响** 2 float |

出处：`VertexAttachment.cpp:72-73`（rigid 替换）、`:122-123`（weighted 相加）、
`SkeletonJson.cpp:1071` / `SkeletonBinary.cpp:976`（长度公式）。

⚠️ weighted 是**每影响 2 个**，不是每顶点 2 个。长度公式里的 `verts.size()/3`
除的是**已剥掉 n 的**每影响三元组 ⇒ `size/3` = 总影响数。运行时也对得上：
`f = skip << 1` 里的 `skip` 累加的是 `n`（影响数），循环里 `f += 2` 是**逐影响**
（`VertexAttachment.cpp:115-127`）。

#### inheritDeform 必须沿 linked mesh 传播

判据照抄 `MeshAttachment::applyDeform`（`MeshAttachment.cpp:94-95`）：

```
this == source || (_inheritDeform && _parentMesh == source)
```

⇒ 父附件被 FFD 动到时，**开了 inheritDeform 的继承方顶点同样每帧变**，得一起回退。
linked mesh 的父**也可以**是 linked mesh，所以传播要循环到不动点。
json 里这个开关的字段名是 **`"deform"`**（不是 `"inheritDeform"`），默认 1
（`SkeletonJson.cpp:572`）；二进制是紧跟 parent 的一个 bool（`SkeletonBinary.cpp:594`）。

#### 导入期必须挡住的四件事

参考实现在这四处**都不安全**，导入器一律**报错**而不是照抄：

| 情况 | 参考实现的行为 |
|---|---|
| deform 引用的皮肤不存在 | `findSkin` 返回 NULL 后紧接着 `skin->getAttachment(...)` ⇒ **空指针**（`SkeletonJson.cpp:1051`）；二进制是 ` _skins[i]` 不检查下标（`:959`） |
| deform 引用的附件不存在 | 这个**倒是**报错："Attachment not found"（`:1061-1065` / `:966-970`） |
| deform 指向 **region** | **无检查的** `static_cast<VertexAttachment*>`（`:1067` / `:972`）；而 3.8 的 `RegionAttachment` 继承自 `Attachment`、**不是** `VertexAttachment`（`RegionAttachment.h:46`）⇒ **未定义行为**。运行时有 RTTI 检查，会**静默不生效**（`DeformTimeline.cpp:72-74`） |
| 帧的 `offset + 顶点数` 超过 deform 长度 | `deformed[v] = ...` 无边界检查 ⇒ **越界写**（`:1092-1099`） |

### 7.2 裁剪作用域（per-slot）

裁剪也是"画不进静态 VB"（`clipTriangles()` 会重写顶点 / **三角形** / UV，
三角形个数都变了，`SkeletonClipping.h:47-54`），但它的作用范围是**一批槽位**，
不是某一个附件 ⇒ 导入期必须先把作用域算出来，再把作用域内的绘制项标成 `kFallback`。

作用域是 **(clipSlot, endSlot]**，两头都不对称：

| 位置 | 是否被裁 | 出处 |
|---|---|---|
| 裁剪附件**自己**那个槽位 | **不裁**（它压根不画） | `SpineMesh.h:153-159`：命中 clipping ⇒ `clipStart()` 然后 `continue` |
| 它**之后**的槽位 | 裁 | `:189-197` `isClipping()` 为真就走 `clipTriangles()` |
| **endSlot** | **含在内** | `:257` 槽位收尾调 `clipEnd(slot)`，而它只在 `_clipAttachment->_endSlot == &slot._data` 时收尾（`SkeletonClipping.cpp:72-76`） |
| endSlot 之后 | 不裁 | 同上 |

三条容易漏、漏了就**静默**错的规则：

1. **裁剪附件那个槽位被 `continue` 掉了**，连槽位收尾的 `clipEnd(*slot)` 也一起跳过了
   （`:159` 的 continue 在 `:257` 之前）。⇒ **end 写成"自己或更靠前的槽位"时，
   `active_end == s` 永远不成立**，这次裁剪只能靠整条绘制指令末尾那次无条件
   `clipEnd()` 收尾（`:259`）⇒ **一路裁到最后一个槽位**，且不报任何错。
2. **嵌套裁剪被忽略**：已经有一个裁剪在跑时 `clipStart()` 直接返回 0
   （`SkeletonClipping.cpp:49-51`）⇒ 后面那条裁剪**永远开不起来**，
   哪怕它的 endSlot 更靠后，也不会在前者结束后补上。
   ⇒ 作用域**不能**用"存在某条 clip 覆盖这个槽"的宽松判定，必须把槽位序列走一遍。
3. **作用域是逐皮肤的**：裁剪附件本身是某皮肤下某槽位的附件，3.8 同时只有一个皮肤生效
   （容器里 `skin_id` 是绘制项的键之一）⇒ 换到没有它的皮肤时作用域不存在。

#### 导入期从两种输入里取作用域（两者**不对称**）

| 输入 | 结束槽位的表示 | 参考实现 | 本导入器 |
|---|---|---|---|
| json | 槽位**名**（`"end"`） | `findSlot()` 的返回值**不检查**（`SkeletonJson.cpp:631-635`）⇒ 槽位名写错 / 缺失时 `_endSlot` 静默变 NULL ⇒ 作用域从"到某槽"变成"到末尾" | **报错**（报出附件名 + 槽位名）；**缺 `"end"` 则是合法的** ⇒ 记为"裁到最后一个槽位" |
| `.skel` | 槽位**下标**（varint，**在 vertexCount 之前**） | 有显式边界检查，越界报 "parse clip attachment failed!"（`SkeletonBinary.cpp:641-645`） | 同一条检查，`< 0` 也显式判（不依赖 `int >= size_t` 的整型提升方向） |

#### 容器里存什么

**作用域本身不落盘。** 它已经完整表达成"作用域内绘制项的 `kind = kFallback`"，
运行时不需要再算一遍。

**裁剪多边形要落盘**（chunk 9 `clip_data`，v6 起）。它和 deform **不同**：
deform 是逐帧动画数据（来自时间轴，永远不该烘焙），而裁剪多边形是
**静态资产几何** —— 局部坐标 + 骨骼绑定，和 mesh 顶点同源
（`ClippingAttachment : VertexAttachment`），不烘焙的话运行时想"只靠容器渲染、
不再持有 spine 源数据"就不成立。

```
ClipPolygon                  12 B（chunk 9，一条/裁剪附件）
  slot_index    u16   @0   裁剪附件所在槽位（调试/校验用）
  skin_id       u16   @2   作用域是逐皮肤的
  vertex_format u8    @4   kRigid / kWeighted4
  _pad          u8    @5
  vertex_count  u16   @6
  vertex_offset u32   @8   **段内顶点下标**，不是字节偏移（见 §3.1 的 stride）
```

⚠️ **多边形顶点复用主顶点缓冲**（chunk 3），不另开一块：`ClippingAttachment` 就是
`VertexAttachment`，编码方式与 mesh 顶点**完全相同**，也走同一个 `ExactBlobPool`
逐字节去重 ⇒ header 不需要新增字段，仍是 64 B。
被裁的绘制项通过 `clip_index` 指到这张表的行号；**没被裁时必须是 `kNoClipIndex = 0xFFFF`，
不能是 0 —— 0 是一条真实存在的多边形**，填 0 会让运行时拿第一条多边形去裁根本不该裁的项，
而且不报错。

⚠️ `vertex_offset` 的单位是**顶点下标**不是字节。只有一条多边形时它落在段首，
`0 × stride` 还是 0，两种解释逐字节相同 ⇒ 写错了测不出来；验收必须用到**第二条**
多边形（偏移非 0）才能钉住（见 §12 第 12 项）。

⚠️ **`kHasDeform` 只由 deform 点亮，不由裁剪点亮。** 两个位都表示"有绘制项不完全走
GPU 路径"，但语义与**代价**都不同：
`kHasDeform` = 顶点**逐帧**变，但变的只是**蒙皮之前的局部空间加数**（`VertexAttachment.cpp:122-123`
`vx = (*vertices)[b] + (*deformArray)[f]`）⇒ v7 起走 **deform 增量**，共享 VB 不动、蒙皮仍在 GPU
（`doc/GPU_SPINE_V7_ANIMATION.md` §5.2；**不再是**全量 CPU 动态 VB）；
裁剪则是 `SkeletonClipping::clipTriangles()` 重写顶点/三角形/UV ⇒ 只能 CPU 生成动态 VB，
且只在裁剪多边形动的时候才需要重算。合成一个位，运行时会为纯裁剪的资产
每帧重算一份根本没变的顶点。

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
  0x04  version          u32   本规格版本，当前 6
                               v2 = 加了 skin 维度（§9.1）
                               v3 = UV 拆成独立通道 + chunk id 重排（§3.2、§14.2）
                               v4 = deform 参与烘焙（§7.1）—— 布局没变，
                                    但带 FFD 的绘制项 kind 从 kStatic 变成 kFallback
                               v5 = 裁剪作用域参与烘焙（§7.2）—— 布局没变，
                                    但作用域内绘制项的 kind 从 kStatic 变成 kFallback
                               v6 = 裁剪多边形参与烘焙（§7.2）—— 绘制项 24 B → 28 B
                                    （新增 clip_index），chunk 9 起真正写数据
  0x08  spine_runtime    u32   产生它的 spine-cpp 版本（3.8.xx → 30800）
        ⚠️ 必须记录：3.8 的 _bones 是 u16，4.x 是 int，布局不同
  0x0C  flags            u32   kIndex32 / kHasDrawOrderTimeline / kUsesTintBlack / kHasClipping
                               / kHasDeform（有绘制项因 FFD 走了 CPU fallback，见 §7.1）
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
  0x3C  skin_count      u32   （v1 这里是 reserved）

ChunkTable  (chunk_count × 16 B)  { id u32, offset u32, size u32, _pad u32 }
Chunks:
  id=1  strings        骨骼名 → 皮肤名 → 槽位名 → 附件名 → 图集页名，每项 UTF-8 且 '\0' 结尾
  id=2  slot_bone      每槽位的骨骼下标（u16 × slot_count）—— rigid 用
  id=3  vertex_buffer  按 vertex_format 分段，段首 8 B 对齐（**不含 UV**）
  id=4  uv_buffer      float2 × uv_count，8 B/条，段首 8 B 对齐（§3.2）
  id=5  index_buffer   uint16（或 flags 打开时 uint32）
  id=6  draw_items     DrawItem[]，28 B/项（含 uv_offset + skin_id + clip_index）
  id=7  attachments    (slot_index u16, attachment_id u16, skin_id u16, _pad u16) × attachment_count
                       —— 8 B/项，换装查的就是这张表
  id=8  atlas_pages    图集页路径，每项 '\0' 结尾
  id=9  clip_data      ClipPolygon[]，12 B/条（§7.2）；**顶点本身在 chunk 3 里**。
                       没有裁剪时这个 chunk **根本不写**（不是写个空块）
```

> ⚠️ **chunk id 在 v3 重排过**：`uv_buffer` 插在 `vertex_buffer` 与 `index_buffer` 之间，
> 其后所有 id 顺移一位。读端按 **id** 找 chunk，**不许按下标** —— v2 的容器用 v3 读端
> 读会拿到 `index_buffer` 当 UV，静默错位。这也正是 `kFormatVersion` 升到 3 的原因。

### 9.1 skin 维度（v2）

容器里的"一个附件"= **(皮肤, 槽位, 附件名)** 三元组，不再跨皮肤合并。
换装的查询是：给定 `(skin_id, slot_index)` → `attachment_id` → 绘制项。

- **`skin_id` 0 恒为 default 皮肤。** 二进制格式的 default 皮肤就是先读的那一个
  （`SkeletonBinary.cpp:276`，名字写死 `"default"`）；编辑器导出的 json 里 default 也总排第一。
  但**导入器会显式检查**：名为 `default` 的皮肤若不在首位就**报错**，绝不静默挪位 ——
  挪了默认外观就变成另一个皮肤，而外观错没有任何报错。
- 字符串表里皮肤名紧跟骨骼名之后，读端靠 `header.skin_count` 切分。
- 代价：**同一个 (槽位, 附件名) 在 N 个皮肤下仍是 N 份附件记录**，但 v3 之后
  **几何不再跟着翻 N 倍** —— 顶点逐字节去重，跨皮肤只多一份 UV（§3.2、§14.2）。
  剩下的 N 份只是绘制项 + 附件记录 + UV 条目，是"能表达换装"的必要成本，不是浪费。

**为什么 header 里要有 0x30..0x38 这三个字段**：顶点缓冲是 rigid 段 + weighted 段拼起来的，
光有 `vertex_count` 和 `draw_items` 无法反推出分界点 —— 读回来就切不开两段，
而 `SpineRigidVertex`(8 B) 与 `SpineWeightedVertex`(48 B) 步长不同，切错就全错。
所以段信息必须由 header 显式给出，不能靠消费端重算。

布局规则：
- 所有 chunk 起点 4 B 对齐；`vertex_buffer`(id=3) 与 `uv_buffer`(id=4) 起点 **8 B 对齐**
  （§3.1 的 `Load2` 前提；UV 是 `float2`，同样要 8 B）。
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
- **v7 在设计中**：骨骼树（chunk 10）+ 动画（chunk 11-14），解算器自写、仍在 CPU。
  字段设计与转录 gap 分析见 **`doc/GPU_SPINE_V7_ANIMATION.md`**。
  v7 **不动** header（仍 64 B）、不动 chunk 1..9、不动 §5 每帧实例数据、不动 §6 shader 数学；
  新增的计数一律从 chunk 表推导（`size / stride`），不进 header。
- `spine_runtime` 不匹配 ⇒ 警告但允许（布局差异只在 `_bones` 宽度，导入器已按 3.8 读；
  4.x 资产需要另一条导入路径，**不在本版本范围**）。
- 任何"导入器静默调整数据"的行为（截断影响数、重排顶点、自动降精度）都**必须报错**，
  不允许悄悄做。

---

## 11.1 两端各有一份实现（C++ / C#）

格式的**权威定义**在 `SpineGpuFormat.h`（C++）。C# 侧是**照抄**，不是"参考"：

| 端 | 位置 |
|---|---|
| C++ | `Function/Render/Spine/SpineGpuFormat.h`（布局 + static_assert）<br>`SpineGpuImporter.cpp`（导入 + `Write/ReadSpineGpuContainer`） |
| C# | `Function/Render/Spine/CSharp/`（`SpineGpuFormat.cs` / `SpineGpuContainerIo.cs`，netstandard2.1，可整目录拷进 Unity Assets） |

C++ 每个偏移都有 `static_assert` 钉着（布局错一个字节不报错，只是形状静默错）。
C# **没有编译期 `offsetof`**，所以换三道关：

1. 偏移写成**命名常量**（`SpineGpuOffsets`），读写两端共用同一组 ⇒ 不可能半边漂。
2. `SpineGpuLayout.Validate()` 在启动时跑一遍常量自洽检查 —— 等价于 static_assert 那一层。
3. 真正的钉子在对拍：C++ 写的字节，C# 读回来再写一遍必须**逐字节相同**。

⚠️ **round-trip 只能抓"大小"漂，抓不到"解读"漂。** 读与写用同一组常量时，
把 `clip_index` 与 `pad2` 的偏移对调，字节照样自洽（因为对称）。
⇒ 必须有**语义断言**（哪些项被裁、多边形顶点落在哪、骨骼索引是不是 0/1）。

跨语言对拍怎么跑：

```
cmake -B build -DZENGINE_BUILD_SPINE_IMPORT_TEST=ON
cmake --build build --target SpineGpuImportTest --config Debug
ctest -C Debug -R SpineGpuCSharpParityTest
```

`SpineGpuImportTest --dump <dir>` 把夹具容器落成 `.zspg`（18 个，覆盖
baseline / linked mesh / 双皮肤 / deform / 裁剪的三种 end / 嵌套 / weighted 多边形 /
nonessential 开与关），C# 端读回来再写一遍比对。
没装 dotnet 时这条测试不注册（它是**加码**，不是构建的前置条件）。

## 11.2 容器由 C++ 唯一产出（`ZEditor -batchmode -suite=SpineCook`）

既然 C# 不做导入器，产出的入口就是一个：

```
ZEditor -batchmode -suite=SpineCook <输入> [-o <输出>] [-atlas <图集>] [--recursive|-r] [-h]
```

* **输入**：一个 `.json` / `.skel`，或一个目录（加 `--recursive` 连子目录一起走）。
* **解析器按扩展名选**，不"看内容像不像 json" —— `.skel` 既没魔数也没长度字段，
  猜错就是越界读。
* **图集**：省略则取同名 `.atlas`；找不到**报错**，不"尽力而为"（用错图集算出的
  UV 是静默错的，图集带裁剪时偏一点且不报任何错）。
* **目录 → 目录**保持相对结构。⚠️ 判据是"输入是目录"或"输出已存在且是目录"，
  **不能**只看 `is_directory(输出)`：输出目录第一次跑时还不存在，判成文件就会让
  每个骨架写同一条路径、互相覆盖，而覆盖在批处理里退出码还是 0。
* **有失败就退出码非 0**（"退出码 0 但少导了两个"是最难查的事故）；
  不认识的开关直接拒绝，不静默吞掉。

为什么走 `-batchmode` 而不是编辑器里的菜单：编辑器那套 `START_SYSTEM()` 会拉起
RHI、世界、资源系统，跑一个"读文件 → 算 → 写文件"的烘焙器全都不需要它们。
分流点在 `Main.cpp` 里 `RegisterRuntime()` **之前**（`Editor/ToolSuite/ToolSuite.h`
的注释写了原因）。

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
6. **换装必须验两个皮肤**：同一副骨架里两个皮肤在同一槽位放同名的附件（几何不同），
   两份都必须留在容器里，且能按 `(skin_id, slot_index)` 查回各自的 `attachment_id`。
   （v1 会把它们合并成一份 ⇒ 换装静默失效，这条就是为它设的回归。）
7. **容器必须可重入**：读回来的资产再序列化一次，字节必须与原来**逐字节相同** ——
   否则 C# 端读到的就不是同一个东西（指针 / 地址 / 未初始化填充都会在这里露出来）。
8. **几何共享必须被钉住**：含 linked mesh 的骨架，Bake 后**顶点数必须等于父附件的顶点数**
   （而不是翻倍），同时两个绘制项的 `uv_offset` 不同、UV 值不同。
   只断言"能画出来"不够 —— 去重一旦退化成复制，画出来照样是对的，内存却悄悄翻倍。
   配套断言：**`uv_count ≠ vertex_count`**，防止有人拿 `vertex_count` 去算 UV 数组长度。
9. **deform 必须逐项回退，不能整体回退**：带 FFD 的骨架里，**只有**被时间轴命中的绘制项
   是 `kFallback`，同一个骨架里的其它项必须仍是 `kStatic`；开了 `inheritDeform` 的
   linked mesh 跟着回退，关了的**不**跟着回退（§7.1）。
   只断言"deform 存在 ⇒ 有 fallback"不够 —— 那会把"整个资产回退"也放过。
10. **裁剪作用域的**两头**都要钉住**：4 槽位夹具（`s0` 在裁剪之前、`s1` 是裁剪附件自己、
   `s2` 在作用域内、`s3` 是边界槽位）里 ——
   * `end = s2` ⇒ `s2` 回退、**`s3` 不回退**（结束槽位是**含**在内的）
   * `end = s1`（自己）/ `end = s0`（更靠前）⇒ `s2`、`s3` **都**回退（收尾检查被 `continue` 跳过了）
   * 缺 `"end"` ⇒ 一路裁到最后一个槽位
   * 裁剪附件自己那个槽位**没有**绘制项
   且 **只有裁剪时不置 `kHasDeform`**（§7.2）。
   只断言"有裁剪 ⇒ 有 fallback"不够 —— 那既放过"整个资产回退"，也放过"结束槽位开/闭写反"。
11. **嵌套裁剪的判据必须是"最后那个槽位"**：5 槽位夹具（`s1`=clipA→`s4`，
   `s2`=clipB→`s3` 套在 clipA 里）里 ——
   * 正确（clipB 被忽略）⇒ `s3`、`s4` 都由 **clipA** 裁 ⇒ `s4` 是 `kFallback`
   * 错误（clipB 生效并终结 clipA）⇒ clipB 在 `s3` 就收尾 ⇒ **`s4` 变成 `kStatic`**
   ⚠️ 只断言 `s3` 不够 —— 两种实现都会让 `s3` 被裁，只有 `s4` 分得开。
12. **裁剪多边形必须落盘，且 `clip_index` / `vertex_offset` 的语义要钉住**：
   * 没有裁剪时 chunk 9 **根本不存在**（不是写个空块），且每个绘制项都是 `kNoClipIndex`
   * 被裁的项 `clip_index` 指到那条多边形；**没被裁的必须是 `kNoClipIndex` 而不是 0**
     —— 0 是一条真实存在的多边形，填 0 会让运行时裁错东西且不报错
   * 多边形顶点复用主顶点缓冲（chunk 3），逐点与 json/`.skel` 里的多边形相同
   * **`vertex_offset` 的单位是顶点下标不是字节** ⇒ 必须用到**第二条**多边形（偏移非 0）
     才能测出来；只有一条时它在段首，`0 × stride` 与 `0` 逐字节相同，写错也测不出来
   * 二进制的 weighted 判据是**显式 bool**，json 只能靠"数组长度 ≠ vertexCount×2"猜
     （没有 uvs 可比对）⇒ 两种输入必须对到同一个结果；json 缺 `vertexCount` 必须报错
     （参考实现会把它误判成 weighted ⇒ 静默解析成垃圾骨骼索引）
13. **必须跨语言对拍，不能两端各自自测**（§11.1）：C++ 落的 `.zspg`，C# 读回来再写一遍
    必须**逐字节相同**；同时要有语义断言，因为 round-trip 抓不到"解读"漂。

---

## 13. 决策状态

### 13.1 已拍板

| # | 问题 | 结论 | 落地处 |
|---|---|---|---|
| 1 | Spine 版本 | **3.8**，升 4.x 单独排期（理由：可归因，不是"3.8 更好"） | §1.1 |
| 3 | Clip 策略 | **从"策略"降级为"约束"**：3.8 只有 CPU 实现 ⇒ 带裁剪的槽位走 CPU 动态 VB，同帧可混跑 | §1.1、§7 |
| 9 | 裁剪的回退粒度 | **逐槽位**（不是整个资产）：作用域是 `(clipSlot, endSlot]`，逐皮肤模拟参考实现的状态机得出；**作用域不落盘**，只表达成作用域内绘制项的 `kind = kFallback`。**裁剪多边形要落盘**（chunk 9，12 B/条，顶点复用主顶点缓冲），绘制项加 `clip_index`（24 B → 28 B）。`kHasDeform` 只由 deform 点亮，不由裁剪点亮 | §7.2 |
| 6 | root 变换存在哪？ | **方案 A：实例项加 `root_index`（16 B → 20 B）**，root 存每帧 root 表，24 B/项。选它而不是 B/C 是因为它不改 `bone_base` 的语义，代价只是每绘制项 4 B | §5.2、§5.3 |
| 2 | `MAX_INFLUENCES = 4` 够吗？ | **定为 4**。导入器遇 `n > 4` **报错并报出附件名**，不静默截断 | §3.1 |
| 4 | 骨骼通道 structured 还是 UBO？ | **默认 structured / SSBO，24 B 紧凑**；仅当目标后端只有 UBO 时才用 32 B std140 变体（记录在 `bone_stride`） | §5.1 |
| 7 | 容器加 skin 维度怎么加？ | **附件表多一列 `skin_id`**（4 B → 8 B），而不是另建一张 skin 表。选它是因为换装查询就是"按 (skin, slot) 查附件 id"，一列就能表达；另建表会多一次间接跳转却不省什么 | §9.1 |
| 8 | linked mesh 的几何怎么省？ | **UV 拆成独立通道 + 顶点逐字节去重**，两条一起做。只拆 UV 不通（顶点仍按附件各存一份），只去重不拆 UV 也不通（UV 不同 ⇒ 字节不同 ⇒ 去重命中率为 0）。去重**只认逐字节完全相同**，不做近似 —— 宁可少共享，不可错共享 | §3.2、§14.2 |
| 11 | deform 怎么回退？ | **不做稠密展开，也不做 OSGame 式全量 CPU 动态 VB** —— deform 是蒙皮之前的局部空间加数（`VertexAttachment.cpp:122-123`，rigid 则是 `:72-74` 整体替换）⇒ 走 **deform 增量**：共享 bind pose VB 不动、蒙皮仍在 GPU，每实例每帧只上传当前帧 deform 数组（实测均值 122 B/条，对比全量动态 VB 的 24.4 KB/帧）。OSGame 引擎（`SpineMesh.h:266`）那种世界空间全量动态 VB 被否决：它每实例一份 VB，**正好废掉 §0 的 VB 共享** | `doc/GPU_SPINE_V7_ANIMATION.md` §5 |
| 10 | 动画怎么落？ | **A2（2026-10-22 拍板）**：骨骼树 + 动画烘焙进容器 → **v7**，解算器自己写，仍在 CPU（与 §0 一致）。否决 A1（运行时持有 spine 源数据 / 引入 spine-cpp），因为它与 §11.2「容器由 C++ 唯一产出」相冲，且 Unity 侧要跟着搬一整份运行时。范围与边界见 `doc/GPU_SPINE_V7_ANIMATION.md` | §11 |

### 13.2 仍待拍板

**无。** 全部九项已拍板；后续若再开新议题写在这里。

（v7 设计内部的两个开放项，记在 `doc/GPU_SPINE_V7_ANIMATION.md` §7：
约束（IK / transform / path）的 setup 数据未预留 chunk，届时需要 chunk 16+ 并 bump 到 v8；
chunk 15 变长载荷的编码未定。）

---

## 14. 已知限制

### 14.1 （已修，2026-10-21）~~容器里没有 skin 维度~~

v2 已补上：附件表多一列 `skin_id`（4 B → 8 B），绘制项尾部填充改成 `skin_id`
（v2 仍是 20 B，v3 起因 `uv_offset` 变 24 B），header 的 `reserved` 改成 `skin_count`，
字符串表插入皮肤名。
附件 = (皮肤, 槽位, 附件名) 三元组，**不再跨皮肤合并**。见 §9.1。

### 14.2 （已修，2026-10-21）~~linked mesh 的几何是复制的，不是共享的~~

v3 已解决，且**当初列的两条路都做了**（缺一不可，见 §13.1 第 8 项）：

1. **UV 拆成独立通道**：顶点只剩"几何 + 蒙皮"（rigid 16 → **8 B**，weighted 56 → **48 B**），
   UV 进 chunk 4 的 `uv_buffer`（`float2` × `uv_count`），由 `DrawItem::uv_offset` 索引。
2. **顶点逐字节去重**：`ExactBlobPool` 按"整段顶点字节完全相同"合并，FNV-1a 只用来缩小比较范围，
   最终判定永远是 `memcmp`。

现在 linked mesh 与父附件**共用同一段顶点**，各自只多一份 UV。测试里量到的实际收缩：
单皮肤 linked mesh 8 → **4** 个 weighted 顶点；跨皮肤 linked mesh 12 → **8**。

剩下的代价只有一条：shader 里多一次间接读取（先 `indexBuffer[...]` 拿顶点下标，
再 `uv_buffer[drawItem.uv_offset + vertexIndex]`）。这条**不能省** —— 它就是"能共用"的前提。

（历史：#1 版本、#2 影响数、#3 Clip、#4 骨骼通道、#5 root 是否 per-instance、#6 root 存放方式。）

### 14.3 （已修，2026-10-21）~~裁剪多边形本身不烘焙~~

v6 已补上：chunk 9 写 `ClipPolygon[]`（12 B/条），绘制项加 `clip_index`（24 B → 28 B）。
多边形顶点**复用主顶点缓冲**（chunk 3）—— `ClippingAttachment : VertexAttachment`，
编码方式与 mesh 顶点完全相同、走同一个 `ExactBlobPool` 去重，所以 header 不用新增字段。

（历史：v5 时这里写的是"多边形不烘焙，运行时从 spine 源数据取"。这不成立 ——
`kFallback` 的语义只说"别用静态 VB"，而"只靠容器渲染、不再持有 spine 源数据"是
整个容器格式的目标之一，裁剪多边形是**静态资产几何**（和 deform 那种逐帧数据不同），
必须能落盘。嵌套裁剪曾经也列在这里 —— 规则一直是明确的，但夹具里没有"两个裁剪附件叠
在一起"的骨架，因为第二条裁剪要**占掉**一个槽位、那个槽位就没有几何了。现已补上
5 槽位夹具，判据是**最后那个槽位**的 `kind`：见 §12 第 10 项。）

### 14.4 （已修，2026-10-21）~~容器在 Unity(C#) 侧的读端~~

已补上：`Function/Render/Spine/CSharp/`（`SpineGpuFormat.cs` + `SpineGpuContainerIo.cs`，
netstandard2.1，可整目录拷进 Unity Assets），并接进 ctest：
`SpineGpuImportTest --dump` 落盘 → C# 读回 → C# 再写 → 逐字节比对（§11.1）。

**C# 导入器不做**（拍板，2026-10-22）：从 spine 源数据在 Unity 侧产出同一个容器
这条路径**放弃**了。理由就是上面那条 —— "两端各写一遍导入器"是漂移的温床，而
Unity 侧真正要的只是**能读**。产出唯一地走 C++：

```
ZEditor -batchmode -suite=SpineCook <hero.json | hero.skel | 目录> [-o <输出>] [--recursive]
```

`SpineCook` 是 ZEditor 的一个**工具套件**（`Source/Editor/ToolSuite/SpineCook/`），
`-batchmode` 不弹窗、不启动引擎、不初始化 RHI，直接跑烘焙（见 §11.2）。目录输入
保持相对结构铺开，同名骨架不会互相覆盖；有失败就退出码非 0。

### 14.5 （设计已定，未实现，2026-10-22）容器里没有骨骼树与动画 —— 边界按 OSGame 实测反推

v6 及以前只烘焙**绑定姿态**：`SpineGpuAsset` 有 `bone_names` 但 `SpineSourceBone::parent`
在 Bake 时被丢掉（`SpineGpuImporter.h:78`），也**没有任何动画 chunk**（chunk 只到 9）。
⇒ 现在一个 `.zspg` **只能画绑定姿态**，动不起来；骨骼矩阵得由外部（spine 运行时）喂。

同时 `kHasDrawOrderTimeline` / `kHasClipping` 这两个 flag 的语义是"走 CPU 路径"，
而 **ZEngine 目前根本没有 CPU 路径** —— 它们指向一条不存在的路。

v7 的处理（见 `doc/GPU_SPINE_V7_ANIMATION.md`）。**边界不是拍脑袋定的**，是扫了
`G:/OSGame/Client_proj/Project/Assets` 下 **782 个 `.skel.bytes`**（全部 3.7.83，
782/782 解析到精确 EOF、0 失败）反推出来的：

* 必须烘焙 **11 类**时间轴：rotate / translate / scale / shear / attachment / color /
  twoColor / deform / drawOrder / pathConstraintPosition / pathConstraintMix；
  event 与 IK / transform 约束**时间轴**实测 0 条，不做。
* ⚠️ **但约束本身必须解算**：IK 22.5% / transform **48.3%** / path 11.1% 的资产带约束，
  而约束在 `Skeleton::updateWorldTransform` 里**每帧都跑**，与有没有时间轴无关。
  只做骨骼时间轴会让近一半资产姿势静默错。⇒ 约束 setup + 执行顺序也要进容器。
* **3.7 vs 3.8 字节布局完全一致**（逐段比对），只有槽位暗色判据不同
  ⇒ 导入器放宽版本 + 一个分支即可，**不需要第二套解析器**。
  现在实测 **0/782 能导入**（版本检查硬拒 3.8 以外的）。
* **deform 是 93% 资产都有**的大头（100 306 条 / 1 733 134 帧 / 2 831 万 float），
  且帧是**稀疏**的 ⇒ 不能展开成稠密数组搬 GPU（GB 级）。v7 保持稀疏烘焙 +
  既有 `kFallback` 语义走 CPU 动态 VB，**只回退被命中的绘制项**。
* 曲线段实测 257 万段（linear 58% / stepped 5% / bezier 37%）⇒ 编码从
  定长 20 B/段改成**紧凑**（u8 类型 + 每条 bezier 16 B），49 MB → 16.8 MB。

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
| `ClippingAttachment : public VertexAttachment`（多边形与 mesh 顶点**同源** ⇒ 可复用主顶点缓冲 + 同一去重池，header 不用加字段） | `ClippingAttachment.h:38` |
| 二进制 `readVertices`：weighted 是**显式 bool**，且 `boneCount`/`boneIdx` 是 **varint**（只有 x/y/w 是 float） | `SkeletonBinary.cpp:658-691` |
| json `readVertices` 的 weighted 判据 = 数组长度 ≠ `vertexCount*2`（裁剪附件没有 uvs 可比对，只能靠这个数） | `SkeletonJson.cpp:1218` |
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
| ⚠️ 找父附件用的 skin = `"skin"` 字段（**空 ⇒ default**），**不是** linked mesh 所在的那个 skin | `SkeletonBinary.cpp:302-303`、`SkeletonJson.cpp:653-654` |
| `applyDeform(source)` = `this == source \|\| (_inheritDeform && _parentMesh == source)` | `MeshAttachment.cpp:94-95`（基类版只判 `this == source`：`VertexAttachment.cpp:134-136`） |

### deform / FFD 的出处（容器 v4）

| 断言 | 出处 |
|---|---|
| deform 长度 = `weighted ? verts.size()/3*2 : verts.size()` | `SkeletonJson.cpp:1071`、`SkeletonBinary.cpp:976` |
| rigid 的 deform 存**绝对**局部坐标，运行时整体**替换** `_vertices` | `VertexAttachment.cpp:72-73`；构造处 `:1085`、`:1100-1104` |
| weighted 的 deform 存**偏移**，运行时逐影响**相加** | `VertexAttachment.cpp:122-123` |
| `f = skip << 1` 里的 `skip` 累加的是**影响数** ⇒ weighted 是每影响 2 float | `VertexAttachment.cpp:90-94`、`:115-127` |
| ⚠️ rigid：3.8 的 `RegionAttachment` 继承自 `Attachment`，**不是** `VertexAttachment` ⇒ deform 指向 region 时参考实现是未定义行为 | `RegionAttachment.h:46`、`SkeletonJson.cpp:1067`、`SkeletonBinary.cpp:972` |
| 运行时对非 VertexAttachment 的附件**静默不生效**（有 RTTI 检查） | `DeformTimeline.cpp:72-74` |
| deform 段结构 = { skin: { slot: { attachment: [帧] } } }；帧可缺省 `vertices`（= 回到 setup） | `SkeletonJson.cpp:1049-1115`、`:1081-1086` |
| 二进制帧：`end==0` 表示无顶点；否则读 `start` 后 `end += start`，再读 `end-start` 个 float ⇒ **落盘第二个 varint 是"个数"不是"终点"** | `SkeletonBinary.cpp:989-1018` |
| ⚠️ 帧的 `offset + 顶点数` 无边界检查 ⇒ 参考实现会**越界写** | `SkeletonJson.cpp:1092-1099` |
| 皮肤找不到时参考实现是**空指针**（json）/ **越界读**（二进制） | `SkeletonJson.cpp:1051`、`SkeletonBinary.cpp:959` |
| 附件找不到时参考实现**倒是**报错："Attachment not found" | `SkeletonJson.cpp:1061-1065`、`SkeletonBinary.cpp:966-970` |
| json 里 inheritDeform 的字段名是 **`"deform"`**（不是 "inheritDeform"），默认 1 | `SkeletonJson.cpp:572` |
| 二进制里 inheritDeform 是紧跟 parent 的一个 bool | `SkeletonBinary.cpp:594` |

### 裁剪作用域的出处（容器 v5）

| 断言 | 出处 |
|---|---|
| `ClippingAttachment : VertexAttachment`，只有一个私有的 `SlotData* _endSlot`，构造时置 NULL | `ClippingAttachment.h:38-53`、`ClippingAttachment.cpp:42` |
| json：名字取自 `"end"`，`findSlot()` 的返回值**不检查** ⇒ 槽位名写错 / 缺失时 `_endSlot` 静默变 NULL | `SkeletonJson.cpp:631-635` |
| 二进制：**先 `endSlotIndex`（varint）再 `vertexCount`**；越界报 "parse clip attachment failed!" | `SkeletonBinary.cpp:637-646` |
| `clipEnd(Slot&)` 只在 `_clipAttachment->_endSlot == &slot._data` 时收尾 | `SkeletonClipping.cpp:72-76` |
| 已经有一个裁剪在跑时 `clipStart()` 直接返回 0 ⇒ **嵌套裁剪被忽略** | `SkeletonClipping.cpp:49-51` |
| 走到裁剪附件所在槽位 ⇒ `clipStart()` 后**立刻 continue** ⇒ 该槽位不画，且**跳过**了收尾的 `clipEnd(*slot)` | `MeshGeneration/SpineMesh.h:153-159`（对比 `:257`） |
| 槽位在裁剪中 ⇒ `clipTriangles()` 重写顶点 / 三角形 / UV | 同文件 `:189-197` |
| 整条绘制指令末尾还有一次**无条件**的 `clipEnd()` | 同文件 `:259` |

### skin 的出处（容器 v2）

| 断言 | 出处 |
|---|---|
| 3.8 的 json：skins 是**对象**（皮肤名 → 槽位 → 附件），皮肤名取自**成员名** | `SkeletonJson.cpp:439` `new Skin(skinMap->_name)` |
| ⚠️ 数组形式读不了：数组成员是 `new Json(NULL)` 且**从不**赋 `_name` | `Json.cpp:402-450`（`parseArray`）对比 `:479/507`（`parseObject` 才赋 `_name`） |
| 二进制：default 皮肤是**先读的**那一个，名字写死 `"default"` | `SkeletonBinary.cpp:276` |
| ⚠️ 空皮肤**占位不对称**：default 空 ⇒ 不占下标；具名皮肤空 ⇒ 仍占下标 | `SkeletonBinary.cpp:277-283`（`skinsCount` 只在 defaultSkin 非空时 +1）、`:293-295` |
| 二进制具名皮肤数 = 单独一个 varint，不含 default | `SkeletonBinary.cpp:277` |
| 同一皮肤内 `(slotIndex, name)` 重复 ⇒ **后写的覆盖**先写的 | `Skin.h:118-120`（`addAttachment` 注释） |
| 皮肤内查附件只在**该皮肤**内 | `Skin.h:126`（`getAttachment(slotIndex, name)`） |

**`.skel` 与 json 必须产出同一个容器。** 这一条有一个硬测试钉住
（`SpineGpuImportTest` 第 9 节）：手写的 `.skel` 字节流夹具与 json 夹具内容等价，
两条解析路径各自 Bake 后**顶点缓冲逐字节相同**。二进制那边的字段顺序、大端、varint、
weighted 扁平编码只要错一处，两边就会分叉 —— 这是唯一能自动发现这类错误的手段。

**越界读的守卫**：`.skel` 既没有魔数也没有长度字段，截断文件是最容易触发越界读的场景。
测试对夹具的**每一个真前缀**都调一次解析，要求全部失败（不能"假装成功"，更不能越界读）。
