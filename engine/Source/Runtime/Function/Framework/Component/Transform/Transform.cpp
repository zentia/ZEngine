#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include "Application/Application.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Function/Framework/Component/Rigidbody/RigidbodyComponent.h"
#include "Runtime/Function/Framework/Component/Transform/TransformChangeDispatch.h"
#include "Runtime/Core/Math/LargeWorldCoordinates.h"
#include "Runtime/Function/Framework/Component/Transform/TransformHierarchy.h"
#include "Runtime/Function/Framework/Component/Transform/TransformSceneRoots.h"
#include "Runtime/Function/Framework/World/WorldManager.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <type_traits>

IMPLEMENT_OBJECT_SERIALIZE(Transform)
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Transform)

namespace
{
    // "TransformComponent" (the legacy authoring name) is mapped centrally in
    // RegisterRuntime.cpp's RegisterLegacyTypeAliases(), which runs after
    // TypeManager::InitializeAllTypes(). It cannot live here: Initialize()
    // calls Reset(), which wipes the name->type map and would drop it.

    TransformChangeSystemHandle g_RendererChangeSystem;
    TransformChangeSystemHandle g_PhysicsChangeSystem;
    bool g_TransformSystemsRegistered {false};

    // ---- 欧拉角（度，合成顺序 X -> Y -> Z，即矩阵 Rz·Ry·Rx）--------------
    //
    // 不用 Quaternion::GetYaw/GetPitch/GetRoll：那三个是 Ogre 遗产，按 **Y-up**
    // 定义的（yaw 绕 Y、pitch 绕 X、roll 绕 Z）。在 Z-up 的 ZEngine 里它们返回
    // 的"yaw"其实是绕 Y 轴的角度，而绕 Y 轴在这里是**翻滚**不是转向 —— 名字与
    // 语义对不上，比没有更危险。这里显式按 X->Y->Z 做分解。

    float Rad(float degrees) { return Math::DegreesToRadians(degrees); }
    float Deg(float radians) { return Math::RadiansToDegrees(radians); }

    // ⚠️ 实现搬到 `Quaternion::FromEulerAngles` / `Quaternion::ToEulerAngles`
    // 去了：脚本面的 `Quaternion.Euler(...)` 与这里必须是**同一套**约定，否则
    // "设进去的欧拉角"和"Quaternion.Euler 算出来的朝向"会静默地不是一回事。
    Quaternion EulerToQuaternion(const Vector3& euler_degrees)
    {
        return Quaternion::FromEulerAngles(euler_degrees);
    }

    Vector3 EulerFromQuaternion(const Quaternion& rotation)
    {
        return Quaternion::ToEulerAngles(rotation);
    }

    bool IsAnyNaN(const Quaternion& q)
    {
        return !(q.w == q.w && q.x == q.x && q.y == q.y && q.z == q.z);
    }

    /// 让 local -Y（本引擎的 forward）对准 `forward`，up 尽量贴近 `up_hint`。
    ///
    /// 基必须是右手系且符合 (X=左, Y=-前, Z=上)：
    ///   y_axis = -forward
    ///   x_axis = up × forward     （左）
    ///   z_axis = forward × x_axis （重新正交化后的 up）
    Quaternion OrientationFacing(const Vector3& forward, const Vector3& up_hint)
    {
        Vector3 f = forward.normalisedCopy();
        if (f.isZeroLength())
        {
            return Quaternion::IDENTITY;
        }

        Vector3 up = up_hint;
        if (up.isZeroLength() || std::fabs(f.dotProduct(up.normalisedCopy())) > 0.9999f)
        {
            // 退化：forward 与 up 几乎平行。换一个与 f 不平行的参考轴，而不是
            // 让 crossProduct 得到零向量再 normalize 出 NaN。
            up = (std::fabs(f.z) < 0.9f) ? Vector3::UNIT_Z : Vector3::UNIT_X;
        }
        up.normalise();

        const Vector3 left = up.crossProduct(f).normalisedCopy();
        const Vector3 real_up = f.crossProduct(left).normalisedCopy();

        Quaternion result;
        result.FromAxes(left, -f, real_up);
        result.normalise();
        return result;
    }

    /// 让 local -X（本引擎的 right）对准 `right`，up 尽量贴近 `up_hint`。
    ///
    /// 基的推导与上面两个一致，只是这次已知的是 X 轴：
    ///   x_axis = left  = -right
    ///   z_axis = up    = up_hint 对 left 正交化
    ///   y_axis = -forward = up × left   （右手系：x × y = z）
    Quaternion OrientationWithRight(const Vector3& right, const Vector3& up_hint)
    {
        Vector3 r = right.normalisedCopy();
        if (r.isZeroLength())
        {
            return Quaternion::IDENTITY;
        }

        const Vector3 left = -r;

        Vector3 up = up_hint;
        if (up.isZeroLength() || std::fabs(left.dotProduct(up.normalisedCopy())) > 0.9999f)
        {
            up = (std::fabs(left.z) < 0.9f) ? Vector3::UNIT_Z : Vector3::UNIT_X;
        }
        up.normalise();

        const Vector3 real_up = (up - left * left.dotProduct(up)).normalisedCopy();
        const Vector3 y_axis = real_up.crossProduct(left);

        Quaternion result;
        result.FromAxes(left, y_axis, real_up);
        result.normalise();
        return result;
    }

    /// 让 local +Z 对准 `up`，同时 forward 尽量贴近 `forward_hint`。
    Quaternion OrientationWithUp(const Vector3& forward_hint, const Vector3& up)
    {
        Vector3 u = up.normalisedCopy();
        if (u.isZeroLength())
        {
            return Quaternion::IDENTITY;
        }

        Vector3 f = forward_hint;
        if (f.isZeroLength() || std::fabs(f.normalisedCopy().dotProduct(u)) > 0.9999f)
        {
            f = (std::fabs(u.z) < 0.9f) ? Vector3::NEGATIVE_UNIT_Y : Vector3::UNIT_X;
        }
        f.normalise();

        const Vector3 left = u.crossProduct(f).normalisedCopy();
        const Vector3 real_forward = left.crossProduct(u).normalisedCopy();

        Quaternion result;
        result.FromAxes(left, -real_forward, u);
        result.normalise();
        return result;
    }

