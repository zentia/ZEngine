#pragma once

// 导航代理（Unity 的 `NavMeshAgent`）。
//
// 为什么补这个
// ------------
// 引擎原本有完整的导航网格（`NavMesh`：A* + string pulling + 可站性查询）和
// 一个 `NavigationSystem`，但**没有"会走路的那个东西"** —— AI 层只能拿到一条
// `NavPath` 的航点数组，然后各自现写"怎么沿数组走"。结果是每个 AI 动作都在重
// 新实现同一套加速 / 限速 / 转向 / 到点判定，而且各自的口径不同（有的直接
// `SetPosition` 瞬移、有的不减速直接冲过终点）。Unity 里这个职责就是
// NavMeshAgent，所以按 Unity 的契约补一个组件：脚本只设 `destination`，剩下的
// 由组件每帧推进。
//
// 与 MotorComponent 的关系
// ------------------------
// `MotorComponent` 是**角色控制器**（胶囊 + 重力 + 台阶），它由 `AIMoveIntent`
// 驱动、只接受"方向 + 速度比例"，自己不做寻路。本组件是**导航代理**：自己寻路、
// 自己积分速度、直接写 Transform。两者可以叠加（Motor 负责碰撞、Agent 负责出
// 方向），但默认路径是本组件直接写 Transform —— 与 Unity 一致（Unity 的
// NavMeshAgent 也是直接驱动 transform，除非 `updatePosition = false`）。
//
// 轴向：Z-up
// ----------
// `NavMesh` 的网格铺在 XY 平面、+Z 为上，所以这里的"水平"是 x/y，"高度"是 z。
// 转向只做**偏航**（绕 Z）—— 和 `MotorComponent::TurnToward` 一样把 z 分量抹掉，
// 代理不该朝目标俯仰。
//
// 朝向约定：本地 -Y 为前
// ----------------------
// 与 `MotorComponent` 一致（`current_rotation * Vector3::NEGATIVE_UNIT_Y`），
// 不重新定义。转向用叉积 + 轴角而不是"算 yaw 再插值角度"，后者在 ±180° 附近会
// 绕远路。
//
// `pathStatus` 的值
// -----------------
// 与 Unity 的 `NavMeshPathStatus` 同序同义（0/1/2），但**是 int 不是枚举**：
// ZFUNCTION 体系下枚举做不出脚本可见的类型（见 `RigidbodyComponent` 的
// frictionCombine 用 int 的同样处理）。枚举定义在 `ScriptNavigation/ScriptNavMesh.h`
// 的 `ZENUM() NavMeshPathStatus`。

#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/AI/Navigation/NavMesh.h"
#include "Runtime/Function/Framework/Component/Component.h"

#include <EASTL/vector.h>

#include "NavMeshAgentComponent.generated.h"

ZCLASS()
class NavMeshAgentComponent : public Component
{
    ZGENERATED_BODY(NavMeshAgentComponent)

public:
    NavMeshAgentComponent() = default;

    /// 每帧推进：沿路径走、积分速度、转向、判定到点。
    ///
    /// 由 `Level::Tick` 驱动（与所有 Component 一样），`m_Enabled == false` 时
    /// 宿主不会调 —— 所以这里不再重复判 enabled，免得两处口径不一致。
    void Tick(float delta_time) override;

    // ---- 寻路 ------------------------------------------------------------

    /// 设置目标点并立即寻路。
    ///
    /// @return: 是否找到了（哪怕只是到最近可达点的）路径。Unity 同：找不到时
    /// 返回 false，但**目标点仍然记下来了** —— 等网格重建 / 障碍消失后再
    /// `autoRepath` 会自己重试。返回 false 就把目标点丢掉的话，"先设目标再建
    /// 网格"这种常见顺序会永远走不起来。
    ZFUNCTION()
    bool SetDestination(const Vector3& target);

    ZFUNCTION()
    Vector3 GetDestination() const { return m_Destination; }

