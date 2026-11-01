#include "Runtime/Function/Framework/Component/MouseEventDispatcher.h"

#include "Runtime/BaseClasses/GameObject.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Function/Physics/Physics.h"
#include "Runtime/Function/Physics/PhysicsQuery.h"
#include "Runtime/Scripting/ScriptCamera/ScriptCamera.h"

namespace
{
    // ⚠️ 这两个**必须**是文件级静态，不能是函数内的 `static` 局部变量：那样
    // `Tick` 和 `Reset` 会各自持有一份互不相干的状态，清掉的那份不是派发用的
    // 那份 —— 编译能过、运行不崩，只是"关卡切了之后 OnMouseExit 打到了野指针"。
    GameObject* s_Hovered {nullptr};
    GameObject* s_Pressed {nullptr};

    /// `ScreenPointToRay` 返回的是**按相机复用的**同一个 `Ray`（见 ScriptCamera
    /// 里的注释），所以必须在下一次调用之前把值拷出来。存指针就会在下一次
    /// `PickAt` 里读到别人的射线 —— 而两台相机交替使用时它还是"看起来对的"。
    bool TryPickRay(const ::Vector2& position_pixels, ::Vector3& out_origin,
                    ::Vector3& out_direction, float& out_max_distance)
    {
        ::Camera* const camera = ::Camera::GetMain();
        if (camera == nullptr)
        {
            return false;
        }

        ::Ray* const ray = camera->ScreenPointToRay(position_pixels);
        if (ray == nullptr)
        {
            return false;
        }

        out_origin = ray->GetOrigin();
        out_direction = ray->GetDirection();
        out_max_distance = camera->GetFarClipPlane();
        return true;
    }
}  // namespace

GameObject* MouseEventDispatcher::PickAt(const ::Vector2& position_pixels)
{
    ::Vector3 origin;
    ::Vector3 direction;
    float max_distance = 0.0f;
    if (!TryPickRay(position_pixels, origin, direction, max_distance))
    {
        // 没有相机（专用服务器 / 没跑渲染 / headless）：没有"鼠标下面"这回事。
        return nullptr;
    }

    // -1 = 所有层。这里**不**做层过滤：`OnMouse*` 是"鼠标点到了什么"，不是
    // "某个查询能打到什么"。要过滤的话脚本自己在回调里判 layer 更清楚。
    const RaycastHit hit = ::Physics::Raycast(origin, direction, max_distance, -1);
    if (!hit.valid)
    {
        return nullptr;
    }
    return hit.gameObject;
}

GameObject* MouseEventDispatcher::Tick(const MouseFrameState& state)
{
    return DispatchTo(state, PickAt(state.position_pixels));
}

GameObject* MouseEventDispatcher::DispatchTo(const MouseFrameState& state,
                                             GameObject* under_mouse)
{
    // --- 1. 进出：只在**换对象**那一帧来 ------------------------------------
    //
    // ⚠️ `OnMouseExit` 必须发给**旧**对象、`OnMouseEnter` 发给新对象。写成"都发给
    // 新的"是最容易犯也最难发现的错：鼠标从 A 移到 B 时 A 永远收不到 Exit，
    // 表现是"高亮不消失"，而没有任何报错。
    if (under_mouse != s_Hovered)
    {
        if (s_Hovered != nullptr)
        {
            s_Hovered->BroadcastMouseEvent(MouseEventKind::kExit);
        }
        if (under_mouse != nullptr)
        {
            under_mouse->BroadcastMouseEvent(MouseEventKind::kEnter);
        }
        s_Hovered = under_mouse;
    }

    // --- 2. 悬停：每帧一次 ---------------------------------------------------
    if (s_Hovered != nullptr)
    {
        s_Hovered->BroadcastMouseEvent(MouseEventKind::kOver);
    }

    // --- 3. 按下：边沿，一次 -------------------------------------------------
    if (state.pressed && s_Hovered != nullptr)
    {
        s_Hovered->BroadcastMouseEvent(MouseEventKind::kDown);
        s_Pressed = s_Hovered;
    }

    // --- 4. 拖拽：发给**按下时**那个对象，不是当前悬停的那个 ------------------
    //
    // 这是 Unity 的语义：按住之后把鼠标拖到别处，Drag 仍然回到原来那个对象上
    // （否则"拖动这个物体"的逻辑在鼠标移出边界时会突然断掉）。
    if (state.held && s_Pressed != nullptr)
    {
        s_Pressed->BroadcastMouseEvent(MouseEventKind::kDrag);
    }

    // --- 5. 松手：同样发给按下时那个对象 -------------------------------------
    if (state.released && s_Pressed != nullptr)
    {
        s_Pressed->BroadcastMouseEvent(MouseEventKind::kUp);
        // ⚠️ `OnMouseUpAsButton` 只在"松手时鼠标仍在同一个对象上"时来 —— 这就是
        // 它和 `OnMouseUp` 唯一的差别，也是按钮语义的保证（按下后拖出去再松手
        // 不该触发点击）。
        if (s_Pressed == s_Hovered)
        {
            s_Pressed->BroadcastMouseEvent(MouseEventKind::kUpAsButton);
        }
        s_Pressed = nullptr;
    }

    return s_Hovered;
}

void MouseEventDispatcher::Forget(GameObject* object)
{
    if (object == nullptr)
    {
        return;
    }
    if (s_Hovered == object)
    {
        s_Hovered = nullptr;
    }
    if (s_Pressed == object)
    {
        s_Pressed = nullptr;
    }
}

void MouseEventDispatcher::Reset()
{
    // ⚠️ 这里**不**补发 `OnMouseExit`：调用方（关卡切换 / 世界卸载）要清掉的是
    // 状态，而不是"通知旧对象鼠标走了" —— 那个对象通常已经销毁了。补发就是把
    // 一次 use-after-free 包装成一次"看起来很贴心"的通知。
    s_Hovered = nullptr;
    s_Pressed = nullptr;
}

GameObject* MouseEventDispatcher::GetHovered()
{
    return s_Hovered;
}

GameObject* MouseEventDispatcher::GetPressed()
{
    return s_Pressed;
}