    void EnsureTransformSystemsRegistered()
    {
        if (g_TransformSystemsRegistered)
        {
            return;
        }

        TransformChangeDispatch& dispatch = GetTransformChangeDispatch();
        g_RendererChangeSystem =
            dispatch.RegisterSystem("Renderer", TransformChangeDispatch::kInterestedInGlobalTRS);
        g_PhysicsChangeSystem =
            dispatch.RegisterSystem("Physics", TransformChangeDispatch::kInterestedInGlobalTRS);
        g_TransformSystemsRegistered = true;
    }

    TransformTRS MakeTRS(const Vector3d& position, const Quaternion& rotation, const Vector3& scale)
    {
        return TransformTRS(position, rotation, scale);
    }

    Level* GetActiveLevel()
    {
        WorldManager* world_manager = GET_SYSTEM(WorldManager);
        return world_manager != nullptr ? world_manager->getCurrentActiveLevel() : nullptr;
    }
}  // namespace

TransformChangeSystemHandle Transform::GetRendererChangeSystem()
{
    EnsureTransformSystemsRegistered();
    return g_RendererChangeSystem;
}

TransformChangeSystemHandle Transform::GetPhysicsChangeSystem()
{
    EnsureTransformSystemsRegistered();
    return g_PhysicsChangeSystem;
}

namespace
{
    // See kTransferProbesLegacyFields (SerializeUtility.h).
    template<typename TransferFunction>
    inline constexpr bool kProbesLegacyFields = kTransferProbesLegacyFields<TransferFunction>;
}  // namespace

template<typename TransferFunction>
void Transform::Transfer(TransferFunction& transfer)
{
    Super::Transfer(transfer);

    transfer.Transfer(m_LocalRotation, "m_LocalRotation");
    if constexpr (TransferFunction::IsReading())
    {
        // Same order as the writer below (m_LocalPositionD first): a positional reader
        // depends on it. Name-addressed readers do not care.
        transfer.Transfer(m_LocalPosition, "m_LocalPositionD");
        transfer.Transfer(m_LocalPositionLegacy, "m_LocalPosition");
        // Old data without m_LocalPositionD: only a name-addressed reader can meet it
        // (the double field is simply absent). A positional read always gets the
        // writer's pair, where the double copy is authoritative -- falling back there
        // turned a spliced zero position (prefab Apply / Revert / Merge splice one
        // leaf at a time, the legacy copy still holding the old value) back into the
        // old position.
        if (kProbesLegacyFields<TransferFunction> && m_LocalPosition == Vector3d::ZERO &&
            m_LocalPositionLegacy != Vector3::ZERO)
        {
            m_LocalPosition = Vector3d(m_LocalPositionLegacy);
        }
    }
    else
    {
        transfer.Transfer(m_LocalPosition, "m_LocalPositionD");
        m_LocalPositionLegacy = m_LocalPosition.ToVector3();
        transfer.Transfer(m_LocalPositionLegacy, "m_LocalPosition");
    }
    transfer.Transfer(m_LocalScale, "m_LocalScale");

    if constexpr (!kProbesLegacyFields<TransferFunction>)
    {
        transfer.Transfer(m_Father, "m_Father");
        transfer.Transfer(m_Children, "m_Children");
    }
    else
    {
        TransformTRS legacy_transform;
        transfer.Transfer(legacy_transform, "transform");
        if (m_LocalPosition == Vector3d::ZERO && m_LocalPositionLegacy == Vector3::ZERO &&
            (legacy_transform.m_Position != Vector3d::ZERO || legacy_transform.m_Rotation != Quaternion::IDENTITY ||
             legacy_transform.m_Scale != Vector3::UNIT_SCALE))
        {
            m_LocalPosition = legacy_transform.m_Position;
            m_LocalPositionLegacy = m_LocalPosition.ToVector3();
            m_LocalRotation = legacy_transform.m_Rotation;
            m_LocalScale = legacy_transform.m_Scale;
        }

        transfer.Transfer(m_Father, "m_Father");
        PPtr<Transform> legacy_parent;
        transfer.Transfer(legacy_parent, "m_parent");
        if (m_Father == nullptr && legacy_parent != nullptr)
        {
            m_Father = legacy_parent;
        }

        transfer.Transfer(m_Children, "m_Children");
        std::vector<PPtr<Transform>> legacy_children;
        transfer.Transfer(legacy_children, "m_children");
        if (m_Children.empty() && !legacy_children.empty())
        {
            m_Children = std::move(legacy_children);
        }
    }
}

void Transform::SetSceneRootLinks(Transform* prev, Transform* next)
{
    m_PrevSceneRoot = prev;
    m_NextSceneRoot = next;
}

void Transform::ApplySerializedLocalToBuffers()
{
}

void Transform::SyncSerializedToHierarchy()
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformInternal::InitLocalTRS(GetTransformAccess(),
                                    m_LocalPosition,
                                    m_LocalRotation,
                                    m_LocalScale);
}

void Transform::SyncHierarchyToSerialized()
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }

    const TransformTRS& trs = GetLocalTRS(GetTransformAccessReadOnly());
    m_LocalPosition = trs.m_Position;
    m_LocalPositionLegacy = m_LocalPosition.ToVector3();
    m_LocalRotation = trs.m_Rotation;
    m_LocalScale = trs.m_Scale;
}

void Transform::WriteLocalToSerializedFields()
{
    if (IsTransformHierarchyInitialized())
    {
        SyncHierarchyToSerialized();
        return;
    }

}

void Transform::MarkTransformDirty(bool scale_changed)
{
    m_IsDirty = true;
    // Unity 的 `hasChanged` 也是"任何一次改动都置位"，包括父级重挂、兄弟顺序
    // 变化 —— 这些走的都是本函数。只有 `SetHasChanged(false)` 能清掉它。
    m_HasChanged = true;
    if (scale_changed)
    {
        m_IsScaleDirty = true;
    }
}

void Transform::RegisterDefaultChangeInterests()
{
    EnsureTransformSystemsRegistered();
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformAccessReadOnly access = GetTransformAccessReadOnly();
    SetDispatchInterested(g_RendererChangeSystem, true);
    SetDispatchInterested(g_PhysicsChangeSystem, true);
}

