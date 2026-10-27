// ============================================================================
// UMGTreeSmokeTest — UWidget 树遍历（UMGTree.h）的无头验证
// ----------------------------------------------------------------------------
// 验证 ZUMG 命名空间里的树遍历纯算法：
//   - TickWidgetTree：深度优先 NativeTick；Collapsed 子树整枝跳过（含后代）。
//   - CollectFocusable：按树序收集 IsFocusable；Collapsed 跳过。
//   - NextFocusIndex：Tab 循环的纯索引算术（前进/后退/wrap/空列表）。
//
// UMGTree.h 是模板，只要求节点暴露 GetVisibility/NativeTick/IsFocusable/
// GetTabOrder/GetChildCount/GetChildAt。这里用最小桩类型（自带一个 Visibility
// 枚举，不引入 ZSlate/引擎数学链）驱动，纯 CPU，只链 include root。真实 UWidget
// 的实例化由 UUserWidget::TickTree 编译期保证（那边能编 ZSlate）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_UMG_TREE_SMOKE_TEST=ON
//   cmake --build build --target UMGTreeSmokeTest
// ============================================================================

#include "Runtime/UMG/Core/UMGTree.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        if (cond)
        {
            std::printf("  [ ok ] %s\n", msg);
        }
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++g_Failures;
        }
    }

    // Standalone visibility enum so the test needs no ZSlate/engine headers.
    enum class Vis { Visible, Collapsed };

    // Minimal node: counts ticks, configurable visibility/focusable/tab-order,
    // holds children (so it doubles as both leaf and panel).
    struct TestNode
    {
        int Ticks {0};
        Vis Vis_ {Vis::Visible};
        bool Focusable_ {false};
        int TabOrder_ {0};
        std::vector<std::shared_ptr<TestNode>> Children;

        int GetVisibility() const { return static_cast<int>(Vis_); }
        void NativeTick(float) { ++Ticks; }
        bool IsFocusable() const { return Focusable_; }
        int GetTabOrder() const { return TabOrder_; }
        int GetChildCount() const { return static_cast<int>(Children.size()); }
        std::shared_ptr<TestNode> GetChildAt(int i) const
        {
            return (i >= 0 && i < static_cast<int>(Children.size())) ? Children[static_cast<size_t>(i)] : nullptr;
        }
    };

    const int kCollapsed = static_cast<int>(Vis::Collapsed);
}  // namespace