    /// 有没有设过目标（`SetDestination` 成功或失败都会置位）。
    ZFUNCTION()
    bool HasDestination() const { return m_HasDestination; }

    /// 只算路径、**不**开始移动。用来做"这条路通不通"的预判，或者自己拿航点
    /// （`GetWaypointCount` / `GetWaypoint`）喂给别的系统。
    ZFUNCTION()
    bool CalculatePath(const Vector3& target);

    /// 丢弃当前路径并停住。目标点**保留**（与 Unity 同：ResetPath 只清路径）。
    ZFUNCTION()
    void ResetPath();

    /// 当前路径的航点数（含终点）。没有路径时是 0。
    ZFUNCTION()
    int GetWaypointCount() const { return static_cast<int>(m_Waypoints.size()); }

    /// 第 index 个航点（世界坐标）。越界返回零向量 —— 与引擎其它"越界取元素"
    /// 的口径一致（不抛、不崩），调用方应当先查 `GetWaypointCount()`。
    ZFUNCTION()
    Vector3 GetWaypoint(int index) const;

    // ---- 状态 ------------------------------------------------------------

    /// 当前有没有**有效**路径在走（有航点、且还没走完）。
    ZFUNCTION()
    bool HasPath() const { return m_HasPath; }

    /// 路径状态：0 = PathComplete，1 = PathPartial，2 = PathInvalid。
    /// 与 Unity 的 `NavMeshPathStatus` 同序。
    ZFUNCTION()
    int GetPathStatus() const { return static_cast<int>(m_PathStatus); }

    /// 0 = 完整到达目标；1 = 只能到最近可达点；2 = 完全不通。
    /// 与 `GetPathStatus()` 是同一个值，给一个说人话的名字。
    ZFUNCTION()
    bool IsPathComplete() const { return m_PathStatus == NavPathStatusValue::complete; }

    /// 沿剩余路径到终点的距离。**已经到底**时是 0，没路径时也是 0 —— 想区分
    /// 就看 `HasPath()`。
    ZFUNCTION()
    float GetRemainingDistance() const;

    /// 当前速度（世界单位/秒）。停住时是零向量。
    ZFUNCTION()
    Vector3 GetVelocity() const { return m_Velocity; }

    /// 这一帧算出的下一个位置。Unity 的 `nextPosition`：脚本可以在 Agent 的
    /// Tick 之后、渲染之前读它，用来同步动画根运动之类的东西。
    ZFUNCTION()
    Vector3 GetNextPosition() const { return m_NextPosition; }

    /// 脚下有没有导航网格（`NavigationSystem` 已就绪且当前位置可站立）。
    /// 在网格外的代理不会自己寻路 —— 这是"AI 不动了"最常见的原因，必须能查。
    ZFUNCTION()
    bool IsOnNavMesh() const;

    // ---- 停 / 走 ---------------------------------------------------------

    /// 停住：速度归零、不再推进路径。`destination` 不清 —— `Resume()` 之后
    /// 接着走同一个目标。
    ZFUNCTION()
    void Stop();

    /// 继续走。只清 `isStopped`，不重新寻路。
    ZFUNCTION()
    void Resume();

    ZFUNCTION()
    bool IsStopped() const { return m_IsStopped; }

    ZFUNCTION()
    void SetIsStopped(bool stopped);

    /// 瞬移到指定位置并**丢弃路径**（Unity 的 `Warp`）。
    ///
    /// @return: 目标位置是否可站立 —— 不可站也照样传送过去（调用方可能就是想
    /// 把它救回网格边上），但返回 false 让调用方知道"它现在站不住"。
    ZFUNCTION()
    bool Warp(const Vector3& new_position);

    /// 直接挪一段**相对**位移，不走寻路、不产生路径（Unity 的 `Move`）。
    /// 用来做击退、被推挤这类"不是它自己想走"的位移。
    ZFUNCTION()
    void Move(const Vector3& offset);

    // ---- 参数 ------------------------------------------------------------