void Transform::UpdateSceneRootRegistration(Transform* old_father)
{
    Level* level = GetActiveLevel();
    if (level == nullptr)
    {
        return;
    }

    TransformSceneRoots& roots = GetTransformSceneRootsForLevel(level);
    if (old_father == nullptr)
    {
        roots.OnTransformLeftRoot(this);
    }
    if (m_Father == nullptr)
    {
        roots.OnTransformBecameRoot(this);
    }
}

void Transform::PostLoadResource(GameObject* parent_gobject)
{
    m_ParentObject = parent_gobject;
    ApplySerializedLocalToBuffers();
    MarkTransformDirty(true);
    // 加载是基准线，不是一次"改动"：这里把 hasChanged 清回 false，否则脚本
    // 一拿到刚加载的 transform 就看到 true，轮询式用法（读完清零）第一次必然
    // 误判成"加载后被改过"。
    m_HasChanged = false;

    if (m_Father == nullptr && parent_gobject != nullptr)
    {
        UpdateSceneRootRegistration(nullptr);
    }
}

void Transform::OnSerializedFieldsUpdated()
{
    ApplySerializedLocalToBuffers();
    SyncSerializedToHierarchy();
    if (IsTransformHierarchyInitialized())
    {
        TransformInternal::OnLocalTRSChanged(GetTransformAccess());
        QueueChanges();
    }
    MarkTransformDirty(true);
}

Vector3d Transform::GetLocalPositionD() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).m_Position;
    }
    return m_LocalPosition;
}

Vector3 Transform::GetLocalPosition() const
{
    return GetLocalPositionD().ToVector3();
}

Quaternion Transform::GetLocalRotation() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).m_Rotation;
    }
    return m_LocalRotation;
}

Vector3 Transform::GetLocalScale() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).m_Scale;
    }
    return m_LocalScale;
}

void Transform::SetLocalPosition(const Vector3d& local_position)
{
    m_LocalPosition = local_position;
    m_LocalPositionLegacy = local_position.ToVector3();
    if (IsTransformHierarchyInitialized())
    {
        GetLocalTRSWritable(GetTransformAccess()).m_Position = local_position;
        TransformInternal::OnLocalPositionChanged(GetTransformAccess());
        QueueChanges();
    }

    MarkTransformDirty(false);
}

void Transform::SetLocalRotation(const Quaternion& local_rotation)
{
    m_LocalRotation = local_rotation;
    if (IsTransformHierarchyInitialized())
    {
        GetLocalTRSWritable(GetTransformAccess()).m_Rotation = local_rotation;
        TransformInternal::OnLocalRotationChanged(GetTransformAccess());
        QueueChanges();
    }

    MarkTransformDirty(false);
}

void Transform::SetLocalScale(const Vector3& local_scale)
{
    m_LocalScale = local_scale;
    if (IsTransformHierarchyInitialized())
    {
        GetLocalTRSWritable(GetTransformAccess()).m_Scale = local_scale;
        TransformInternal::OnLocalScaleChanged(GetTransformAccess());
        QueueChanges();
    }

    MarkTransformDirty(true);
}

void Transform::SetLocalPositionAndRotation(const Vector3& local_position, const Quaternion& local_rotation)
{
    SetLocalPosition(local_position);
    SetLocalRotation(local_rotation);
}

Matrix4x4 Transform::GetLocalMatrix() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly()).getMatrix();
    }
    return BuildLocalTRS().getMatrix();
}

Matrix4x4 Transform::GetLocalToWorldMatrix() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalMatrix(GetTransformAccessReadOnly());
    }

    Matrix4x4 world = GetLocalMatrix();
    Transform* parent = m_Father;
    while (parent != nullptr)
    {
        world = parent->GetLocalMatrix() * world;
        parent = parent->GetParent();
    }
    return world;
}

Matrix4x4 Transform::GetWorldToLocalMatrix() const
{
    return GetLocalToWorldMatrix().InverseAffine();
}

Vector3d Transform::GetWorldPositionD() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalPositionD(GetTransformAccessReadOnly());
    }
    return Vector3d(GetLocalToWorldMatrix().GetTrans());
}

Vector3 Transform::GetPosition() const
{
    if (LargeWorldCoordinates::IsEnabled())
    {
        return LargeWorldCoordinates::WorldToRender(GetWorldPositionD());
    }
    return GetWorldPositionD().ToVector3();
}

Quaternion Transform::GetRotation() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalRotation(GetTransformAccessReadOnly());
    }

    Vector3 position;
    Vector3 scale;
    Quaternion rotation;
    GetLocalToWorldMatrix().Decomposition(position, scale, rotation);
    return rotation;
}

Vector3 Transform::GetLossyScale() const
{
    if (IsTransformHierarchyInitialized())
    {
        return CalculateGlobalScaleLossy(GetTransformAccessReadOnly());
    }

    Vector3 position;
    Vector3 scale;
    Quaternion rotation;
    GetLocalToWorldMatrix().Decomposition(position, scale, rotation);
    return scale;
}

void Transform::GetPositionAndRotation(Vector3& world_position, Quaternion& world_rotation) const
{
    world_position = GetPosition();
    world_rotation = GetRotation();
}

void Transform::GetLocalPositionAndRotation(Vector3& local_position, Quaternion& local_rotation) const
{
    local_position = GetLocalPosition();
    local_rotation = GetLocalRotation();
}

void Transform::SetPosition(const Vector3d& world_position)
{
    if (m_Father == nullptr)
    {
        SetLocalPosition(world_position);
        return;
    }

    const Vector3d parent_world = m_Father->GetWorldPositionD();
    const Quaternion parent_rotation = m_Father->GetRotation();
    const Vector3 parent_scale = m_Father->GetLossyScale();
    Vector3 delta_float = (world_position - parent_world).ToVector3();
    delta_float = parent_rotation.inverse() * delta_float;
    Vector3d delta(delta_float);
    if (std::abs(parent_scale.x) > 1e-8)
    {
        delta.x /= parent_scale.x;
    }
    if (std::abs(parent_scale.y) > 1e-8)
    {
        delta.y /= parent_scale.y;
    }
    if (std::abs(parent_scale.z) > 1e-8)
    {
        delta.z /= parent_scale.z;
    }
    SetLocalPosition(delta);
}

