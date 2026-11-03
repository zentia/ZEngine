#include "JointComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Runtime/Function/Physics/Physics.h"
#include "Runtime/Function/Physics/PhysicsScene.h"

std::shared_ptr<PhysicsScene> JointComponent::GetActivePhysicsScene() const
{
    // 走 `Physics::CurrentScene()` 而不是自己写一遍 WorldManager 查询：那份实
    // 现里已经有一条"没有 WorldManager 时退回 PhysicsManager 的主场景"的兜底
    //（`Physics` 门面能在无头测试里工作就是靠它）。关节要和射线查询落在**同
    // 一个**场景里 —— 各查各的会得到"射线打得到、关节建不出"这种只在无头
    // 环境下出现的怪事。
    return Physics::CurrentScene();
}

void JointComponent::PostLoadResource(GameObject* parent_object)
{
    m_ParentObject = parent_object;

    // 与 RigidBodyComponent 同一条时序：`Level::load()` 创建组件时物理场景还没
    // 发布为 active，宿主和连接体的 body 都还不存在 —— 一个 body 都没有的关节
    // 建不出来。推迟到 `FlushPendingJoint()`（WorldManager 在关卡 active 之后
    // 调，且在刚体那一批之后）。
    m_HasPendingJoint = true;
    FlushPendingJoint();
}

bool JointComponent::FlushPendingJoint()
{
    if (!m_HasPendingJoint)
    {
        return false;
    }

    if (!BuildJoint())
    {
        // 保持 pending，下一次 flush 再试。最常见的原因是连接体的 body 还没建
        // 出来（它在另一个对象上，可能这一帧才补建）。
        return false;
    }

    m_HasPendingJoint = false;
    return true;
}

bool JointComponent::BuildJointDesc(PhysicsJointDesc& out_desc, uint32_t& out_other_body_id)
{
    if (m_ParentObject == nullptr)
    {
        return false;
    }

    const RigidBodyComponent* self_body = m_ParentObject->tryGetComponentConst(RigidBodyComponent);
    if (self_body == nullptr || !self_body->HasBody())
    {
        return false;
    }

    const uint32_t self_id = self_body->GetBodyId();
    if (self_id == s_InvalidRigidbodyId)
    {
        return false;
    }

    const Vector3 self_position = self_body->GetBodyPosition();
    const Quaternion self_rotation = self_body->GetBodyRotation();

    // 轴先归一化：`CreateJoint` 里也归一化一次，这里多做一次是为了
    // `isZeroLength()` 这个判据 —— 零向量归一化在各家实现里不是 nan 就是原样
    // 返回，而"轴写成零向量"必然是脚本笔误，早拒早好。
    const Vector3 world_axis = (self_rotation * m_JointRes.m_Axis).normalisedCopy();
    if (world_axis.isZeroLength())
    {
        LOG_ERROR(ZPhysics, "JointComponent: axis is a zero vector, refusing to build the joint");
        return false;
    }

    const Vector3 world_anchor = self_position + self_rotation * m_JointRes.m_Anchor;

    // CharacterJoint 的"摆动轴"：与扭转轴垂直、定出摆动的两个方向。零向量只在
    // 脚本笔误时出现，这里退化成"任取一条垂直轴"而不是拒掉整个关节 —— 它不决定
    // 关节存不存在，只决定摆动的两个方向怎么摆。（退化本身由
    // `PhysicsScene::PlaneAxisFor` 兜底。）
    const Vector3 world_plane_axis = (self_rotation * m_JointRes.m_SwingAxis).normalisedCopy();

    RigidBodyComponent* connected_body = m_JointRes.m_ConnectedBody;
    if (connected_body != nullptr)
    {
        if (!connected_body->HasBody() || connected_body->GetBodyId() == s_InvalidRigidbodyId)
        {
            return false;
        }
        out_other_body_id = connected_body->GetBodyId();

        // autoConfigure 的意思是"让连接点落在当前姿态正好重合的位置上"，也就是
        // 直接复用宿主算出来的世界锚点 —— 这正是 Unity 里"挂着不动"的初值。
        out_desc.m_PivotB = m_JointRes.m_AutoConfigureConnectedAnchor
                                ? world_anchor
                                : connected_body->GetBodyPosition() +
                                      connected_body->GetBodyRotation() * m_JointRes.m_ConnectedAnchor;
    }
    else
    {
        // 拴在世界：Unity 的 `connectedAnchor` 在没有连接体时是**世界空间**坐标
        //（有连接体时才是连接体的局部空间）。少这一半区分的话，"把弹簧挂到世界
        // 某一点"会变成"挂到原点"。
        out_other_body_id = s_InvalidRigidbodyId;
        out_desc.m_PivotB = m_JointRes.m_AutoConfigureConnectedAnchor ? world_anchor : m_JointRes.m_ConnectedAnchor;
    }

    out_desc.m_Type = GetJointType();
    out_desc.m_BodyA = self_id;
    out_desc.m_PivotA = world_anchor;
    out_desc.m_AxisA = world_axis;
    // 两个刚体共用一个世界轴：Unity 的 `Joint.axis` 只有一个，Jolt 的第二个轴
    // 存在是为了两端姿态不同的情况，本引擎的脚本面不给那种用法。
    out_desc.m_AxisB = world_axis;
    out_desc.m_PlaneAxisA = world_plane_axis;
    out_desc.m_PlaneAxisB = world_plane_axis;
    // ConfigurableJoint 的参考系：X = axis，Y = secondaryAxis。零向量 / 与 X
    // 平行都由后端 `PlaneAxisFor` 兜底（那里换成一条垂直轴），所以这里不拒 ——
    // 它不决定关节存不存在，只决定三个方向怎么摆。
    const Vector3 world_secondary_axis = (self_rotation * m_JointRes.m_SecondaryAxis).normalisedCopy();
    out_desc.m_SecondaryAxisA = world_secondary_axis;
    out_desc.m_SecondaryAxisB = world_secondary_axis;
    out_desc.m_AutoDetectPoint = false;

    FillJointLimits(out_desc);

    return true;
}

