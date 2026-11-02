#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Bounds.h"
#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Math/Vector4.h"

#include <vector>

#include "ScriptMesh.generated.h"

// =============================================================================
// `Mesh` —— Unity 的 `UnityEngine.Mesh`（网格数据对象）
// =============================================================================
//
// 为什么是一个**自己持有数据**的对象，而不是网格资产的句柄
// -------------------------------------------------------
// 前面几个切片做的门面（`MaterialInstance`、摄像机、输入）都是"句柄"：状态在别
// 处，对象只是转发。这里反过来 —— `Mesh` 自己存顶点，`MeshFilter` 才是那个"指
// 向别处"的东西。原因是两边的数据形状不一样：
//
//   * 渲染要的网格是**资产**：`MeshData`（`Resource/ResType/Data/MeshData.h`）
//     由导入器写进 .zasset，`RenderResourceBase` 按**资产 id** 上传并缓存一份
//     GPU buffer。运行时拿不到"改一个顶点"这种粒度的入口 —— 数据根本不在 CPU
//     侧的可写内存里。
//   * Unity 的 `Mesh` 要的正好相反：`new Mesh()` 出来的对象脚本可以逐顶点填，
//     `RecalculateNormals()` 立刻能算出结果。这是一个 **CPU 侧对象**。
//
// 于是 `Mesh` 就是 CPU 侧对象：数据在自己身上，全部方法不碰资产系统。这带来一
// 个立竿见影的好处 —— **整套 API 在 headless 下可测**（没有资产系统、没有 GPU
// 也能跑），而不是"测试里永远拿到空数组"。
//
// ⚠️ 与引擎渲染链路的关系（写在最前面，因为这是最容易误解的一点）
// ------------------------------------------------------------------
// `Mesh` **不参与绘制**。渲染资源按资产 id 上传（见 `RenderResourceBase`），
// 一个 CPU 侧的顶点数组没有对应的资产 id，也就进不了那条路。今天它影响的是
// **包围盒**：`MeshFilter.mesh = m` 会把这份网格的 `bounds` 推进同对象上的
// `BaseRenderer`（`SetLocalBounds`），于是剔除与 `renderer.bounds` 是真的跟着
// 变的。
//
// 想画一个脚本生成的网格，今天的路径是把它写成资产（编辑器侧），再让
// `MeshFilter.sharedMeshPath` 指向它。
//
// 刻意没做的 Unity 成员
// ---------------------
//   * `MarkDynamic()` / `UploadMeshData()` / `Optimize()` /
//     `OptimizeIndexBuffers()` / `OptimizeReorderVertexBuffer()`：全是"给 GPU
//     上传策略的提示"，而这边的 GPU buffer 由资产导入期决定，没有可提示的对象。
//     做成空实现会让脚本以为自己优化过了。
//   * `GetIndices()` / `SetIndices()` / `GetTopology()` / `SetSubMeshes()` /
//     `GetSubMesh(i)`：`MeshData` 只有**一个**三角形索引缓冲，没有 sub-mesh 与
//     拓扑的概念（Unity 的 subMeshCount 在这里恒为 1）。发一组"能调但永远是
//     triangles / 恒有一个 sub-mesh"的方法是假成功。
//   * `bindposes` / `boneWeights`：蒙皮数据走 `SkeletonBinding` 资产
//     （`MeshData::bind`），与这里的顶点数组不是同一条路。
//   * `blendShape*`：没有后端。
//   * `isReadable`：Unity 那个是"CPU 侧还留着副本吗"（导入时勾的）。这里的网格
//     **本来就只在 CPU 侧**，所以发一个恒为 true 的属性只是把 Unity 的一个
//     导入期开关搬过来 —— 没有信息量，反而让人以为有"不可读"的状态。
// =============================================================================

ZCLASS()
class Mesh : public Object
{
    ZGENERATED_BODY(Mesh)

public:
    Mesh() = default;

    // ---- 规模 ------------------------------------------------------------

    /// Unity 的 `mesh.vertexCount`。
    ZFUNCTION()
    int GetVertexCount() const { return static_cast<int>(m_Vertices.size()); }

    /// Unity 的 `mesh.triangles.length / 3`。
    ///
    /// 按"索引数整除 3"算而不是"存了多少个三角形"：`SetTriangles` 传进来的长度
    /// 不是 3 的倍数时，尾巴上那几个索引会被**丢弃**（见那里的注释），这里报的
    /// 必须是"真的能被画出来的三角形数"。
    ZFUNCTION()
    int GetTriangleCount() const { return static_cast<int>(m_Triangles.size() / 3); }