void Transform::SetRotation(const Quaternion& world_rotation)
{
    if (m_Father == nullptr)
    {
        SetLocalRotation(world_rotation);
        return;
    }

    const Quaternion parent_rotation = m_Father->GetRotation();
    SetLocalRotation(parent_rotation.inverse() * world_rotation);
}

void Transform::SetPositionAndRotation(const Vector3& world_position, const Quaternion& world_rotation)
{
    SetPosition(world_position);
    SetRotation(world_rotation);
}

Vector3 Transform::TransformPoint(const Vector3& local_point) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::TransformPoint(GetTransformAccessReadOnly(), local_point);
    }
    Vector4 point(local_point.x, local_point.y, local_point.z, 1.0f);
    point = GetLocalToWorldMatrix() * point;
    return Vector3(point.x, point.y, point.z);
}

Vector3 Transform::TransformDirection(const Vector3& local_direction) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::TransformDirection(GetTransformAccessReadOnly(), local_direction);
    }
    return GetRotation() * local_direction;
}

Vector3 Transform::TransformVector(const Vector3& local_vector) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::TransformVector(GetTransformAccessReadOnly(), local_vector);
    }
    Matrix4x4 matrix = GetLocalToWorldMatrix();
    return Vector3(matrix[0][0] * local_vector.x + matrix[0][1] * local_vector.y + matrix[0][2] * local_vector.z,
                   matrix[1][0] * local_vector.x + matrix[1][1] * local_vector.y + matrix[1][2] * local_vector.z,
                   matrix[2][0] * local_vector.x + matrix[2][1] * local_vector.y + matrix[2][2] * local_vector.z);
}

Vector3 Transform::InverseTransformPoint(const Vector3& world_point) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::InverseTransformPoint(GetTransformAccessReadOnly(), world_point);
    }
    Vector4 point(world_point.x, world_point.y, world_point.z, 1.0f);
    point = GetWorldToLocalMatrix() * point;
    return Vector3(point.x, point.y, point.z);
}

Vector3 Transform::InverseTransformDirection(const Vector3& world_direction) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::InverseTransformDirection(GetTransformAccessReadOnly(), world_direction);
    }
    return GetRotation().inverse() * world_direction;
}

Vector3 Transform::InverseTransformVector(const Vector3& world_vector) const
{
    if (IsTransformHierarchyInitialized())
    {
        return ::InverseTransformVector(GetTransformAccessReadOnly(), world_vector);
    }
    Matrix4x4 matrix = GetWorldToLocalMatrix();
    return Vector3(matrix[0][0] * world_vector.x + matrix[0][1] * world_vector.y + matrix[0][2] * world_vector.z,
                 matrix[1][0] * world_vector.x + matrix[1][1] * world_vector.y + matrix[1][2] * world_vector.z,
                 matrix[2][0] * world_vector.x + matrix[2][1] * world_vector.y + matrix[2][2] * world_vector.z);
}

Transform* Transform::GetChild(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Children.size()))
    {
        return nullptr;
    }
    return m_Children[static_cast<size_t>(index)];
}

Transform* Transform::Find(const eastl::string& path) const
{
    if (path.empty())
    {
        return nullptr;
    }

    // 逐段走，而不是先切成一个 vector 再去查：路径绝大多数情况只有一两段，
    // 且任一段缺失就可以立刻返回，没必要为剩下的段分配内存。
    Transform* current = const_cast<Transform*>(this);
    eastl::string::size_type segment_begin = 0;

    while (segment_begin < path.size())
    {
        eastl::string::size_type slash = path.find('/', segment_begin);
        if (slash == eastl::string::npos)
        {
            slash = path.size();
        }
        if (slash == segment_begin)
        {
            // 空段（前导 '/'、连续 '//'、结尾 '/'）。Unity 不把它当"自己"，
            // 这里同样返回 nullptr —— 半个路径的结果没有意义。
            return nullptr;
        }

        const eastl::string segment = path.substr(segment_begin, slash - segment_begin);

        Transform* next = nullptr;
        for (const PPtr<Transform>& child : current->m_Children)
        {
            Transform* child_transform = child;
            if (child_transform == nullptr)
            {
                continue;
            }
            const GameObject* child_object = child_transform->GetParentObject();
            if (child_object != nullptr && child_object->GetName() == segment)
            {
                next = child_transform;
                break;
            }
        }
        if (next == nullptr)
        {
            return nullptr;
        }

        current = next;
        segment_begin = slash + 1;
    }

    return current;
}

Transform* Transform::GetRoot()
{
    Transform* root = this;
    while (root->GetParent() != nullptr)
    {
        root = root->GetParent();
    }
    return root;
}

const Transform* Transform::GetRoot() const
{
    return const_cast<Transform*>(this)->GetRoot();
}

int Transform::GetSiblingIndex() const
{
    if (m_Father == nullptr)
    {
        return 0;
    }

    const std::vector<PPtr<Transform>>& siblings = m_Father->m_Children;
    for (size_t i = 0; i < siblings.size(); ++i)
    {
        if (siblings[i] == this)
        {
            return static_cast<int>(i);
        }
    }
    return 0;
}

void Transform::SetSiblingIndex(int new_index)
{
    Transform* parent = m_Father;
    if (parent == nullptr)
    {
        return;
    }

    std::vector<PPtr<Transform>>& siblings = parent->m_Children;
    if (siblings.empty())
    {
        return;
    }

    if (new_index > static_cast<int>(siblings.size()) - 1)
    {
        new_index = static_cast<int>(siblings.size()) - 1;
    }

    if (new_index >= 0 && new_index < static_cast<int>(siblings.size()) && siblings[static_cast<size_t>(new_index)] == this)
    {
        return;
    }

    ReorderWithinParent(parent, new_index);
}