bool JointComponent::BuildJoint()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return false;
    }

    PhysicsJointDesc desc;
    uint32_t other_body_id = s_InvalidRigidbodyId;
    if (!BuildJointDesc(desc, other_body_id))
    {
        return false;
    }

    if (other_body_id == s_InvalidRigidbodyId)
    {
        // 没有连接体 ⇒ 造一个"世界锚点"。Jolt 的约束必须有两个 body，没有
        // "世界"这一端。
        m_AnchorBodyId = physics_scene->CreateStaticAnchorBody(desc.m_PivotB);
        if (m_AnchorBodyId == s_InvalidRigidbodyId)
        {
            return false;
        }
        desc.m_BodyB = m_AnchorBodyId;
    }
    else
    {
        desc.m_BodyB = other_body_id;
    }

    const uint32_t joint_id = physics_scene->CreateJoint(desc);
    if (joint_id == kInvalidJointId)
    {
        // 锚点是**为这个关节**造的，关节没造就别留着它 —— 否则每次失败都往
        // 场景里掉一个看不见的静态 body。
        if (m_AnchorBodyId != s_InvalidRigidbodyId)
        {
            physics_scene->RemoveRigidBody(m_AnchorBodyId);
            m_AnchorBodyId = s_InvalidRigidbodyId;
        }
        return false;
    }

    m_JointId = joint_id;
    // `enableCollision` 在 `ApplyJointSettings()` **之外**推 —— 那个是虚函数，
    // 子类覆写后不回调基类（见头文件里的注释）。
    PushEnableCollision();
    ApplyJointSettings();

    // 组件在关节建出来**之前**被停用过（`m_Enabled` 是序列化的，关卡加载时就
    // 可能是 false）：Unity 里停用的关节组件等于没有关节。
    if (!m_Enabled)
    {
        physics_scene->SetJointEnabled(m_JointId, false);
    }

    return true;
}

void JointComponent::DestroyJoint()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();

    if (m_JointId != kInvalidJointId)
    {
        if (physics_scene)
        {
            physics_scene->RemoveJoint(m_JointId);
        }
        m_JointId = kInvalidJointId;
    }

    if (m_AnchorBodyId != s_InvalidRigidbodyId)
    {
        if (physics_scene)
        {
            physics_scene->RemoveRigidBody(m_AnchorBodyId);
        }
        m_AnchorBodyId = s_InvalidRigidbodyId;
    }
}

bool JointComponent::RebuildJoint()
{
    DestroyJoint();

    const bool built = BuildJoint();
    // 建不出来就回到 pending：下一次 `FlushPendingJoints` 还会再试。写成
    // "失败即放弃"的话，一个 Awake 里设过 anchor 的关节就永久消失了。
    m_HasPendingJoint = !built;
    return built;
}

