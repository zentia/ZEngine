#include "Runtime/Function/AI/Navigation/NavMeshAgentComponent.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"

#include <cmath>

// 中间航点的"算走过"半径。
//
// 为什么这么小（5cm）而不是用 `m_Radius`：string pulling 产出的航点本来就落在
// 障碍拐角上，用一个和代理半径同量级的通过判定会把这些拐角切掉，代理就会贴着
// 墙角斜插过去 —— 看起来路径更短，实际会卡进几何体。终点另算（用
// `m_StoppingDistance`），因为终点不需要"贴着走"。
namespace
{
    constexpr float k_intermediate_waypoint_radius = 0.05f;

    /// 水平距离（Z-up：高度不参与导航推进与到点判定）。
    float HorizontalDistance(const Vector3& a, const Vector3& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }
}

Transform* NavMeshAgentComponent::GetOwnerTransform() const
{
    if (m_ParentObject == nullptr)
    {
        return nullptr;
    }
    return m_ParentObject->tryGetComponent<Transform>("Transform");
}

bool NavMeshAgentComponent::RequestPath(const Vector3& target, bool update_status)
{
    NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
    if (navigation_system == nullptr)
    {
        return false;
    }

    Transform* transform = GetOwnerTransform();
    if (transform == nullptr)
    {
        return false;
    }

    const NavPath path = navigation_system->FindPath(transform->GetPosition(), target, /*smooth=*/true);

    NavPathStatusValue status = ToPathStatusValue(path.status);

    // Unity 的 `PathPartial`：路是通的，但到不了你给的**那个点**（目标被吸附到
    // 了最近的格子）。`FindPath` 自己只报 success，这里按"终点被挪了多远"补出
    // 这个区分 —— 少了它，"绕不到门口"和"这条路根本不通"在脚本里是同一个值，
    // 而这两种情况的处置完全不同（前者该重试，后者该换目标）。
    if (path.status == NavPath::Status::success && !path.waypoints.empty())
    {
        // 判据是"目标点自己站不站得住"，不是"终点离目标多远" —— `FindPath` 会
        // 把最后一个航点直接写成 `goal`，所以那个距离恒为 0，量不出吸附。
        //
        // 用 `IsStandable`（而不是只看格子的 walkable 位）：墙顶那一格也是
        // walkable 的（NavMesh 按"射线打到的第一个面"取样，墙顶就是个孤岛），
        // 但它离地面 3 米，站不上去。只看 walkable 位会把"目标在墙里"误判成
        // "能到"。
        if (!navigation_system->IsStandable(target))
        {
            status = NavPathStatusValue::partial;
        }
    }

    if (update_status)
    {
        m_PathStatus = status;
    }

    if (!path.IsValid())
    {
        m_Waypoints.clear();
        m_WaypointIndex = 0;
        m_HasPath = false;
        return false;
    }

    m_Waypoints = path.waypoints;
    m_WaypointIndex = 0;
    m_HasPath = true;
    return true;
}

bool NavMeshAgentComponent::SetDestination(const Vector3& target)
{
    m_Destination = target;
    m_HasDestination = true;

    // 目标点先记下来再寻路（见头文件注释）：寻路失败不代表目标无效，网格重建
    // 之后 autoRepath 会自己重试。
    const bool found = RequestPath(target, /*update_status=*/true);
    m_RepathTimer = 0.f;

    if (!found)
    {
        LOG_WARNING(ZGame, "NavMeshAgent: no path to (%.2f, %.2f, %.2f)", target.x, target.y, target.z);
    }
    return found;
}

bool NavMeshAgentComponent::CalculatePath(const Vector3& target)
{
    // 与 SetDestination 的区别只有"不开始走"：不写 m_Destination，也不重置
    // 重算计时器 —— 它是一次纯查询，不该影响正在进行的移动。
    return RequestPath(target, /*update_status=*/true);
}

void NavMeshAgentComponent::ResetPath()
{
    m_Waypoints.clear();
    m_WaypointIndex = 0;
    m_HasPath = false;
    m_Velocity = Vector3::ZERO;
    m_RepathTimer = 0.f;
}

Vector3 NavMeshAgentComponent::GetWaypoint(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Waypoints.size()))
    {
        return Vector3::ZERO;
    }
    return m_Waypoints[index];
}

float NavMeshAgentComponent::ComputeRemainingDistance(const Vector3& from) const
{
    const int count = static_cast<int>(m_Waypoints.size());
    if (!m_HasPath || count == 0 || m_WaypointIndex >= count)
    {
        return 0.f;
    }

    float total = HorizontalDistance(m_Waypoints[m_WaypointIndex], from);
    for (int i = m_WaypointIndex + 1; i < count; ++i)
    {
        total += HorizontalDistance(m_Waypoints[i], m_Waypoints[i - 1]);
    }
    return total;
}