    ZFUNCTION()
    float GetSpeed() const { return m_Speed; }
    ZFUNCTION()
    void SetSpeed(float speed) { m_Speed = (speed > 0.f) ? speed : 0.f; }

    ZFUNCTION()
    float GetAngularSpeed() const { return m_AngularSpeed; }
    ZFUNCTION()
    void SetAngularSpeed(float angular_speed) { m_AngularSpeed = (angular_speed > 0.f) ? angular_speed : 0.f; }

    ZFUNCTION()
    float GetAcceleration() const { return m_Acceleration; }
    ZFUNCTION()
    void SetAcceleration(float acceleration) { m_Acceleration = (acceleration > 0.f) ? acceleration : 0.f; }

    ZFUNCTION()
    float GetStoppingDistance() const { return m_StoppingDistance; }
    ZFUNCTION()
    void SetStoppingDistance(float distance) { m_StoppingDistance = (distance > 0.f) ? distance : 0.f; }

    ZFUNCTION()
    float GetRadius() const { return m_Radius; }
    ZFUNCTION()
    void SetRadius(float radius) { m_Radius = (radius > 0.f) ? radius : 0.f; }

    ZFUNCTION()
    float GetHeight() const { return m_Height; }
    ZFUNCTION()
    void SetHeight(float height) { m_Height = (height > 0.f) ? height : 0.f; }

    /// 到点前是否自动减速。关掉的话代理会全速冲过终点再急停（Unity 同）。
    ZFUNCTION()
    bool IsAutoBraking() const { return m_AutoBraking; }
    ZFUNCTION()
    void SetAutoBraking(bool auto_braking) { m_AutoBraking = auto_braking; }

    /// 目标没变但走路失败时，是否每隔 `m_RepathInterval` 秒自动重算一次。
    /// 动态障碍（门开了、同伴让路）必须开；静态关卡可以关掉省查询。
    ZFUNCTION()
    bool IsAutoRepath() const { return m_AutoRepath; }
    ZFUNCTION()
    void SetAutoRepath(bool auto_repath) { m_AutoRepath = auto_repath; }

    /// 是否由本组件写 Transform 的位置。关掉后代理只算速度 / 航向，位置由别的
    /// 系统（角色控制器、根运动）负责 —— 这时用 `GetVelocity()` 取方向。
    ZFUNCTION()
    bool IsUpdatePosition() const { return m_UpdatePosition; }
    ZFUNCTION()
    void SetUpdatePosition(bool update_position) { m_UpdatePosition = update_position; }

    ZFUNCTION()
    bool IsUpdateRotation() const { return m_UpdateRotation; }
    ZFUNCTION()
    void SetUpdateRotation(bool update_rotation) { m_UpdateRotation = update_rotation; }

    // ---- 脚本属性 --------------------------------------------------------
    //
    // 只列**没有 ZPROPERTY 备份**的那些。speed / acceleration / radius 这些已经
    // 由 `ZPROPERTY() float m_Speed` 自动导出成 `speed`（ZHeaderTool 会剥掉
    // `m_` 并把首字母小写），再写一个同名的 ZSCRIPTPROPERTY 是**硬错误**：
    //     ZSCRIPTPROPERTY name 'speed' collides with the script-exposed
    //     ZPROPERTY field of line N
    // 所以这里只补运行时状态（velocity / hasPath / …）—— 它们本来就不该进序列化。

