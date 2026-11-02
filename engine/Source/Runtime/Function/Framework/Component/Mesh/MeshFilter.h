#pragma once

#include "Runtime/BaseClasses/PPtr.h"
#include "Runtime/Core/Math/Bounds.h"
#include "Runtime/Function/Framework/Component/Component.h"

#include <EASTL/string.h>

#include "MeshFilter.generated.h"

class BaseRenderer;
class Mesh;

// =============================================================================
// `MeshFilter` —— Unity 的 `UnityEngine.MeshFilter`
// =============================================================================
//
// Unity 里 `MeshFilter`（网格从哪来）与 `MeshRenderer`（怎么画）是两个组件，挂
// 在同一个 GameObject 上。ZEngine 之前没有前者：网格直接长在渲染器上
// （`BaseRenderer::m_SubMeshes`）。
//
// 这里补上 `MeshFilter`，但**不**把网格从渲染器上搬走 —— 搬走意味着改渲染状态
// 提交、prefab 克隆、序列化迁移一整条链。做法是让 `MeshFilter` 做**唯一来源**：
// 它的内容变化就写回同对象上的 `BaseRenderer`（见 `PushToRenderer`）。
//
// 于是两者的分工与 Unity 一致（filter 提供网格、renderer 负责画），而数据只有
// 一份，不存在"两边不一致"的状态。
//
// ⚠️ 一个必须知道的副作用：给对象挂上 `MeshFilter` 之后，渲染器上**手工填的
// sub-mesh 会被它覆盖**（空的 filter 会把 sub-mesh 清空）。Unity 也是这个分工，
// 但那边 renderer 本来就没有自己的网格字段，所以不会有人踩。这里踩得到，所以
// 写在文件头而不是藏在 .cpp 里。
//
// 两份网格：资产 vs 运行时
// ------------------------
//   * `sharedMeshPath`（`MeshData` 资产路径）—— **参与绘制**。它写进
//     `BaseRenderer` 的 sub-mesh 列表，之后就是引擎原来的那条资产链路
//     （`RenderResourceBase` 按资产 id 上传）。
//   * `mesh`（运行时 `Mesh` 对象）—— **不参与绘制**，只影响包围盒。
//     原因见 `ScriptMesh.h` 的文件头：渲染资源按资产 id 上传，一个 CPU 侧的顶点
//     数组没有资产 id。它推进 `BaseRenderer::SetLocalBounds`，所以剔除与
//     `renderer.bounds` 是真的跟着变的。
//
// 两条路都发给脚本，因为它们的用途本来就不同（换模型 vs 让剔除框跟着程序化网
// 格走）；但文档里必须说清哪条会画出来。
//
// 刻意没做的 Unity 成员
// ---------------------
//   * `mesh` 的"自动实例化"语义：Unity 读 `meshFilter.mesh` 会在第一次读时把
//     sharedMesh 拷一份出来，之后写它不影响资产。这里 `mesh` 只在脚本显式
//     Set 过之后才存在，没 Set 过就是 nullptr —— 自动拷贝一份"看起来和资产
//     一样、其实已经分家"的网格，是那种改了半天发现改的不是资产的 bug 来源。
// =============================================================================

ZCLASS()
class MeshFilter : public Component
{
    ZGENERATED_BODY(MeshFilter)

public:
    MeshFilter() = default;

    void PostLoadResource(GameObject* parent_object) override;
    void OnSerializedFieldsUpdated() override;
    void Tick(float delta_time) override;

    // ---- 资产网格（参与绘制）--------------------------------------------

    /// Unity 的 `meshFilter.sharedMesh` —— 这里给的是**资产路径**，因为本引擎
    /// 的网格资产就是一条路径（`SubMeshRes::m_MeshAsset`），没有"网格资产对象"
    /// 这么个脚本可见的东西。
    ZFUNCTION()
    eastl::string GetSharedMeshPath() const { return m_MeshAsset; }

    /// 换网格资产。空串表示"不画东西"。
    ///
    /// 立刻写回渲染器（不用等下一帧）：`Tick` 里做的话，同一帧内
    /// `renderer.bounds` 读到的还是旧网格，而"换完立刻问包围盒"是 gameplay 代码
    /// 很常见的写法。
    ZFUNCTION()
    void SetSharedMeshPath(const eastl::string& mesh_asset);
    ZSCRIPTPROPERTY(Name = "sharedMeshPath", Getter = "GetSharedMeshPath", Setter = "SetSharedMeshPath")

    // ---- 运行时网格（只影响包围盒）---------------------------------------

    /// 脚本 Set 过的那份运行时网格；没 Set 过返回 nullptr。
    ZFUNCTION()
    Mesh* GetMesh();

    /// 把一份运行时网格挂上来，并把它的包围盒推进渲染器。传 nullptr 撤掉。
    ZFUNCTION()
    void SetMesh(Mesh* mesh);
    ZSCRIPTPROPERTY(Name = "mesh", Getter = "GetMesh", Setter = "SetMesh")

    // ---- 诊断 ------------------------------------------------------------

    /// 同对象上有没有渲染器。
    ///
    /// 不是 Unity 的成员。留着是因为"换了网格画面没变"最常见的原因是**这个对象
    /// 上根本没挂渲染器** —— 而 `SetSharedMeshPath` 在这种情况下是静默成功
    /// （没有渲染器就没有要写回的地方，也不该报错）。给脚本一条自查的路。
    ZFUNCTION()
    bool HasRenderer() const;

    /// 本 filter 贡献的包围盒：有运行时网格就是它的盒，否则是零盒。
    ///
    /// 要"当前真的在剔除用的那个盒"该问渲染器的 `localBounds` —— 那个值还包含
    /// 资产网格算出来的那一份。
    ZFUNCTION()
    Bounds GetLocalBounds() const;

private:
    /// 同对象上的渲染器（没有 / 还没挂上返回 nullptr）。刻意不导出：脚本要的
    /// 是 `HasRenderer()`，给它一个裸 `BaseRenderer*` 又要解释一堆所有权。
    BaseRenderer* FindRenderer() const;

    /// 把当前状态写回渲染器。
    void PushToRenderer();

    ZPROPERTY(Name = "mesh_asset")
    eastl::string m_MeshAsset;

    /// 脚本 Set 进来的运行时网格。用 PPtr：`Mesh` 被 Destroy 之后解析成 nullptr，
    /// 于是 `GetLocalBounds()` 自然退回零盒，不会读到已释放的对象。
    PPtr<Mesh> m_RuntimeMesh;

    /// 上一次推送没写成（当时同对象上还没有渲染器），下一个 Tick 补一次。
    /// 不序列化：它是"这一帧还没做完的事"，不是存档该记的状态。
    bool m_PushPending {false};
};