    /// Unity 的 `mesh.subMeshCount` —— 这里恒为 1 或 0（空网格）。
    ///
    /// 发它而不是"不发"是为了让照着 Unity 写的循环（`for i in 0..subMeshCount`）
    /// 跑得通；0 而不是 1 是给空网格的 —— "没有顶点却有一个 sub-mesh" 会让
    /// `GetTriangles(0)` 看起来该有东西。
    ZFUNCTION()
    int GetSubMeshCount() const { return m_Triangles.empty() ? 0 : 1; }

    // ---- 顶点属性 --------------------------------------------------------

    ZFUNCTION()
    std::vector<Vector3> GetVertices() const { return m_Vertices; }

    /// Unity 的 `mesh.vertices = v`。
    ///
    /// 换了顶点之后旧法线 / 切线 / 包围盒都可能失效，所以这里**不**顺手重算
    /// （Unity 也不重算：`mesh.vertices = v` 之后法线还是旧的，要显式
    /// `RecalculateNormals()`）。但包围盒标记成脏 —— `bounds` 是一个"随时会被
    /// 读"的量，让它留着上一批顶点的结果比让它错更危险；法线则是"要不要重算"
    /// 由调用方决定。
    ZFUNCTION()
    void SetVertices(const std::vector<Vector3>& vertices)
    {
        m_Vertices = vertices;
        m_BoundsDirty = true;
    }
    ZSCRIPTPROPERTY(Name = "vertices", Getter = "GetVertices", Setter = "SetVertices")

    ZFUNCTION()
    std::vector<Vector3> GetNormals() const { return m_Normals; }
    ZFUNCTION()
    void SetNormals(const std::vector<Vector3>& normals) { m_Normals = normals; }
    ZSCRIPTPROPERTY(Name = "normals", Getter = "GetNormals", Setter = "SetNormals")

    /// Unity 的 `mesh.tangents`（Vector4，w = ±1 表示副切线朝向）。
    ZFUNCTION()
    std::vector<Vector4> GetTangents() const { return m_Tangents; }
    ZFUNCTION()
    void SetTangents(const std::vector<Vector4>& tangents) { m_Tangents = tangents; }
    ZSCRIPTPROPERTY(Name = "tangents", Getter = "GetTangents", Setter = "SetTangents")

    /// Unity 的 `mesh.GetUVs(channel)`。
    ///
    /// ⚠️ 只有 channel 0：引擎的顶点格式只有一套 UV（`RawVertexBuffer::uvs`，
    /// `MeshData::Vertex` 的 u/v）。其它 channel 返回**空数组**而不是"返回 0 号
    /// 那份" —— 后者会让"我要 uv2"这种调用悄悄拿到 uv，于是光照贴图那类计算
    /// 用错了通道还看不出来。
    ZFUNCTION()
    std::vector<Vector2> GetUVs(int channel) const { return channel == 0 ? m_UVs : std::vector<Vector2>(); }

    /// Unity 的 `mesh.SetUVs(channel, uvs)`。返回 bool：**这个 channel 存得下吗**。
    ZFUNCTION()
    bool SetUVs(int channel, const std::vector<Vector2>& uvs)
    {
        if (channel != 0)
        {
            return false;
        }
        m_UVs = uvs;
        return true;
    }

    /// 0 号 UV 的简写（Unity 的 `mesh.uv`）。给最常见的场合少敲一个参数。
    ZFUNCTION()
    std::vector<Vector2> GetUV0() const { return m_UVs; }
    ZFUNCTION()
    void SetUV0(const std::vector<Vector2>& uvs) { m_UVs = uvs; }
    ZSCRIPTPROPERTY(Name = "uv", Getter = "GetUV0", Setter = "SetUV0")

    /// Unity 的 `mesh.triangles`（顶点下标，长度恒为 3 的倍数）。
    ZFUNCTION()
    std::vector<int32_t> GetTriangles() const { return m_Triangles; }