float NavMeshAgentComponent::GetRemainingDistance() const
{
    const Transform* transform = GetOwnerTransform();
    if (transform == nullptr)
    {
        return 0.f;
    }

    // 扣掉 `m_StoppingDistance`：Unity 的 remainingDistance 在进入停止半径后就
    // 是 0，而"还差 0.4 米"和"已经到了"对上层逻辑（播到点动画、切状态）是两
    // 回事 —— 不扣的话永远差一点点，状态机就切不过去。
    const float remaining = ComputeRemainingDistance(transform->GetPosition()) - m_StoppingDistance;
    return (remaining > 0.f) ? remaining : 0.f;
}

bool NavMeshAgentComponent::IsOnNavMesh() const
{
    NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
    if (navigation_system == nullptr || !navigation_system->IsReady())
    {
        return false;
    }

    const Transform* transform = GetOwnerTransform();
    if (transform == nullptr)
    {
        return false;
    }
    return navigation_system->IsStandable(transform->GetPosition());
}

void NavMeshAgentComponent::Stop()
{
    m_IsStopped = true;
    m_Velocity = Vector3::ZERO;
}

void NavMeshAgentComponent::Resume()
{
    m_IsStopped = false;
    // 起来之后立刻重算一次：停着的时候网格可能重建过，而旧航点是按旧网格算的。
    m_RepathTimer = m_RepathInterval;
}

void NavMeshAgentComponent::SetIsStopped(bool stopped)
{
    if (stopped)
    {
        Stop();
    }
    else
    {
        Resume();
    }
}

bool NavMeshAgentComponent::Warp(const Vector3& new_position)
{
    Transform* transform = GetOwnerTransform();
    if (transform == nullptr)
    {
        return false;
    }

    transform->SetPosition(new_position);
    // 传送之后旧航点全是从别处算出来的，留着只会让代理往回走。
    ResetPath();
    m_NextPosition = new_position;

    NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
    return (navigation_system != nullptr) && navigation_system->IsStandable(new_position);
}

void NavMeshAgentComponent::Move(const Vector3& offset)
{
    Transform* transform = GetOwnerTransform();
    if (transform == nullptr)
    {
        return;
    }

    transform->SetPosition(transform->GetPosition() + offset);
    m_NextPosition = transform->GetPosition();
}

void NavMeshAgentComponent::TurnTowardDirection(const Vector3& direction, float delta_time, Transform* transform)
{
    Vector3 to_target = direction;
    to_target.z = 0.f;  // 只偏航：代理不该朝移动方向俯仰
    if (to_target.squaredLength() <= 1e-8f)
    {
        return;
    }
    to_target.normalise();

    const Quaternion current_rotation = transform->GetRotation();
    Vector3 current_forward = current_rotation * Vector3::NEGATIVE_UNIT_Y;
    current_forward.z = 0.f;
    if (current_forward.squaredLength() <= 1e-8f)
    {
        current_forward = Vector3::NEGATIVE_UNIT_Y;
    }
    current_forward.normalise();

    const float alignment = Math::Clamp(current_forward.dotProduct(to_target), -1.f, 1.f);
    Vector3 turn_axis = current_forward.crossProduct(to_target);

    // 叉积接近零说明"已经朝着"或"正好背着"，而叉积长度分不出这两者 —— 只靠
    // atan2(长度, 点积) 会把 180° 当成"已经对齐"，代理就一直背对目标站着。
    float angle = 0.f;
    if (turn_axis.squaredLength() > 1e-8f)
    {
        turn_axis.normalise();
        angle = Math::atan2(turn_axis.length(), alignment).valueRadians();
    }
    else if (alignment < 0.f)
    {
        turn_axis = Vector3::UNIT_Z;  // Z-up：绕竖直轴转，且每次都转同一边
        angle = Math_PI;
    }
    else
    {
        return;
    }

    const float max_step = Math::DegreesToRadians(m_AngularSpeed) * delta_time;
    const float step = (Math::abs(angle) <= max_step) ? angle : ((angle > 0.f ? 1.f : -1.f) * max_step);

    Quaternion next_rotation = Quaternion(Radian(step), turn_axis) * current_rotation;
    next_rotation.normalise();
    transform->SetRotation(next_rotation);
}

