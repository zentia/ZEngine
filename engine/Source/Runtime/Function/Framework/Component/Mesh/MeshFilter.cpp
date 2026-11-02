#include "MeshFilter.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"
#include "Runtime/Scripting/ScriptMesh/ScriptMesh.h"

void MeshFilter::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;
    PushToRenderer();
}

void MeshFilter::OnSerializedFieldsUpdated()
{
    PushToRenderer();
}

void MeshFilter::Tick(float delta_time)
{
    (void)delta_time;

    // 只做"补一次没写成的推送"（理由见 `PushToRenderer` 里那节注释）。
    //
    // 判 `m_PushPending` 而不是每帧无脑推：推一次要 `SetSubMeshes`（它会重建
    // override 槽位数组），每帧做是纯粹的浪费 —— 而且 `SetSubMeshes` 里那条
    // "sub-mesh 列表变了"的判定会被每帧同样的内容骗过去。
    if (m_PushPending)
    {
        PushToRenderer();
    }
}

void MeshFilter::SetSharedMeshPath(const eastl::string& mesh_asset)
{
    m_MeshAsset = mesh_asset;
    PushToRenderer();
}

Mesh* MeshFilter::GetMesh()
{
    return m_RuntimeMesh;
}

void MeshFilter::SetMesh(Mesh* mesh)
{
    m_RuntimeMesh = mesh;
    PushToRenderer();
}

bool MeshFilter::HasRenderer() const
{
    return FindRenderer() != nullptr;
}

Bounds MeshFilter::GetLocalBounds() const
{
    Mesh* runtime_mesh = m_RuntimeMesh;
    return (runtime_mesh != nullptr) ? runtime_mesh->GetBounds() : Bounds();
}

BaseRenderer* MeshFilter::FindRenderer() const
{
    // 还没挂到对象上（构造完到 PostLoadResource 之间）：下面那行会解引用空指针。
    // 这不是防御性判空 —— 单测里"造一个裸组件调 SetSharedMeshPath"是唯一能在
    // headless 下走到这条分支的方式，判掉它等于把这条路径变成死代码。
    if (m_ParentObject == nullptr)
    {
        return nullptr;
    }

    // `Cast` 是动态转换，所以这里拿到的是**任意** BaseRenderer 派生类
    // （MeshRenderer / SkinMeshRenderer / SpineRenderer …），与 Unity 的
    // "filter 不关心对面是哪种 renderer" 一致。
    return m_ParentObject->tryGetComponent(BaseRenderer);
}

void MeshFilter::PushToRenderer()
{
    BaseRenderer* renderer = FindRenderer();
    if (renderer == nullptr)
    {
        // 没有渲染器就没有要写回的地方。刻意不报错：一个只有 MeshFilter 的对象
        // 是完全合法的中间状态（比如 prefab 还在拼），而"换了网格画面没变"该由
        // `HasRenderer()` 查，不是靠这里抛。
        //
        // ⚠️ 但要**记下来**，因为"现在没有"不等于"永远没有"：
        // `AddComponent` 是"先 PostLoadResource 再挂进列表"（GameObject.cpp 里
        // 的注释写了为什么），所以"先加 filter 后加 renderer"时，filter 那次
        // PostLoadResource 看不到 renderer —— 不记账的话这份网格就永远丢在
        // 半路上了。挂上 renderer 之后的第一个 Tick 补一次。
        m_PushPending = true;
        return;
    }
    m_PushPending = false;

    // 资产网格：写进 sub-mesh 列表。空路径 -> 空列表（不画东西），与 Unity 的
    // `sharedMesh = null` 同形。
    std::vector<SubMeshRes> sub_meshes;
    if (!m_MeshAsset.empty())
    {
        SubMeshRes sub_mesh;
        sub_mesh.m_MeshAsset = m_MeshAsset;
        // m_Transform 留在默认（单位变换）：filter 只管"哪个网格"，摆位归
        // Transform 组件 —— 在 sub-mesh 上再叠一层位移会和 Transform 打架。
        sub_meshes.push_back(sub_mesh);
    }
    renderer->SetSubMeshes(sub_meshes);

    // 运行时网格：只推包围盒（不参与绘制，理由见头文件）。
    Mesh* runtime_mesh = m_RuntimeMesh;
    if (runtime_mesh != nullptr)
    {
        renderer->SetLocalBounds(runtime_mesh->GetBounds());
    }
    else
    {
        renderer->ClearLocalBoundsOverride();
    }
}
