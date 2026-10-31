#pragma once

#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Resource/ResType/Components/RigidBody.h"
#include "Runtime/Resource/ResType/Data/BasicShape.h"

class PhysicsScene;

enum SweepPass
{
    SWEEP_PASS_UP,
    SWEEP_PASS_SIDE,
    SWEEP_PASS_DOWN,
    SWEEP_PASS_SENSOR
};

class Controller
{
public:
    virtual ~Controller() = default;

    virtual Vector3 move(const Vector3& current_position, const Vector3& displacement) = 0;
};

/// Owns the Capsule it is initialized with AND the RigidBodyShape it builds from it. Both are
/// ObjectManager::NewObject objects (mimalloc, registered, with an instance ID) held only through
/// PPtr, so nothing else frees them; the destructor does, with MemoryManager::DestroyObject (the
/// pairing for NewObject). The caller must therefore pass a capsule it will NOT free itself.
class CharacterController : public Controller
{
public:
    /// Takes OWNERSHIP of `capsule`. Calling it again releases the previous capsule and shape first.
    void Initialize(PPtr<Capsule> capsule);
    ~CharacterController() override;
    CharacterController() = default;
    // Owning a raw heap pair: copying would double-free.
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    Vector3 move(const Vector3& current_position, const Vector3& displacement) override;

    // ---- 可注入场景的那一层 ------------------------------------------------
    //
    // `move()` 自己通过 `WorldManager::GetCurrentActivePhysicsScene()` 找场景。
    // 那是唯一的生产路径，但**不可测**：活动关卡只能由加载流程设置
    // （`WorldManager::m_CurrentActiveLevel` 没有 setter），headless 测试里
    // 永远是空。和 PhysicsScene 那个切片的取舍一样：真逻辑放进一个显式接收
    // `PhysicsScene&` 的方法，`move()` 只负责找场景再转发。
    //
    // 于是测试可以自己造一台场景直接驱动控制器，而组件层的 ZFUNCTION 由接线门
    // 守着。

    /// 真正的一次位移求解。`scene` 为空（或还没 Initialize 出形状）时退化成
    /// "无碰撞直移" —— 和 `move()` 此前的空形状分支一致，不引入新的失败模式。
    Vector3 MoveInScene(PhysicsScene* scene, const Vector3& current_position, const Vector3& displacement);

    /// Unity `CharacterController.Move` 的**全部**求解：逐轴推进 + 碰撞标志。
    ///
    /// 为什么放在这里而不是 `MotorComponent::Move` 里
    /// ---------------------------------------------
    /// `MotorComponent` 的方法要靠 `WorldManager` 找活动场景，而活动关卡只能由
    /// 加载流程设置（没有 setter）—— 于是组件层在 headless 下永远走"没有场景"
    /// 那条路，**一行真实逻辑都测不到**。把求解放进这个显式接收 `PhysicsScene*`
    /// 的方法，测试就能自己造场景直接驱动它；组件层只剩"找 Transform / 找场景 /
    /// 写回位置"这点薄壳，由接线门守着。
    ///
    /// @out_position: 推进之后的落点。被 min_move_distance 丢弃时等于 `from`。
    /// @return:       CollisionFlags 位域。
    int32_t MoveWithFlags(PhysicsScene* scene,
                          const Vector3& from,
                          const Vector3& motion,
                          bool detect_collisions,
                          float min_move_distance,
                          Vector3& out_position);

    /// `SimpleMove` 的位移求解。**纯函数**（除传入参数外无隐藏状态），所以不碰
    /// 任何系统也能测。
    ///
    /// @inout_fall_speed: 累积的下落速度。落地时归零 —— 不归零的话它会一直负
    ///                    下去，落地那一帧的位移就是一个巨大的穿地量。
    static Vector3 ComputeSimpleMoveMotion(const Vector3& speed,
                                           float gravity,
                                           float delta_time,
                                           bool grounded,
                                           float& inout_fall_speed);

    /// 着地探测：从胶囊中心向下打一条射线，长度 = 半高 + 半径 + skin。
    ///
    /// 为什么是射线而不是"没在跳"
    /// --------------------------
    /// `MotorComponent` 的跳跃状态机（`JumpState::idle`）**不是**着地判据：它是
    /// 玩家按下跳跃键才离开 idle 的，一个从平台走下去的角色在空中仍然是 idle。
    /// 而 `CalculateTargetPosition` 里那个 `final_position.z <= 0` 的 z-hack 把
    /// "地面"写死成了 z=0 平面 —— 有二楼的场景里它是错的。
    ///
    /// 射线只打**静态几何 + 刚体**，角色自身没有 body（控制器用的是 IsOverlap，
    /// 不是场景里的刚体），所以不会打到自己。
    ///
    /// @skin_width: 允许脚底离地面这么远仍算"贴地"。没有它，一步位移后脚底刚好
    ///              离开地面一毫米就会读到 false，表现是"跳跃输入随机失灵"。
    bool ProbeGrounded(PhysicsScene* scene, const Vector3& current_position, float skin_width) const;

    // ---- 胶囊尺寸（Unity：CharacterController.height / radius / center）----
    //
    // 三个量不是独立的：Unity 的 `height` 是**含两个半球帽**的总高，
    // 而 Jolt / 本引擎的 `Capsule` 存的是圆柱段的半高 `m_HalfHeight`：
    //
    //     height = 2 * (half_height + radius)
    //
    // 所以 SetHeight 必须**保持半径**去反解半高，直接把 height 当半高写会让
    // 角色凭空长高一整个半径（而且撞到天花板的时机全错）。

    /// 含帽总高。胶囊没就绪时返回 0。
    float GetCapsuleHeight() const;
    /// 保持半径，反解半高。`height` 小于 2*radius 时夹到 2*radius（退化成球）。
    void SetCapsuleHeight(float height);

    float GetCapsuleRadius() const;
    /// 半径不能小于等于 0：Jolt 的 CapsuleShape 用一个非正的 convex radius 会
    /// 直接断言。夹到 1e-3。
    void SetCapsuleRadius(float radius);

    float GetCapsuleHalfHeight() const;
    void SetCapsuleHalfHeight(float half_height);

    /// Unity 的 `center`：胶囊中心相对 GameObject 原点的偏移。
    /// 本引擎把原点放在**脚底**，所以 center 只有 Z 分量，且恒等于
    /// `half_height + radius`（这正是 Initialize 里写的形状局部变换）。
    Vector3 GetCapsuleCenter() const;
    /// 保持半径去反解半高，使中心落在 `center.z`。小于半径时夹住。
    void SetCapsuleCenter(const Vector3& center);

    bool HasShape() const { return m_RigidbodyShape != nullptr; }

private:
    /// Destroy the shape, and the capsule unless it is `keep_capsule`.
    void ReleaseOwnedObjects(const Capsule* keep_capsule);

    /// 尺寸改过之后必须重写形状的局部变换：`Initialize` 把胶囊抬高了
    /// `half_height + radius`（原点在脚底），半径或半高一变，这个抬高量就过期了。
    /// 忘掉这一步的表现是"改了 height 但碰撞体还停在原来的高度"—— 一个不报错
    /// 的错位。
    void RefreshShapeTransform();

    PPtr<Capsule> m_Capsule;
    PPtr<RigidBodyShape> m_RigidbodyShape;
};