void NavMeshAgentComponent::Tick(float delta_time)
{
    Transform* transform = GetOwnerTransform();
    if (transform == nullptr)
    {
        return;
    }
    if (delta_time <= 0.f)
    {
        return;
    }

    Vector3 position = transform->GetPosition();
    m_NextPosition = position;

    // 走到一半路被堵死（动态障碍）时自动重算。只在**没有路**时重试：有路还重
    // 算的话每 0.5 秒一次 A*，几十个代理就把帧吃掉了。
    m_RepathTimer += delta_time;
    if (m_HasDestination && m_AutoRepath && !m_HasPath && m_RepathTimer >= m_RepathInterval && !m_IsStopped)
    {
        m_RepathTimer = 0.f;
        RequestPath(m_Destination, /*update_status=*/true);
    }

    // ---- 停下 / 没路：按加速度把速度收掉 --------------------------------
    //
    // 不是直接 `m_Velocity = ZERO`：从全速一帧归零，视觉上是"撞墙"，而且
    // `GetVelocity()` 给出去的速度曲线在停的那一帧是断的（根运动会跳）。
    if (m_IsStopped || !m_HasPath || m_Waypoints.empty())
    {
        const float current_speed = m_Velocity.length();
        const float decel_step = m_Acceleration * delta_time;
        if (current_speed <= decel_step || current_speed < 1e-6f)
        {
            m_Velocity = Vector3::ZERO;
        }
        else
        {
            m_Velocity = m_Velocity - m_Velocity * (decel_step / current_speed);
        }
        return;
    }

    const int count = static_cast<int>(m_Waypoints.size());

    // ---- 跳过已经走过的航点 --------------------------------------------
    while (m_WaypointIndex < count)
    {
        const Vector3& waypoint = m_Waypoints[m_WaypointIndex];
        const bool is_last = (m_WaypointIndex == count - 1);
        const float arrive_radius = is_last ? m_StoppingDistance : k_intermediate_waypoint_radius;

        const Vector3 to_waypoint = waypoint - position;
        if (to_waypoint.length() <= arrive_radius)
        {
            ++m_WaypointIndex;
            continue;
        }

        // 高速时一帧可能跨过航点（3.5 m/s @ 60fps ≈ 5.8cm，比上面的半径还大）。
        // 只用距离判定的话会在这个航点前多停一帧，表现为走直线时周期性一顿。
        const bool already_past = (m_Velocity.squaredLength() > 1e-8f) && (to_waypoint.dotProduct(m_Velocity) < 0.f);
        if (already_past)
        {
            ++m_WaypointIndex;
            continue;
        }
        break;
    }

    if (m_WaypointIndex >= count)
    {
        // 走完：清路径、归零速度，但**保留** m_PathStatus —— 上层要能看出这一
        // 次是"完整到达"还是"只到了最近点"。
        m_HasPath = false;
        m_Waypoints.clear();
        m_WaypointIndex = 0;
        m_Velocity = Vector3::ZERO;
        return;
    }

    // ---- 朝当前航点走 ---------------------------------------------------
    const bool   is_last_waypoint = (m_WaypointIndex == count - 1);
    const Vector3 to_waypoint = m_Waypoints[m_WaypointIndex] - position;
    const float  distance_to_waypoint = to_waypoint.length();

    float desired_speed = m_Speed;
    if (m_AutoBraking)
    {
        // v² = 2·a·d：在还剩 d 的时候把速度压到 sqrt(2ad)，正好在终点滑到 0。
        // 用**整条剩余路径**的长度而不是当前这一段，否则每个拐角都会先全速冲
        // 一下再急停。
        const float remaining = ComputeRemainingDistance(position) - m_StoppingDistance;
        const float brake_distance = (remaining > 0.f) ? remaining : 0.f;
        const float braking_speed = std::sqrt(2.f * m_Acceleration * brake_distance);
        desired_speed = Math::min(desired_speed, braking_speed);
    }

    const float inverse_distance = (distance_to_waypoint > 1e-6f) ? (1.f / distance_to_waypoint) : 0.f;
    const Vector3 desired_velocity = to_waypoint * (inverse_distance * desired_speed);

    const Vector3 velocity_delta = desired_velocity - m_Velocity;
    const float velocity_delta_length = velocity_delta.length();
    const float max_velocity_delta = m_Acceleration * delta_time;
    if (velocity_delta_length <= max_velocity_delta)
    {
        m_Velocity = desired_velocity;
    }
    else
    {
        m_Velocity += velocity_delta * (max_velocity_delta / velocity_delta_length);
    }

    if (m_Velocity.length() > m_Speed)
    {
        m_Velocity = m_Velocity.normalisedCopy() * m_Speed;
    }

    const Vector3 new_position = position + m_Velocity * delta_time;
    m_NextPosition = new_position;
    if (m_UpdatePosition)
    {
        transform->SetPosition(new_position);
    }
    if (m_UpdateRotation && m_Velocity.squaredLength() > 1e-6f)
    {
        TurnTowardDirection(m_Velocity, delta_time, transform);
    }

    // ---- 到点 ------------------------------------------------------------
    //
    // 判据用"水平距离进了停止半径"，不用航点下标走完：终点被吸附过（PathPartial）
    // 时下标会走完，但代理离真正的目标点还远 —— 那种情况不该算到达。
    //
    // 用**移动之后**的 new_position 而不是帧初的 position：用帧初值的话，进到
    // 停止半径内的那一帧不算到达，得等下一帧由"航点走完"那条分支来收尾 —— 于是
    // "到达时清掉 hasPath"这件事被拆到两处，改坏一处还有另一处兜着，测不出来。
    if (is_last_waypoint && HorizontalDistance(new_position, m_Waypoints.back()) <= m_StoppingDistance)
    {
        m_HasPath = false;
        m_Waypoints.clear();
        m_WaypointIndex = 0;
        m_Velocity = Vector3::ZERO;
        // 目标**清掉**：到了就说明这次任务结束，留着会让 autoRepath 在原地反复
        // 重算同一条路。注意这与 `ResetPath()` 不同 —— 那个是"路作废了、目标仍
        // 有效"（比如刚被推离网格），只清路。
        m_Destination = Vector3::ZERO;
        m_HasDestination = false;
    }
}