void Transform::ReorderWithinParent(Transform* parent, int new_index)
{
    std::vector<PPtr<Transform>>& siblings = parent->m_Children;
    if (siblings.empty())
    {
        return;
    }

    const int current_index = GetSiblingIndex();
    PPtr<Transform> self = this;
    siblings.erase(siblings.begin() + current_index);

    if (new_index < 0)
    {
        new_index = 0;
    }
    if (new_index > static_cast<int>(siblings.size()))
    {
        new_index = static_cast<int>(siblings.size());
    }
    siblings.insert(siblings.begin() + new_index, self);

    bool hierarchy_updated = false;
    if (IsTransformHierarchyInitialized() && parent->IsTransformHierarchyInitialized())
    {
        TransformHierarchy& hierarchy = *m_TransformData.hierarchy;
        const uint32_t thread_first = m_TransformData.index;
        const uint32_t thread_last = FindLastChildIndex();
        if (thread_first > 0)
        {
            const uint32_t thread_prev = (new_index > 0)
                                               ? static_cast<Transform*>(siblings[static_cast<size_t>(new_index - 1)])
                                                     ->FindLastChildIndex()
                                               : parent->m_TransformData.index;
            DetachTransformThread(hierarchy, thread_first, thread_last);
            InsertTransformThreadAfter(hierarchy, thread_prev, thread_first, thread_last);
            hierarchy_updated = true;
        }
    }

    if (!hierarchy_updated)
    {
        parent->RebuildTransformHierarchy();
    }
    else
    {
        const TransformChangeDispatch::InterestType sibling_interest =
            TransformChangeDispatch::kInterestedInSiblingOrder;
        const TransformChangeSystemMask sibling_mask =
            GetTransformChangeDispatch().GetChangeMaskForInterest(sibling_interest);
        TransformInternal::OnTransformChangedMask(GetTransformAccess(), 0, sibling_mask, 0);
        QueueChanges();
        GetTransformChangeDispatch().NotifyParentHierarchyChanged(parent->GetTransformAccessReadOnly());
    }

    MarkTransformDirty(true);
}

void Transform::SetAsLastSibling()
{
    SetSiblingIndex(INT_MAX);
}

void Transform::MoveAfterSibling(Transform* sibling)
{
    if (sibling == nullptr || sibling == this)
    {
        return;
    }

    Transform* parent = m_Father;
    // 父节点不同（含"自己是根"）时不搬：跨父级搬移的语义是 SetParent —— 它要
    // 决定 worldPositionStays，这里猜不出来。
    if (parent == nullptr || sibling->m_Father != parent)
    {
        return;
    }

    const std::vector<PPtr<Transform>>& siblings = parent->m_Children;
    int self_index = -1;
    int sibling_index = -1;
    for (size_t i = 0; i < siblings.size(); ++i)
    {
        if (siblings[i] == this)
        {
            self_index = static_cast<int>(i);
        }
        if (siblings[i] == sibling)
        {
            sibling_index = static_cast<int>(i);
        }
    }
    if (self_index < 0 || sibling_index < 0)
    {
        return;
    }

    // 目标下标按**移除自己之后**的列表算，这是最容易写错的一处：
    //   我在它前面 → 移除后它左移一位 → 插到 (sib-1)+1 = sib
    //   我在它后面 → 它不动           → 插到 sib+1
    // 写成 `sibling_index + 1` 的话，"往前挪一个位置"会变成空操作。
    const int target = (self_index < sibling_index) ? sibling_index : sibling_index + 1;
    ReorderWithinParent(parent, target);
}

int Transform::GetHierarchyCount() const
{
    Transform* root = const_cast<Transform*>(this)->GetRoot();
    if (root == nullptr)
    {
        return 1;
    }

    if (root->IsTransformHierarchyInitialized())
    {
        // deep_child_count[0] 是根节点的"自己 + 全部后代"，也就是整份层级的
        // 节点数（含根节点本身）。
        return static_cast<int>(GetDeepChildCount(*root->m_TransformData.hierarchy, 0));
    }

    // 层级还没建（还没进场景）：退化为按 m_Children 递归，数出来的东西一样。
    return static_cast<int>(root->CountNodesDeep());
}

int Transform::GetHierarchyCapacity() const
{
    Transform* root = const_cast<Transform*>(this)->GetRoot();
    if (root == nullptr)
    {
        return 1;
    }

    if (root->IsTransformHierarchyInitialized())
    {
        return static_cast<int>(root->m_TransformData.hierarchy->transform_capacity);
    }

    // 没建层级时"容量"这个概念还不存在（第一次 EnsureTransformHierarchyExists
    // 才分配数组）。返回实际用到的节点数，保证 capacity >= count 恒成立。
    return static_cast<int>(root->CountNodesDeep());
}

