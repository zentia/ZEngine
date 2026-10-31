#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/AI/Navigation/NavigationSystem.h"

#include <EASTL/vector.h>

#include <cmath>

#include "ScriptNavMesh.generated.h"

// 脚本侧的导航查询（Unity 的 `UnityEngine.AI.NavMesh` 静态类 + `NavMeshPath` +
// `NavMeshHit`）。逐点行走的那个组件在
// `Runtime/Function/AI/Navigation/NavMeshAgentComponent.h`。
//
// 为什么静态类叫 `Navigation` 而不是 `NavMesh`
// ---------------------------------------------
// `NavMesh` 这个名字已经被引擎内部的网格类占了
// （`Runtime/Function/AI/Navigation/NavMesh.h`），同一命名空间下再声明一个同名
// class 是编译错误，不是二义性。这里沿用 `VectorMath`（Unity 的 `Vector3`）那
// 一套取舍：**名字让位给引擎，偏差写进文档**，而不是去改一个被引用得更多的
// 内部类名。
//
// `hit` 在 `SamplePosition` 和 `Raycast` 里意思相反
// -------------------------------------------------
// 这是 Unity 自己的不一致，照搬而不是"修正"：
//   * `SamplePosition`：`hit == true` 表示**找到了**可站立点；
//   * `Raycast`：`hit == true` 表示**被挡住了**（`position` 是挡住的那个点）。
// 反过来"统一"会让从 Unity 搬过来的分支判断在两个方法里各错一次。
//
// header-only（同 ScriptScreen.h / ScriptMath.h）—— 只是一层转发，没有状态。

/// 路径状态，与 Unity 的 `NavMeshPathStatus` 同序同义。
///
/// ZFUNCTION 返回不了枚举类型（见 `RigidbodyComponent` 的 frictionCombine 用
/// int 的同样处理），所以这个 ZENUM 只是**给脚本一个可读的名字表**，实际接口
/// 上流通的是 int。
ZENUM()
enum class NavMeshPathStatus : unsigned char
{
    PathComplete,
    PathPartial,
    PathInvalid,
};

// ---- 一条算好的路径 -------------------------------------------------------

ZCLASS()
class NavMeshPath : public Object
{
    ZGENERATED_BODY(NavMeshPath)

public:
    NavMeshPath() = default;

    /// 拐点数（含起点与终点）。空路径是 0。
    ZFUNCTION()
    int GetCornerCount() const { return static_cast<int>(m_Corners.size()); }

    /// 第 index 个拐点。越界返回零向量 —— 调用方应先查 `GetCornerCount()`。
    ZFUNCTION()
    Vector3 GetCorner(int index) const
    {
        if (index < 0 || index >= static_cast<int>(m_Corners.size()))
        {
            return Vector3::ZERO;
        }
        return m_Corners[index];
    }

    /// 0 = PathComplete，1 = PathPartial，2 = PathInvalid（见 ZENUM）。
    ZFUNCTION()
    int GetStatus() const { return m_Status; }

    /// 清空拐点并把状态置为 PathInvalid。复用一个 path 对象做多次查询时用它，
    /// 免得查失败还留着上一次的旧拐点（读出来像是成功）。
    ZFUNCTION()
    void ClearCorners()
    {
        m_Corners.clear();
        m_Status = static_cast<int>(NavMeshPathStatus::PathInvalid);
    }

    /// 由 `Navigation.CalculatePath` 填装。**刻意不导出**：`eastl::vector<Vector3>`
    /// 没有 Converter，导出了脚本也调不动。
    void SetCorners(const eastl::vector<Vector3>& corners)
    {
        m_Corners = corners;
        m_Status = static_cast<int>(NavMeshPathStatus::PathComplete);
    }

    void SetStatus(int status) { m_Status = status; }

private:
    eastl::vector<Vector3> m_Corners;
    int m_Status {static_cast<int>(NavMeshPathStatus::PathInvalid)};
};

// ---- 一次命中查询的结果 ---------------------------------------------------

ZCLASS()
class NavMeshHit : public Object
{
    ZGENERATED_BODY(NavMeshHit)

public:
    NavMeshHit() = default;

    /// 是否命中。**语义随来源方法而变**，见文件头。
    ZFUNCTION()
    bool IsHit() const { return m_Hit; }

    ZFUNCTION()
    Vector3 GetPosition() const { return m_Position; }

    /// `SamplePosition`：源点到命中点的距离；`Raycast`：沿射线走了多远才被挡。
    ZFUNCTION()
    float GetDistance() const { return m_Distance; }

    void SetHit(bool hit) { m_Hit = hit; }
    void SetPosition(const Vector3& position) { m_Position = position; }
    void SetDistance(float distance) { m_Distance = distance; }

private:
    bool m_Hit {false};
    Vector3 m_Position {Vector3::ZERO};
    float m_Distance {0.f};
};

// ---- 静态查询入口 ---------------------------------------------------------

ZCLASS()
class Navigation : public Object
{
    ZGENERATED_BODY(Navigation)

public:
    /// 导航网格是否已就绪。没就绪时所有查询都返回"没找到" —— "AI 不动了"第一
    /// 件事就该查这个。
    ZFUNCTION()
    static bool IsReady()
    {
        NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
        return (navigation_system != nullptr) && navigation_system->IsReady();
    }