int main()
{
    std::printf("UMGTreeSmokeTest (widget-tree traversal)\n");
    using ZUMG::CollectFocusable;
    using ZUMG::NextFocusIndex;
    using ZUMG::TickWidgetTree;

    // --- TickWidgetTree: 深度优先，全部 tick ---------------------------------
    {
        auto panel = std::make_shared<TestNode>();
        auto a = std::make_shared<TestNode>();
        auto b = std::make_shared<TestNode>();
        panel->Children = {a, b};
        TickWidgetTree(panel, 0.016f, kCollapsed);
        Check(a->Ticks == 1 && b->Ticks == 1, "TickWidgetTree: every visible widget ticked once");
    }

    // --- TickWidgetTree: Collapsed 子树整枝跳过（含后代）---------------------
    {
        auto panel = std::make_shared<TestNode>();
        auto visible = std::make_shared<TestNode>();
        auto hiddenPanel = std::make_shared<TestNode>();
        auto hiddenChild = std::make_shared<TestNode>();
        hiddenPanel->Children = {hiddenChild};
        hiddenPanel->Vis_ = Vis::Collapsed;
        panel->Children = {visible, hiddenPanel};
        TickWidgetTree(panel, 0.016f, kCollapsed);
        Check(visible->Ticks == 1, "TickWidgetTree: visible widget ticked");
        Check(hiddenChild->Ticks == 0, "TickWidgetTree: Collapsed subtree (and its children) NOT ticked");
    }

    // --- CollectFocusable: 只收集 IsFocusable，按树序 ------------------------
    {
        auto panel = std::make_shared<TestNode>();
        auto plain = std::make_shared<TestNode>();   // not focusable
        auto btn1 = std::make_shared<TestNode>();
        btn1->Focusable_ = true;
        auto btn2 = std::make_shared<TestNode>();
        btn2->Focusable_ = true;
        panel->Children = {plain, btn1, btn2};
        std::vector<std::shared_ptr<TestNode>> out;
        const int n = CollectFocusable(panel, out, kCollapsed);
        Check(n == 2, "CollectFocusable: only focusable widgets collected");
        Check(out[0] == btn1 && out[1] == btn2, "CollectFocusable: depth-first tree order preserved");
    }

    // --- CollectFocusable: Collapsed 子树里的 focusable 不收 -----------------
    {
        auto panel = std::make_shared<TestNode>();
        auto hiddenBtn = std::make_shared<TestNode>();
        hiddenBtn->Focusable_ = true;
        hiddenBtn->Vis_ = Vis::Collapsed;
        panel->Children = {hiddenBtn};
        std::vector<std::shared_ptr<TestNode>> out;
        Check(CollectFocusable(panel, out, kCollapsed) == 0, "CollectFocusable: Collapsed focusable not collected");
    }

    // --- CollectFocusable + stable_sort by TabOrder --------------------------
    {
        auto panel = std::make_shared<TestNode>();
        auto late = std::make_shared<TestNode>();
        late->Focusable_ = true;
        late->TabOrder_ = 5;
        auto early = std::make_shared<TestNode>();
        early->Focusable_ = true;
        early->TabOrder_ = 1;
        // Added late-first but tab-order makes early come first.
        panel->Children = {late, early};
        std::vector<std::shared_ptr<TestNode>> out;
        CollectFocusable(panel, out, kCollapsed);
        std::stable_sort(out.begin(), out.end(), [](const std::shared_ptr<TestNode>& a, const std::shared_ptr<TestNode>& b) {
            return a->GetTabOrder() < b->GetTabOrder();
        });
        Check(out[0] == early && out[1] == late, "stable_sort by TabOrder reorders while keeping equal-order tree sequence");
    }

    // --- NextFocusIndex: 前进 + wrap -----------------------------------------
    Check(NextFocusIndex(-1, 3, false) == 0, "NextFocusIndex: no current -> start at 0");
    Check(NextFocusIndex(0, 3, false) == 1, "NextFocusIndex: forward step");
    Check(NextFocusIndex(2, 3, false) == 0, "NextFocusIndex: wraps from last to first");
    Check(NextFocusIndex(1, 3, false) == 2, "NextFocusIndex: middle forward");

    // --- NextFocusIndex: 后退 + wrap -----------------------------------------
    Check(NextFocusIndex(-1, 3, true) == 2, "NextFocusIndex: no current + reverse -> start at last");
    Check(NextFocusIndex(2, 3, true) == 1, "NextFocusIndex: reverse step");
    Check(NextFocusIndex(0, 3, true) == 2, "NextFocusIndex: wraps from first to last");

    // --- NextFocusIndex: 空列表 / 单元素 -------------------------------------
    Check(NextFocusIndex(-1, 0, false) == -1, "NextFocusIndex: empty list -> -1 (nothing focusable)");
    Check(NextFocusIndex(0, 0, false) == -1, "NextFocusIndex: zero count -> -1");
    Check(NextFocusIndex(0, 1, false) == 0, "NextFocusIndex: single element stays put (forward wrap)");
    Check(NextFocusIndex(0, 1, true) == 0, "NextFocusIndex: single element stays put (reverse wrap)");

    // --- 端到端：Tab 循环走完所有可聚焦 widget 后回到起点 --------------------
    {
        auto panel = std::make_shared<TestNode>();
        for (int i = 0; i < 3; ++i)
        {
            auto b = std::make_shared<TestNode>();
            b->Focusable_ = true;
            panel->Children.push_back(b);
        }
        std::vector<std::shared_ptr<TestNode>> out;
        CollectFocusable(panel, out, kCollapsed);
        int idx = -1;
        bool visited_first_again = false;
        const int first = 0;
        for (int step = 0; step < 4; ++step)
        {
            idx = NextFocusIndex(idx, static_cast<int>(out.size()), false);
            if (step == 3 && idx == first)
                visited_first_again = true;
        }
        Check(visited_first_again, "end-to-end: Tab wraps back to the first widget after a full cycle");
    }

    if (g_Failures == 0)
    {
        std::printf("UMGTreeSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("UMGTreeSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
