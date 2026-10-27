#pragma once

// ============================================================================
// UMGTree — UWidget 树的共享遍历（Tick 递归 + 焦点收集），header-only
// ----------------------------------------------------------------------------
// 把"按深度优先遍历一棵 widget 树"这件事抽成不依赖任何具体 widget 类型的纯算法，
// 供 UUserWidget::TickTree 和焦点导航共用。核心是模板，只要求节点类型暴露：
//   GetVisibility() / NativeTick(float) / IsFocusable() / GetTabOrder() /
//   GetChildCount() / GetChildAt(int)
// 这样测试可以用最小桩类型（不拖 ZSlate/引擎数学链）headless 单测
// （UMGTreeSmokeTest），而 UUserWidget 用真实 UWidget 实例化（那边已能编 ZSlate）。
//
// 语义：
//   - Tick 递归：先访问自身 NativeTick，再递归子节点。Collapsed 子树整枝跳过
//     （不可见 widget 不 tick，省掉隐藏 HUD 的每帧开销）。
//   - 焦点收集：按深度优先顺序收集所有 IsFocusable() 的 widget；Tab 在其中循环。
//     Collapsed 子树同样跳过（不可见 widget 不可聚焦）。
//   - NextFocusIndex：纯索引算术（wrap/clamp），把"Tab 循环移动焦点"的逻辑独立
//     出来，无需任何 widget 即可测。
//
// EVisibility 只用到"是否 Collapsed"这一比较，故模板不硬依赖 ZSlate 枚举——只要
// 节点的 GetVisibility() 返回值能与节点类型的 Collapsed 值比较即可。为让桩类型
// 无需引入 ZSlate，这里用模板参数 VisCollapsed 传入"Collapsed 哨兵值"。
// ============================================================================

#include <memory>
#include <vector>

namespace ZUMG
{
// Depth-first tick of a widget subtree. Skips Collapsed subtrees entirely.
// VisCollapsed is the sentinel meaning "hidden" (ZSlate::EVisibility::Collapsed
// for real widgets; the test passes its own enum value).
template<typename Node, typename VisCollapsed>
inline void TickWidgetTree(const std::shared_ptr<Node>& root, float delta_time, VisCollapsed collapsed)
{
    if (!root)
        return;
    if (root->GetVisibility() == collapsed)
        return;  // hidden subtree: no tick, no recursion

    root->NativeTick(delta_time);

    const int n = root->GetChildCount();
    for (int i = 0; i < n; ++i)
    {
        TickWidgetTree(root->GetChildAt(i), delta_time, collapsed);
    }
}

// Depth-first collection of focusable widgets (in tree order). Skips Collapsed
// subtrees. Returns the number of widgets appended to `out` by this call
// (including via recursion). Caller may stable-sort by TabOrder afterwards.
template<typename Node, typename VisCollapsed>
inline int CollectFocusable(const std::shared_ptr<Node>& root, std::vector<std::shared_ptr<Node>>& out, VisCollapsed collapsed)
{
    if (!root)
        return 0;
    if (root->GetVisibility() == collapsed)
        return 0;

    int added = 0;
    if (root->IsFocusable())
    {
        out.push_back(root);
        ++added;
    }

    const int n = root->GetChildCount();
    for (int i = 0; i < n; ++i)
    {
        added += CollectFocusable(root->GetChildAt(i), out, collapsed);
    }
    return added;
}

// Advance focus to the next (or previous, with wrap) focusable widget in an
// ordered list, returning the new index. Pure index arithmetic so the wrap/clamp
// logic is testable without any widget. Returns -1 when there is nothing
// focusable (count <= 0).
inline int NextFocusIndex(int current, int count, bool reverse)
{
    if (count <= 0)
        return -1;  // nothing focusable
    if (current < 0)
        return reverse ? count - 1 : 0;  // no current focus -> start at an end
    const int step = reverse ? -1 : 1;
    int next = current + step;
    if (next < 0)
        next = count - 1;  // wrap to end
    else if (next >= count)
        next = 0;  // wrap to start
    return next;
}
}  // namespace ZUMG