void Transform::SetHierarchyCapacity(int capacity)
{
    Transform* root = GetRoot();
    if (root == nullptr || capacity <= 0)
    {
        return;
    }

    // 容量不能小于已经在用的节点数，否则数组装不下活着的节点。
    const int minimum = GetHierarchyCount();
    if (capacity < minimum)
    {
        capacity = minimum;
    }

    root->EnsureTransformHierarchyExists();
    if (!root->IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformHierarchy* hierarchy = root->m_TransformData.hierarchy;
    if (capacity <= static_cast<int>(hierarchy->transform_capacity))
    {
        // 只增不减，见头文件注释。
        return;
    }

    GrowTransformHierarchyCapacity(hierarchy, static_cast<uint32_t>(capacity));
}

// =============================================================================
// 方向 / 欧拉角 / Translate / Rotate / LookAt
// =============================================================================

void Transform::SetForward(const Vector3& direction)
{
    const Quaternion rotation = OrientationFacing(direction, Vector3::UNIT_Z);
    if (!IsAnyNaN(rotation))
    {
        SetRotation(rotation);
    }
}

void Transform::SetUp(const Vector3& direction)
{
    // "up 对准 direction" = 先把 up 转过去，同时尽量保住当前朝向。
    // 直接构造基会让朝向任意，表现为物体绕自身轴乱转一下。
    const Quaternion rotation = OrientationWithUp(GetForward(), direction);
    if (!IsAnyNaN(rotation))
    {
        SetRotation(rotation);
    }
}

void Transform::SetRight(const Vector3& direction)
{
    const Quaternion rotation = OrientationWithRight(direction, GetUp());
    if (!IsAnyNaN(rotation))
    {
        SetRotation(rotation);
    }
}

Vector3 Transform::GetEulerAngles() const
{
    return EulerFromQuaternion(GetRotation());
}

void Transform::SetEulerAngles(const Vector3& euler_angles)
{
    SetRotation(EulerToQuaternion(euler_angles));
}

Vector3 Transform::GetLocalEulerAngles() const
{
    return EulerFromQuaternion(GetLocalRotation());
}

void Transform::SetLocalEulerAngles(const Vector3& euler_angles)
{
    SetLocalRotation(EulerToQuaternion(euler_angles));
}

void Transform::Translate(const Vector3& translation)
{
    // 自身轴：等价于"沿 forward/up/right 走"。走 TransformDirection 才是对的
    // —— 直接用 GetPosition() + translation 会变成世界轴平移。
    SetPosition(GetPosition() + TransformDirection(translation));
}

void Transform::TranslateWorld(const Vector3& translation)
{
    SetPosition(GetPosition() + translation);
}

void Transform::Rotate(const Vector3& euler_angles)
{
    // 自身轴 ⇒ 右乘：先有的姿态再叠加这次的局部旋转。
    SetLocalRotation(GetLocalRotation() * EulerToQuaternion(euler_angles));
}

void Transform::RotateAxis(const Vector3& axis, float angle)
{
    if (axis.isZeroLength())
    {
        return;   // 零轴没有旋转可言；FromAngleAxis 会 normalize 出 NaN
    }
    Quaternion delta;
    delta.FromAngleAxis(Radian(Math::DegreesToRadians(angle)), axis.normalisedCopy());
    // 世界轴 ⇒ 左乘。
    SetRotation(delta * GetRotation());
}

void Transform::RotateAround(const Vector3& point, const Vector3& axis, float angle)
{
    if (axis.isZeroLength())
    {
        return;
    }
    Quaternion delta;
    delta.FromAngleAxis(Radian(Math::DegreesToRadians(angle)), axis.normalisedCopy());

    const Vector3 offset = GetPosition() - point;
    // 与 RotateAxis 同一条规则：绕世界轴 ⇒ 左乘。位置和朝向都要转，只转其中一个
    // 就是"绕着点滑动但始终面朝同一边"。
    SetRotation(delta * GetRotation());
    SetPosition(point + (delta * offset));
}

void Transform::LookAt(const Vector3& world_position)
{
    LookAtWithUp(world_position, Vector3::UNIT_Z);
}

void Transform::LookAtWithUp(const Vector3& world_position, const Vector3& world_up)
{
    const Vector3 direction = world_position - GetPosition();
    if (direction.isZeroLength())
    {
        // 目标就是自己所在的位置：没有唯一解。静默跳过而不是写 NaN —— NaN 会
        // 一路污染到渲染矩阵，而且极难回溯来源。
        return;
    }
    const Quaternion rotation = OrientationFacing(direction, world_up);
    if (!IsAnyNaN(rotation))
    {
        SetRotation(rotation);
    }
}

bool Transform::IsChildOf(Transform* other) const
{
    if (other == nullptr)
    {
        return false;
    }
    for (const Transform* probe = m_Father; probe != nullptr; probe = probe->m_Father)
    {
        if (probe == other)
        {
            return true;
        }
    }
    return false;
}

void Transform::DetachChildren()
{
    // 先抄一份：SetParent(nullptr) 会改动 m_Children（把节点从当前父节点摘掉），
    // 正序遍历一边改一边读会跳过元素 —— 表现为"只解开了一半"。
    std::vector<PPtr<Transform>> children = m_Children;
    for (const PPtr<Transform>& child : children)
    {
        if (child != nullptr)
        {
            child->SetParent(nullptr, true);
        }
    }
}

TransformAccess Transform::GetTransformAccess()
{
    return m_TransformData;
}

void Transform::SetDispatchInterested(TransformChangeSystemHandle system, bool interested)
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }
    GetTransformChangeDispatch().SetSystemInterested(GetTransformAccessReadOnly(), system, interested);
}

void Transform::SetDispatchInterestedAll(bool interested)
{
    EnsureTransformSystemsRegistered();
    SetDispatchInterested(g_RendererChangeSystem, interested);
    SetDispatchInterested(g_PhysicsChangeSystem, interested);
}

void Transform::QueueChanges()
{
    if (!IsTransformHierarchyInitialized())
    {
        return;
    }
    GetTransformChangeDispatch().QueueTransformChangeIfHasChanged(GetTransformAccess());
}

uint32_t Transform::CountNodesDeep() const
{
    uint32_t count = 1;
    for (const PPtr<Transform>& child : m_Children)
    {
        if (Transform* child_transform = child)
        {
            count += child_transform->CountNodesDeep();
        }
    }
    return count;
}

uint32_t Transform::InitializeTransformHierarchyRecursive(TransformHierarchy& hierarchy,
                                                          int& index,
                                                          int32_t parent_index)
{
    const uint32_t new_index = static_cast<uint32_t>(index);
    index = hierarchy.next_indices[new_index];

    TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
    const uint32_t old_index = m_TransformData.index;

    m_TransformData.hierarchy = &hierarchy;
    m_TransformData.index = new_index;
    hierarchy.parent_indices[new_index] = parent_index;
    hierarchy.transform_pointers[new_index] = this;

    if (old_hierarchy == nullptr)
    {
        SyncSerializedToHierarchy();
        RegisterDefaultChangeInterests();
    }
    else
    {
        hierarchy.local_transforms[new_index] = old_hierarchy->local_transforms[old_index];
        hierarchy.system_changed[new_index] = old_hierarchy->system_changed[old_index];
        hierarchy.system_interested[new_index] = old_hierarchy->system_interested[old_index];
    }

    hierarchy.combined_system_changed |= hierarchy.system_changed[new_index];
    hierarchy.combined_system_interest |= hierarchy.system_interested[new_index];

    uint32_t count = 1;
    for (const PPtr<Transform>& child : m_Children)
    {
        if (Transform* child_transform = child)
        {
            count += child_transform->InitializeTransformHierarchyRecursive(hierarchy, index, static_cast<int32_t>(new_index));
        }
    }

    hierarchy.deep_child_count[new_index] = count;
    return count;
}

void Transform::EnsureTransformHierarchyExists()
{
    if (IsTransformHierarchyInitialized())
    {
        return;
    }
    RebuildTransformHierarchy();
}