    ZSCRIPTPROPERTY(Name="destination", Getter="GetDestination", Setter="SetDestination")
    ZSCRIPTPROPERTY(Name="velocity", Getter="GetVelocity")
    ZSCRIPTPROPERTY(Name="isStopped", Getter="IsStopped", Setter="SetIsStopped")
    ZSCRIPTPROPERTY(Name="hasPath", Getter="HasPath")
    ZSCRIPTPROPERTY(Name="pathStatus", Getter="GetPathStatus")
    ZSCRIPTPROPERTY(Name="remainingDistance", Getter="GetRemainingDistance")
    ZSCRIPTPROPERTY(Name="nextPosition", Getter="GetNextPosition")
    ZSCRIPTPROPERTY(Name="isOnNavMesh", Getter="IsOnNavMesh")

private:
    /// 与 Unity `NavMeshPathStatus` 同序的枚举（0/1/2）。
    /// 值必须和 `ScriptNavMesh.h` 的 `ZENUM() NavMeshPathStatus` 对齐 —— 两处
    /// 分开定义是因为 ZFUNCTION 不能返回 ZENUM 类型，只能返回 int。
    enum class NavPathStatusValue : unsigned char
    {
        complete = 0,
        partial = 1,
        invalid = 2,
    };

    /// 把 `NavPath::Status` 映射成脚本可见的三态。
    ///
    /// `goal_unwalkable` 归 **invalid** 而不是 partial：它意味着"一个可站立格都
    /// 没有"，那不是"只能走一半"，是"根本不通"。真正的 PathPartial 由
    /// `RequestPath` 在 **success** 分支里另外判（路通、但目标点自己站不住）。
    static NavPathStatusValue ToPathStatusValue(NavPath::Status status)
    {
        switch (status)
        {
            case NavPath::Status::success:
                return NavPathStatusValue::complete;
            case NavPath::Status::goal_unwalkable:
            case NavPath::Status::unreachable:
            case NavPath::Status::start_unwalkable:
            case NavPath::Status::no_navmesh:
            default:
                return NavPathStatusValue::invalid;
        }
    }

    /// 重新向 `NavigationSystem` 要一条路径。`update_status` 为 false 时保留
    /// 旧状态（`autoRepath` 重试失败不该把已经走得好好路径状态打坏）。
    bool RequestPath(const Vector3& target, bool update_status);

    /// 没有宿主 / 没有 Transform 时代理是空转的，所有入口都先过这里。
    ///
    /// const 成员函数里也能拿到非 const 的 Transform（`m_ParentObject` 本身是
    /// const 指针、指向的对象不是），所以只需要一个版本。
    Transform* GetOwnerTransform() const;

    /// 沿剩余航点折线量到终点的长度。没有路径时是 0。
    float ComputeRemainingDistance(const Vector3& from) const;

    /// 朝 `direction` 转，单步不超过 `m_AngularSpeed * delta_time` 度。
    /// 与 `MotorComponent::TurnToward` 同一套叉积 + 轴角做法（见文件头）。
    void TurnTowardDirection(const Vector3& direction, float delta_time, Transform* transform);

    // ---- 已发布参数（序列化 + 脚本可读）------------------------------

    ZPROPERTY()
    float m_Speed {3.5f};
    ZPROPERTY()
    float m_AngularSpeed {120.f};
    ZPROPERTY()
    float m_Acceleration {8.f};
    ZPROPERTY()
    float m_StoppingDistance {0.5f};
    ZPROPERTY()
    float m_Radius {0.5f};
    ZPROPERTY()
    float m_Height {2.f};
    ZPROPERTY()
    bool m_AutoBraking {true};
    ZPROPERTY()
    bool m_AutoRepath {true};
    ZPROPERTY()
    float m_RepathInterval {0.5f};
    ZPROPERTY()
    bool m_UpdatePosition {true};
    ZPROPERTY()
    bool m_UpdateRotation {true};

    // ---- 运行时状态（不序列化）--------------------------------------

    Vector3 m_Destination {Vector3::ZERO};
    bool m_HasDestination {false};

    bool m_IsStopped {false};
    bool m_HasPath {false};

    Vector3 m_Velocity {Vector3::ZERO};
    Vector3 m_NextPosition {Vector3::ZERO};

    eastl::vector<Vector3> m_Waypoints;
    int m_WaypointIndex {0};

    NavPathStatusValue m_PathStatus {NavPathStatusValue::invalid};

    float m_RepathTimer {0.f};
};
