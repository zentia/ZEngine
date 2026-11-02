# Mesh / MeshFilter 脚本 API

`Mesh`（`Runtime/Scripting/ScriptMesh/ScriptMesh.h`）与 `MeshFilter`
（`Runtime/Function/Framework/Component/Mesh/MeshFilter.h`）—— Unity 的
`UnityEngine.Mesh` / `UnityEngine.MeshFilter` 的对位。

## 1. 为什么 `Mesh` 自己持有数据，而不是网格资产的句柄

前面几个切片的门面（`MaterialInstance`、摄像机、输入）都是**句柄**：状态在别处，
对象只转发。这里反过来 —— `Mesh` 自己存顶点，`MeshFilter` 才是"指向别处"的那个。
原因是两边的数据形状不一样：

| | 渲染要的网格 | Unity 的 `Mesh` |
|---|---|---|
| 形态 | `MeshData` **资产**（导入器写进 .zasset） | CPU 侧对象 |
| 上传 | `RenderResourceBase` 按**资产 id** 上传并缓存 GPU buffer | 无 |
| 粒度 | 整个资产 | 逐顶点可写 |

运行时拿不到"改一个顶点"这种粒度的入口 —— 数据根本不在 CPU 侧的可写内存里。
所以 `Mesh` 就是 CPU 侧对象：数据在自己身上，全部方法不碰资产系统。

**副产品**：整套 API 在 headless 下可测（没有资产系统、没有 GPU 也能跑），而不是
"测试里永远拿到空数组"。这是选这个形态的主要理由之一 —— 资产门面的测试在本工程
里一贯很弱（`loadAsset` 在测试进程里读不到东西），而这里不必付那个代价。

## 2. 渲染链路的边界（最容易误解的一点）

`Mesh` **不参与绘制**。渲染资源按资产 id 上传，一个 CPU 侧顶点数组没有资产 id。

`MeshFilter` 因此有两条路，语义完全不同：

| 成员 | 参与绘制 | 落到哪 |
|---|---|---|
| `sharedMeshPath`（`MeshData` 资产路径） | **是** | `BaseRenderer` 的 sub-mesh 列表 |
| `mesh`（运行时 `Mesh`） | 否 | `BaseRenderer::SetLocalBounds`（剔除与 `renderer.bounds`） |

想画一个脚本生成的网格，今天的路径是把它写成资产（编辑器侧），再让
`sharedMeshPath` 指向它。这一条写在 `ScriptMesh.h` 和 `MeshFilter.h` 的文件头，
不写清楚的话"设了 mesh 但画面没变"会被当成 bug 查很久。

## 3. 发布面

### `Mesh`

| Unity | 本引擎 | 备注 |
|---|---|---|
| `vertexCount` | `GetVertexCount()` | |
| `triangles.length / 3` | `GetTriangleCount()` | 索引数整除 3 |
| `subMeshCount` | `GetSubMeshCount()` | 恒为 1（空网格为 0） |
| `vertices` | `GetVertices()` / `SetVertices()` | 属性 `vertices` |
| `normals` | `GetNormals()` / `SetNormals()` | 属性 `normals` |
| `tangents` | `GetTangents()` / `SetTangents()` | `Vector4`，`w` = 手性 ±1 |
| `uv` / `GetUVs(0)` | `GetUV0()` / `SetUV0()` | 属性 `uv` |
| `GetUVs(channel)` / `SetUVs(channel, …)` | 同名 | 只有 channel 0 |
| `triangles` | `GetTriangles()` / `SetTriangles()` | 属性 `triangles` |
| `bounds` | `GetBounds()` | 只读属性 |
| `RecalculateBounds()` / `RecalculateNormals()` / `RecalculateTangents()` | 同名 | |
| `Clear()` | 同名 | |

数组返回类型走 ZHeaderTool 的 `std::vector<T> -> T[]` 分支（`parser.py` 的
`ts_type_for`），绑定侧是 `Converter<std::vector<T>>`。挂不上的话这些方法是**从
d.ts 里消失**，不是变成 `any`，所以 TS 侧毫无察觉 —— `ScriptComponentSmokeTest`
的 ZA5 就是这道门。

### `MeshFilter`