void Transform::RebuildTransformHierarchy()
{
    Transform* root = GetRoot();
    TransformHierarchy* old_hierarchy = root->m_TransformData.hierarchy;

    const uint32_t node_count = root->CountNodesDeep();
    if (node_count == 0)
    {
        return;
    }

    TransformHierarchy* hierarchy = CreateTransformHierarchy(node_count);
    AllocateTransformThread(*hierarchy, 0, node_count - 1);

    int index = 0;
    root->InitializeTransformHierarchyRecursive(*hierarchy, index, -1);
    assert(index == -1);
    assert(GetDeepChildCount(*hierarchy, 0) == node_count);

    DestroyTransformHierarchy(old_hierarchy);
    QueueChanges();
}

void Transform::Tick(float delta_time)
{
    (void)delta_time;

    if (m_IsDirty)
    {
        TryUpdateRigidBodyComponent();
    }

    if (!g_isPlaying)
    {
        WriteLocalToSerializedFields();
    }
}

void Transform::TryUpdateRigidBodyComponent()
{
    if (!m_ParentObject)
    {
        return;
    }

    RigidBodyComponent* rigid_body_component = m_ParentObject->tryGetComponent(RigidBodyComponent);
    if (rigid_body_component)
    {
        rigid_body_component->UpdateGlobalTransform(BuildLocalTRS(), m_IsScaleDirty);
        m_IsScaleDirty = false;
    }
}

TransformTRS Transform::BuildLocalTRS() const
{
    if (IsTransformHierarchyInitialized())
    {
        return GetLocalTRS(GetTransformAccessReadOnly());
    }
    return TransformTRS(m_LocalPosition, m_LocalRotation, m_LocalScale);
}

void Transform::DetachFromParentList()
{
    Transform* parent = m_Father;
    if (parent == nullptr)
    {
        return;
    }

    auto& siblings = parent->m_Children;
    siblings.erase(std::remove_if(siblings.begin(),
                                  siblings.end(),
                                  [this](const PPtr<Transform>& child) { return static_cast<Transform*>(child) == this; }),
                   siblings.end());
}

void Transform::AttachToParentList()
{
    Transform* parent = m_Father;
    if (parent == nullptr)
    {
        return;
    }

    auto& siblings = parent->m_Children;
    for (const PPtr<Transform>& child : siblings)
    {
        if (static_cast<Transform*>(child) == this)
        {
            return;
        }
    }
    siblings.emplace_back(this);
}

uint32_t Transform::FindLastChildIndex() const
{
    if (!IsTransformHierarchyInitialized())
    {
        return 0;
    }

    const Transform* cur = this;
    while (!cur->m_Children.empty())
    {
        Transform* last_child = cur->m_Children.back();
        if (last_child == nullptr)
        {
            break;
        }
        cur = last_child;
    }
    return cur->m_TransformData.index;
}

void Transform::EnsureCapacityIncrease(uint32_t node_count_increase)
{
    Transform* root = GetRoot();
    if (!root->IsTransformHierarchyInitialized())
    {
        return;
    }

    TransformHierarchy& hierarchy = *root->m_TransformData.hierarchy;
    const uint32_t required_capacity = GetDeepChildCount(hierarchy, 0) + node_count_increase;
    if (required_capacity <= hierarchy.transform_capacity)
    {
        return;
    }

    GrowTransformHierarchyCapacity(&hierarchy, required_capacity * 2);
}

bool Transform::ApplySetParentHierarchy(Transform* previous_father, Transform* new_father)
{
    if (!IsTransformHierarchyInitialized())
    {
        return false;
    }

    if (new_father != nullptr)
    {
        new_father->EnsureTransformHierarchyExists();
        if (!new_father->IsTransformHierarchyInitialized())
        {
            return false;
        }
    }

    if (previous_father != nullptr)
    {
        previous_father->EnsureTransformHierarchyExists();
        if (!previous_father->IsTransformHierarchyInitialized())
        {
            return false;
        }
    }

    const uint32_t node_count = GetDeepChildCount(m_TransformData);
    if (new_father != nullptr &&
        (previous_father == nullptr || previous_father->m_TransformData.hierarchy != new_father->m_TransformData.hierarchy))
    {
        new_father->EnsureCapacityIncrease(node_count);
        if (GetDeepChildCount(*new_father->m_TransformData.hierarchy, 0) + node_count >
            new_father->m_TransformData.hierarchy->transform_capacity)
        {
            return false;
        }
    }

    const uint32_t transform_thread_insert_index =
        new_father != nullptr ? new_father->FindLastChildIndex() : 0;

    if (previous_father == nullptr && new_father != nullptr)
    {
        TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
        TransformHierarchy* new_hierarchy = new_father->m_TransformData.hierarchy;
        uint32_t new_first = 0;
        uint32_t new_last = 0;
        AddTransformSubhierarchy(*old_hierarchy, 0, *new_hierarchy, new_first, new_last, 0);
        InsertTransformThreadAfter(*new_hierarchy, transform_thread_insert_index, new_first, new_last);
        UpdateDeepChildCountUpwards(*new_hierarchy, static_cast<int32_t>(new_father->m_TransformData.index),
                                    static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*new_hierarchy, new_first);
        DestroyTransformHierarchy(old_hierarchy);
        return true;
    }

    if (previous_father != nullptr && new_father == nullptr)
    {
        TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
        const uint32_t old_first = m_TransformData.index;
        const uint32_t old_last = FindLastChildIndex();
        if (old_first == 0)
        {
            return false;
        }

        TransformHierarchy* new_hierarchy = CreateTransformHierarchy(node_count);
        CopyTransformSubhierarchy(*old_hierarchy, old_first, *new_hierarchy, 0);
        DetachTransformThread(*old_hierarchy, old_first, old_last);
        FreeTransformThread(*old_hierarchy, old_first, old_last);
        UpdateDeepChildCountUpwards(*old_hierarchy,
                                    static_cast<int32_t>(previous_father->m_TransformData.index),
                                    -static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*new_hierarchy, 0);
        return true;
    }

    if (previous_father != nullptr && new_father != nullptr &&
        previous_father->m_TransformData.hierarchy == new_father->m_TransformData.hierarchy)
    {
        TransformHierarchy* hierarchy = m_TransformData.hierarchy;
        const uint32_t first = m_TransformData.index;
        const uint32_t last = FindLastChildIndex();
        if (first == 0)
        {
            return false;
        }

        DetachTransformThread(*hierarchy, first, last);
        UpdateDeepChildCountUpwards(*hierarchy,
                                    static_cast<int32_t>(previous_father->m_TransformData.index),
                                    -static_cast<int32_t>(node_count));
        InsertTransformThreadAfter(*hierarchy, transform_thread_insert_index, first, last);
        UpdateDeepChildCountUpwards(*hierarchy,
                                    static_cast<int32_t>(new_father->m_TransformData.index),
                                    static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*hierarchy, first);
        return true;
    }

    if (previous_father != nullptr && new_father != nullptr)
    {
        TransformHierarchy* old_hierarchy = m_TransformData.hierarchy;
        const uint32_t old_first = m_TransformData.index;
        const uint32_t old_last = FindLastChildIndex();
        if (old_first == 0)
        {
            return false;
        }

        TransformHierarchy* new_hierarchy = new_father->m_TransformData.hierarchy;
        uint32_t new_first = 0;
        uint32_t new_last = 0;
        AddTransformSubhierarchy(*old_hierarchy, m_TransformData.index, *new_hierarchy, new_first, new_last, 0);
        DetachTransformThread(*old_hierarchy, old_first, old_last);
        UpdateDeepChildCountUpwards(*old_hierarchy,
                                    static_cast<int32_t>(previous_father->m_TransformData.index),
                                    -static_cast<int32_t>(node_count));
        FreeTransformThread(*old_hierarchy, old_first, old_last);
        InsertTransformThreadAfter(*new_hierarchy, transform_thread_insert_index, new_first, new_last);
        UpdateDeepChildCountUpwards(*new_hierarchy,
                                    static_cast<int32_t>(new_father->m_TransformData.index),
                                    static_cast<int32_t>(node_count));
        UpdateTransformAccessors(*new_hierarchy, new_first);
        return true;
    }

    return false;
}