    ZFUNCTION()
    static int GetWalkableCellCount()
    {
        NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
        return (navigation_system == nullptr) ? 0 : navigation_system->GetWalkableCellCount();
    }

    /// 网格的格子边长（世界单位）。判断"终点被吸附了多远"时要拿它当尺子。
    ZFUNCTION()
    static float GetCellSize()
    {
        NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
        if (navigation_system == nullptr)
        {
            return 0.f;
        }
        return navigation_system->GetNavMesh().GetSettings().cell_size;
    }

    /// 算一条从 source 到 target 的路径。
    ///
    /// 返回一个新对象而不是填调用方传进来的那个 —— TS 没有 out/ref 参数（同
    /// `RaycastHit` 为什么自带 `valid` 字段）。查询失败时返回一个
    /// `status == PathInvalid`、`cornerCount == 0` 的对象，**不是 null**；只有
    /// 连 ObjectManager / NavigationSystem 都拿不到时才 null。
    ZFUNCTION()
    static NavMeshPath* CalculatePath(const Vector3& source, const Vector3& target)
    {
        ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
        NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
        if (object_manager == nullptr || navigation_system == nullptr)
        {
            return nullptr;
        }

        NavMeshPath* path = object_manager->NewObject<NavMeshPath>();
        const NavPath raw = navigation_system->FindPath(source, target, /*smooth=*/true);

        int status = static_cast<int>(NavMeshPathStatus::PathInvalid);
        if (raw.IsValid())
        {
            path->SetCorners(raw.waypoints);
            // Unity 的 PathPartial：路是通的，但到不了你给的**那个点**（目标被
            // 吸附到了最近的可达格）。
            //
            // 判据是"目标点自己站不站得住"而不是"终点离目标多远"：`FindPath`
            // 会把最后一个航点直接写成 `goal`，所以那个距离恒为 0。
            //
            // 也不能只看格子的 walkable 位：墙顶那一格同样 walkable（NavMesh
            // 取的是射线打到的第一个面，墙顶成了个孤岛），但它离地面 3 米。
            status = navigation_system->IsStandable(target)
                         ? static_cast<int>(NavMeshPathStatus::PathComplete)
                         : static_cast<int>(NavMeshPathStatus::PathPartial);
        }
        else if (raw.status == NavPath::Status::goal_unwalkable)
        {
            // 一个可站立格都没有：这是"完全不通"，不是"只能走一半"。
            status = static_cast<int>(NavMeshPathStatus::PathInvalid);
        }
        path->SetStatus(status);
        return path;
    }

    /// 在 source 附近 max_distance 以内找一个可站立点（Unity 的
    /// `NavMesh.SamplePosition`）。
    ///
    /// ⚠️ 与 Unity 的差别：Unity 是"由内向外"在 maxDistance 半径内搜索，这里先
    /// 取全局最近点、再判它是否落在半径内。半径内有更近的点但全局最近点在半径
    /// 外的情况在网格上不存在（最近就是最近），所以行为一致。
    ZFUNCTION()
    static NavMeshHit* SamplePosition(const Vector3& source, float max_distance)
    {
        NavMeshHit* hit = NewHit();
        if (hit == nullptr)
        {
            return nullptr;
        }

        NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
        if (navigation_system == nullptr || !navigation_system->IsReady())
        {
            return hit;
        }

        Vector3 nearest {Vector3::ZERO};
        if (!navigation_system->FindNearestWalkablePoint(source, nearest))
        {
            return hit;  // 一个可站立格都没有：hit 保持 false
        }

        const float distance = (nearest - source).length();
        if (max_distance > 0.f && distance > max_distance)
        {
            return hit;  // 最近点也在半径外
        }

        hit->SetHit(true);
        hit->SetPosition(nearest);
        hit->SetDistance(distance);
        return hit;
    }

    /// 沿 source→target 走一遍，看导航网格上通不通（Unity 的 `NavMesh.Raycast`）。
    ///
    /// `hit == true` 表示**被挡住**，`position` 是挡住的那个点、`distance` 是沿
    /// 射线走过的距离。全程可走时 `hit == false`。
    ///
    /// 步长取半个格子：整格步长会正好跨过一格宽的墙而不命中。
    ZFUNCTION()
    static NavMeshHit* Raycast(const Vector3& source, const Vector3& target)
    {
        NavMeshHit* hit = NewHit();
        if (hit == nullptr)
        {
            return nullptr;
        }

        NavigationSystem* navigation_system = GET_SYSTEM(NavigationSystem);
        if (navigation_system == nullptr || !navigation_system->IsReady())
        {
            return hit;
        }

        const Vector3 delta = target - source;
        const float total = delta.length();
        if (total <= 1e-5f)
        {
            return hit;
        }

        const float step = Math::max(navigation_system->GetNavMesh().GetSettings().cell_size * 0.5f, 0.01f);
        const Vector3 direction = delta * (1.f / total);

        for (float travelled = 0.f; travelled <= total; travelled += step)
        {
            const Vector3 sample = source + direction * travelled;
            if (!navigation_system->IsStandable(sample))
            {
                hit->SetHit(true);
                hit->SetPosition(sample);
                hit->SetDistance(travelled);
                return hit;
            }
        }
        return hit;
    }

private:
    static NavMeshHit* NewHit()
    {
        ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
        if (object_manager == nullptr)
        {
            return nullptr;
        }
        return object_manager->NewObject<NavMeshHit>();
    }
};