| Unity | 本引擎 | 备注 |
|---|---|---|
| `sharedMesh` | `sharedMeshPath`（字符串） | 本引擎的网格资产就是一条路径 |
| `mesh` | `mesh`（`Mesh`） | 不自动实例化；没 Set 过是 null |
| — | `HasRenderer()` | 诊断用，非 Unity 成员 |
| — | `GetLocalBounds()` | 本 filter 贡献的盒 |

## 4. 三条"宁可窄而诚实"的规则

1. **`GetUVs(1)` 返回空数组而不是 0 号那份。** 让"我要 uv2"悄悄拿到 uv，会让光照
   贴图那类计算用错通道还看不出来。
2. **`SetTriangles` 把不是 3 的倍数的尾巴丢掉**，而不是原样存下。存下来的话
   `GetTriangleCount()` 说的是 `length/3`，画的却是一个残缺三角形 —— 而残缺三角形
   在渲染侧是读越界。
3. **空网格的 `bounds` 是零盒而不是单位立方体。** 零盒一眼看得出"没算出来"；单位
   立方体是一个完全合理的包围盒，会让剔除悄悄多画或少画。

## 5. 刻意没做的 Unity 成员

- `MarkDynamic()` / `UploadMeshData()` / `Optimize()` 系列：全是"给 GPU 上传策略的
  提示"，而 GPU buffer 由资产导入期决定，没有可提示的对象。空实现会让脚本以为自己
  优化过了。
- `GetIndices()` / `SetIndices()` / `GetTopology()` / `SetSubMeshes()` /
  `GetSubMesh(i)`：`MeshData` 只有一个三角形索引缓冲，没有 sub-mesh 与拓扑的概念。
- `bindposes` / `boneWeights`：蒙皮数据走 `SkeletonBinding` 资产，与这里的顶点数组
  不是同一条路。
- `blendShape*`：没有后端。
- `isReadable`：Unity 那个是"导入时勾了 CPU 副本吗"。这里的网格本来就只在 CPU 侧，
  发一个恒为 true 的属性没有信息量。
- `MeshFilter.mesh` 的**自动实例化**语义：Unity 第一次读 `mesh` 会把 sharedMesh 拷
  一份出来。这里只在脚本显式 Set 过之后才存在 —— 自动拷贝一份"看起来和资产一样、
  其实已经分家"的网格，是"改了半天发现改的不是资产"的来源。

## 6. 接线

`MeshFilter` 是网格的**唯一来源**：它的内容变化就写回同对象上的 `BaseRenderer`
（sub-mesh 列表 + `SetLocalBounds`）。数据只有一份，不存在两个组件不一致的状态。

两个必须知道的细节：

- **`AddComponent` 先跑 `PostLoadResource` 再挂进列表**（`GameObject.cpp` 的注释写了
  为什么），所以"先加 filter 后加 renderer"时 filter 那一次推送看不到 renderer。
  这条路径靠 `m_PushPending` + `Tick` 里补一次兜住。
- **给对象挂上 `MeshFilter` 之后，渲染器上手工填的 sub-mesh 会被它覆盖**（空的
  filter 会清空）。Unity 也是这个分工，但那边 renderer 本来就没有自己的网格字段。

## 7. 变异测试

`.mutate_mesh.py`，24 个变异体，24/24 caught。四条值得记：

- **MF05（空网格的 bounds 退回单位立方体）一开始是存活的**，而它存活的原因是
  **白测**：新对象的 `m_BoundsDirty` 是 false、缓存本来就是零盒，`Clear()` 又直接给
  `m_Bounds` 赋值 —— 两条"空网格返回零盒"的断言（ME3 / ME25）**根本没有走进
  `ComputeBounds`**。补了一条"从有顶点变成没顶点"的断言（ME47）才抓到。判据是
  "被测分支有没有被走到"，不是"断言看起来覆盖了那个语义"。

- **MF11（`w` 恒为 +1）需要一组翻转 `v` 的 UV 才抓得到。** 默认布局下手性本来就是
  +1，只断言 `|w| == 1` 等于没测。对应测试 `ME46`。
- **MF15（每个三角形只给第一个顶点累加）需要四个角都判。** 只判 `[0]` 时它恰好把
  `[0]` 算对了。
- **MF08 变异的是 `SafeNormalise` 的零长度守卫，不是"跳过退化面"那个判据。** 后者
  去掉之后结果一模一样（零长度面法线加进来等于没加）—— 变异一个没有可观测差别的
  位置只会得到一个等价变异体，而等价变异体说明不了任何事。