JointComponent::~JointComponent()
{
    DestroyJoint();
}

RigidBodyComponent* JointComponent::GetConnectedBody() const
{
    return m_JointRes.m_ConnectedBody;
}

void JointComponent::SetConnectedBody(RigidBodyComponent* connected_body)
{
    m_JointRes.m_ConnectedBody = connected_body;
    RebuildJoint();
}

void JointComponent::SetAnchor(const Vector3& anchor)
{
    m_JointRes.m_Anchor = anchor;
    RebuildJoint();
}

void JointComponent::SetConnectedAnchor(const Vector3& connected_anchor)
{
    m_JointRes.m_ConnectedAnchor = connected_anchor;
    RebuildJoint();
}

void JointComponent::SetAxis(const Vector3& axis)
{
    m_JointRes.m_Axis = axis;
    RebuildJoint();
}

void JointComponent::SetAutoConfigureConnectedAnchor(bool auto_configure)
{
    m_JointRes.m_AutoConfigureConnectedAnchor = auto_configure;
    RebuildJoint();
}

void JointComponent::PushEnableCollision()
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        // 还没建出来：值已经存在 res 里，`BuildJoint` 成功时会再推一次。
        return;
    }
    physics_scene->SetJointEnableCollision(m_JointId, m_JointRes.m_EnableCollision);
}

void JointComponent::SetEnableCollision(bool enable)
{
    m_JointRes.m_EnableCollision = enable;
    // 不重建：后端是事后改两个 body 的碰撞组。
    PushEnableCollision();
}

Vector3 JointComponent::GetCurrentForce() const
{
    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene || m_JointId == kInvalidJointId)
    {
        return Vector3::ZERO;
    }
    Vector3 force = Vector3::ZERO;
    physics_scene->GetJointForce(m_JointId, force);
    return force;
}

bool JointComponent::RepairJoint()
{
    if (!m_HasBroken)
    {
        return HasJoint();
    }

    m_HasBroken = false;
    Component::SetEnabled(true);
    return RebuildJoint();
}

void JointComponent::FixedTick(float fixed_delta_time)
{
    // 断了、没关节、步长无效 —— 三种都不用查。步长 <= 0 除下去是 inf / nan，
    // 那会让"没超限"变成"看运气"。
    if (m_HasBroken || m_JointId == kInvalidJointId || fixed_delta_time <= 0.f)
    {
        return;
    }

    // 两个阈值都是"永不断"时一次冲量都不用问：这是绝大多数关节的常态，而
    // `GetJointImpulse` 要过一遍 Jolt 的 switch。
    if (m_JointRes.m_BreakForce >= kInfiniteBreak && m_JointRes.m_BreakTorque >= kInfiniteBreak)
    {
        return;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (!physics_scene)
    {
        return;
    }

    float linear_impulse = 0.f;
    float angular_impulse = 0.f;
    if (!physics_scene->GetJointImpulse(m_JointId, linear_impulse, angular_impulse))
    {
        return;
    }

    // 冲量 / 步长 = 力。约束的 lambda 是"上一步为了维持约束施加了多少冲量"，
    // 这正是 Unity 判断裂用的那个量。
    const float force = linear_impulse / fixed_delta_time;
    const float torque = angular_impulse / fixed_delta_time;

    if (force <= m_JointRes.m_BreakForce && torque <= m_JointRes.m_BreakTorque)
    {
        return;
    }

    LOG_INFO(ZPhysics, "joint {} broke (force {:.1f} / torque {:.1f})", m_JointId, force, torque);

    m_HasBroken = true;
    // Unity 会 `Destroy` 掉这个组件；这里关掉它（同时关掉 Jolt 那一侧）。
    // 在 FixedTick 中间自销毁组件会把正在遍历的组件列表抽掉一块。
    SetEnabled(false);
}

void JointComponent::SetEnabled(bool enabled)
{
    Component::SetEnabled(enabled);

    if (m_JointId == kInvalidJointId)
    {
        return;
    }

    std::shared_ptr<PhysicsScene> physics_scene = GetActivePhysicsScene();
    if (physics_scene)
    {
        physics_scene->SetJointEnabled(m_JointId, enabled);
    }
}