    /// Unity 的 `mesh.triangles = t`。
    ///
    /// ⚠️ 长度不是 3 的倍数时**丢掉尾巴上多出来的 1~2 个索引**而不是原样存下：
    /// 存下来的话 `GetTriangleCount()` 说的是 `length/3`，画的却是一个残缺的三
    /// 角形 —— 而"残缺的三角形"在渲染侧是读越界。宁可在写入时就把数据变合法。
    ///
    /// 越界下标**不**在这里查：那要按顶点数判，而 `SetTriangles` 常常在
    /// `SetVertices` 之前调（先给拓扑再填顶点）。越界的后果留给取用方按自己的
    /// 方式处理（这里所有读路径都只按自己拿到的下标访问，不额外放大）。
    ZFUNCTION()
    void SetTriangles(const std::vector<int32_t>& triangles)
    {
        m_Triangles = triangles;
        if (m_Triangles.size() % 3 != 0)
        {
            m_Triangles.resize((m_Triangles.size() / 3) * 3);
        }
    }
    ZSCRIPTPROPERTY(Name = "triangles", Getter = "GetTriangles", Setter = "SetTriangles")

    /// Unity 的 `mesh.Clear()`：清空全部属性（不是"销毁对象"）。
    ZFUNCTION()
    void Clear()
    {
        m_Vertices.clear();
        m_Normals.clear();
        m_Tangents.clear();
        m_UVs.clear();
        m_Triangles.clear();
        m_Bounds = Bounds();
        m_BoundsDirty = false;
    }

    // ---- 包围盒 ----------------------------------------------------------

    /// Unity 的 `mesh.bounds`。
    ///
    /// 脏了就现算再缓存：`bounds` 是每帧都可能被读的量，而算一遍是 O(顶点数)。
    /// 与 `SetVertices` 里那条注释对应 —— 换顶点之后读 `bounds` 拿到的是**新的**。
    ZFUNCTION()
    Bounds GetBounds() const
    {
        if (m_BoundsDirty)
        {
            m_Bounds = ComputeBounds();
            m_BoundsDirty = false;
        }
        return m_Bounds;
    }
    ZSCRIPTPROPERTY(Name = "bounds", Getter = "GetBounds")

    /// Unity 的 `mesh.RecalculateBounds()`：显式重算一次（与读 `bounds` 是同一
    /// 份缓存）。
    ZFUNCTION()
    void RecalculateBounds()
    {
        m_Bounds = ComputeBounds();
        m_BoundsDirty = false;
    }

    // ---- 重算 ------------------------------------------------------------

    /// Unity 的 `mesh.RecalculateNormals()`。
    ///
    /// 做法是"按面法线累加到三个顶点再归一化"：平滑法线（共享顶点处取平均）是
    /// Unity 的行为，也是"看起来像曲面"的那一半。
    ///
    /// 面法线用**未归一化**的叉积 —— 这样面积大的面自然占更大权重，这是
    /// Unity/大多数 DCC 的做法；先归一化再累加会把小面放大成和大面一样重要。
    ///
    /// 长度退化到 0（三点共线 / 重合）的面跳过：它的法线没有定义，归一化会得到
    /// NaN 并**污染整个顶点**（NaN 一进累加就出不来）。
    ZFUNCTION()
    void RecalculateNormals();

    /// Unity 的 `mesh.RecalculateTangents()`。
    ///
    /// 需要法线和 UV：切线是"UV 的 u 方向在模型空间指哪"，没有这两样就没有定义。
    /// 缺任一样就**直接返回**（法线数组留原样）—— 拿一个"随便猜的切线"出来比
    /// 没有更糟，法线贴图的错误方向是那种看得出错但查不出原因的问题。
    ///
    /// 算完之后对法线做一次 Gram-Schmidt 正交化，`w` 记副切线朝向（±1）—— 这是
    /// Unity 的 `Vector4` 切线的约定，shader 侧靠 `w` 决定副切线翻不翻。
    ZFUNCTION()
    void RecalculateTangents();

private:
    /// 由当前顶点算包围盒；空网格退回零盒（与 Unity 的 `default(Bounds)` 同形）。
    Bounds ComputeBounds() const;

    // 刻意不标 ZPROPERTY：这是**运行时**网格对象，顶点数组没有"存进 .zasset"
    // 这一说（引擎的网格资产是 `MeshData`，由导入器写）。标成 ZPROPERTY 会让
    // 编辑器以为可以序列化它，而序列化出来的东西没有任何一条加载路径会读。
    std::vector<Vector3> m_Vertices;
    std::vector<Vector3> m_Normals;
    std::vector<Vector4> m_Tangents;
    std::vector<Vector2> m_UVs;
    std::vector<int32_t> m_Triangles;

    /// `GetBounds` 的缓存。`mutable` 是因为 `GetBounds` 是 const 的。
    mutable Bounds m_Bounds;
    mutable bool m_BoundsDirty {false};
};