bool Transform::SetParent(Transform* new_parent, SetParentOptions options)
{
    {
        Transform* probe = new_parent;
        while (probe != nullptr)
        {
            if (probe == this)
            {
                return false;
            }
            probe = probe->GetParent();
        }
    }

    Transform* const old_father = m_Father;
    if (old_father == new_parent)
    {
        return true;
    }

    Transform* const old_root = GetRoot();

    Matrix4x4 world_before;
    const bool preserve_world = (options & kWorldPositionStays) != 0;
    if (preserve_world)
    {
        world_before = GetLocalToWorldMatrix();
    }

    DetachFromParentList();
    m_Father = new_parent;
    AttachToParentList();
    UpdateSceneRootRegistration(old_father);

    const bool hierarchy_updated = ApplySetParentHierarchy(old_father, new_parent);
    if (!hierarchy_updated)
    {
        Transform* const new_root = GetRoot();
        new_root->RebuildTransformHierarchy();
        if (old_root != new_root && old_root != nullptr)
        {
            old_root->RebuildTransformHierarchy();
        }
    }
    else if (IsTransformHierarchyInitialized())
    {
        const TransformChangeDispatch::InterestType parenting_interest =
            static_cast<TransformChangeDispatch::InterestType>(
                TransformChangeDispatch::kInterestedInGlobalTRS |
                TransformChangeDispatch::kInterestedInParentHierarchy);
        const TransformChangeSystemMask common_mask =
            GetTransformChangeDispatch().GetChangeMaskForInterest(parenting_interest);
        TransformInternal::OnTransformChangedMask(GetTransformAccess(), 0, common_mask, 0);
        QueueChanges();
        GetTransformChangeDispatch().NotifyParentHierarchyChanged(GetTransformAccessReadOnly());
        if (old_father != nullptr && old_father->IsTransformHierarchyInitialized())
        {
            GetTransformChangeDispatch().NotifyParentHierarchyChanged(old_father->GetTransformAccessReadOnly());
        }
    }

    if (preserve_world)
    {
        Matrix4x4 local_after;
        if (new_parent != nullptr)
        {
            local_after = new_parent->GetWorldToLocalMatrix() * world_before;
        }
        else
        {
            local_after = world_before;
        }

        Vector3 new_position;
        Vector3 new_scale;
        Quaternion new_rotation;
        local_after.Decomposition(new_position, new_scale, new_rotation);
        SetLocalPosition(Vector3d(new_position));
        SetLocalScale(new_scale);
        SetLocalRotation(new_rotation);
    }

    MarkTransformDirty(true);

    // Unity 的 `OnTransformParentChanged()`。
    //
    // 只发给**宿主对象自己**的组件，不发给子树 —— 改父级的是这个对象，子对象
    // 的 `transform.parent` 没动（Unity 同样如此）。
    //
    // 放在末尾而不是开头：消息到达时层级必须已经是新的了。脚本在这个回调里
    // 读 `transform.parent` / `transform.position` 是常规用法，早一步派发它
    // 读到的就是旧父级 —— 而"读到的父级偶尔是上一个"没有任何报错。
    if (GameObject* const owner = GetParentObject())
    {
        owner->BroadcastAppEvent(AppEventKind::kParentChanged, false);
    }

    // Unity 的 `OnTransformChildrenChanged()`：同一件事的**另一半**，发给两个
    // **父级**（旧的少了这个孩子、新的多了这个孩子），不是发给这个孩子本身。
    //
    // `old_father == new_parent` 的情况在函数开头就 return 了，所以这里的两个
    // 是**一定不同**的两个对象 —— 不存在"同一对象收两次"的问题。
    //
    // ⚠️ 判空不是防御性编程：脱离父级（`SetParent(nullptr)`）时 new_parent 就是
    // 空的，此时只有旧父级该收到"少了一个孩子"。
    if (old_father != nullptr)
    {
        if (GameObject* const old_owner = old_father->GetParentObject())
        {
            old_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);
        }
    }
    if (new_parent != nullptr)
    {
        if (GameObject* const new_owner = new_parent->GetParentObject())
        {
            new_owner->BroadcastAppEvent(AppEventKind::kChildrenChanged, false);
        }
    }

    return true;
}
