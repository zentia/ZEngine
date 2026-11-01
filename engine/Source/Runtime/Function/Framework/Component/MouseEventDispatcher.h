#pragma once

#include "Runtime/Core/Math/Vector2.h"
#include "Runtime/Function/Framework/Component/Component.h"

class GameObject;

/// 一帧的鼠标输入。
///
/// ⚠️ 三个 bool 是**独立的**，不是"用一个状态推导另两个"：`held` 与
/// `pressed` / `released` 的边沿语义不同，压成一个 `down` 会丢掉"这一帧刚按下"
/// 和"一直按着"的区别 —— 而 `OnMouseDown` 只该在边沿来一次，`OnMouseDrag` 要
/// 每帧来。
struct MouseFrameState
{
    ::Vector2 position_pixels;
    bool pressed {false};    ///< 本帧刚按下（边沿）
    bool held {false};       ///< 本帧仍按着
    bool released {false};   ///< 本帧刚松开（边沿）
};

/// Unity 的 `OnMouse*` 派发器。
///
/// 为什么单独一个类而不是塞进 `InputSystem`
/// ---------------------------------------
/// 它要把"屏幕上的一个点"变成"场景里的一个对象"，这一步同时需要**相机**和
/// **物理**，而 `InputSystem` 两样都不该知道（它只管键鼠状态）。硬塞进去等于把
/// 输入层和世界层焊死。
///
/// 为什么是静态而不是一个 `IEngineSystem`
/// -------------------------------------
/// 它只有一份进程级状态（"现在悬停的是谁"、"按下的是谁"），和 `TimeState` /
/// `Gizmos.color` 同一类。做成系统要注册、要声明依赖、还要在 headless 测试里
/// 先造一个出来 —— 而这里真正需要被替换的只有"点到了谁"那一步，那一步已经
/// 单独成了 `PickAt()` / `DispatchTo()`。
class MouseEventDispatcher
{
public:
    /// 屏幕坐标 -> 鼠标下面的对象。**没有相机 / 没有物理就返回 nullptr**。
    ///
    /// 只打最近的那一个（Unity 同：`OnMouse*` 只发给最前面那个 collider）。
    static GameObject* PickAt(const ::Vector2& position_pixels);

    /// 派发一帧的七条消息，返回本帧鼠标下面的对象。
    static GameObject* Tick(const MouseFrameState& state);

    /// 只做"派发"这一半：`under_mouse` 由调用方给定。
    ///
    /// ⚠️ 存在的原因就是**可测**。`PickAt` 需要 `RenderSystem`（相机）+ 一台
    /// 物理场景，headless 下两个都没有；如果只有 `Tick`，"七条消息谁在什么
    /// 时候来"这一整块逻辑就一条断言都碰不到 —— 而这恰恰是全部复杂度所在。
    /// `Tick` 是 `DispatchTo(state, PickAt(state.position_pixels))` 的薄壳。
    static GameObject* DispatchTo(const MouseFrameState& state, GameObject* under_mouse);

    /// 对象被销毁时把它从悬停 / 按下状态里摘掉。
    ///
    /// ⚠️ 这不是防御性编程：`s_Hovered` / `s_Pressed` 是**裸指针**，而脚本完全
    /// 可以在 `OnMouseOver` 里 `Destroy` 掉那个对象（"点一下就消失"是最常见的
    /// 一种写法）。不摘的话下一次派发就是一次 use-after-free，而且它只在"鼠标
    /// 恰好还停在那儿"时触发 —— 本地跑不出来，用户机器上偶发崩溃。
    ///
    /// 由 `GameObject::~GameObject` 调用，所以覆盖的是**每一次**销毁，不只是
    /// 关卡卸载。
    static void Forget(GameObject* object);

    /// 清掉悬停与按下状态。关卡切换 / 世界卸载时用。
    static void Reset();

    static GameObject* GetHovered();
    static GameObject* GetPressed();
};
