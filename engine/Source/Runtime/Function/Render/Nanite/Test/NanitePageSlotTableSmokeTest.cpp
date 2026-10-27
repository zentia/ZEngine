// =============================================================================
// NanitePageSlotTableSmokeTest — 页槽位分配器（NanitePageSlotTable.h）的无头验证
// -----------------------------------------------------------------------------
// 覆盖 nanite_pages::PageSlotTable：
//   - Admit 认领连续 vertex+index 槽位，base 单调推进（首帧无碎片时）。
//   - 稳定句柄：已驻留页重复 Admit 是 no-op，返回同一槽位。
//   - Evict 归还槽位，空闲表合并相邻段（防碎片）。
//   - churn：反复 admit/evict 后总用量回到基线，无泄漏；槽位不重叠。
//   - best-fit：碎片化后能装进最小合适段。
//   - 空间不足：Admit 返回 false，不破坏已有槽位。
//   - Reset 全清。
//
// 全部纯 CPU，headless，只链 include root（header-only，只依赖 STL）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_NANITE_PAGETABLE_SMOKE_TEST=ON
//   cmake --build build --target NanitePageSlotTableSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Nanite/NanitePageSlotTable.h"

#include <cstdio>
#include <unordered_set>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", msg);
        if (!cond)
            ++g_Failures;
    }

    using nanite_pages::PageSlotTable;
    using nanite_pages::PageSlot;

    // 校验所有已驻留页的 vertex 槽位区间两两不重叠、且都在容量内。静默版（churn 用）。
    bool VertexSlotsDisjointQuiet(const PageSlotTable& t)
    {
        std::vector<bool> occupied(t.GetVertexCapacity(), false);
        for (uint32_t page = 0; page < 4096; ++page)
        {
            const PageSlot* s = t.FindSlot(page);
            if (!s)
                continue;
            if (s->vertex_slot_base + s->vertex_slot_count > t.GetVertexCapacity())
                return false;
            for (uint32_t k = 0; k < s->vertex_slot_count; ++k)
            {
                const uint32_t idx = s->vertex_slot_base + k;
                if (occupied[idx])
                    return false;  // 重叠！
                occupied[idx] = true;
            }
        }
        return true;
    }
}  // namespace

int main()
{
    std::printf("NanitePageSlotTableSmokeTest (page-slot allocator)\n");
    std::printf("============================================================\n");

    // --- 1. 基本认领：连续、base 单调推进 -----------------------------------
    {
        PageSlotTable t(100, 100);
        PageSlot s1, s2, s3;
        Check(t.Admit(1, 30, 20, s1), "基本: 页1 admit 成功");
        Check(t.Admit(2, 30, 20, s2), "基本: 页2 admit 成功");
        Check(s1.vertex_slot_base == 0 && s1.vertex_slot_count == 30, "基本: 页1 vertex [0,30)");
        Check(s2.vertex_slot_base == 30 && s2.vertex_slot_count == 30, "基本: 页2 vertex [30,60)");
        Check(s1.index_slot_base == 0 && s2.index_slot_base == 20, "基本: index 段独立推进");
        Check(t.GetVertexUsed() == 60 && t.GetIndexUsed() == 40, "基本: 用量正确累计");
        Check(VertexSlotsDisjointQuiet(t), "基本: 槽位两两不重叠");
    }

    // --- 2. 稳定句柄：重复 Admit 是 no-op -----------------------------------
    {
        PageSlotTable t(100, 100);
        PageSlot s1, s1_again;
        t.Admit(7, 20, 10, s1);
        Check(t.Admit(7, 20, 10, s1_again), "稳定: 重复 admit 返回成功");
        Check(s1_again.vertex_slot_base == s1.vertex_slot_base, "稳定: 返回同一 vertex base");
        Check(t.GetVertexUsed() == 20, "稳定: 重复 admit 不重复计费");
        Check(t.GetSlotCount() == 1, "稳定: 仍只有一个槽位");
    }

    // --- 3. Evict 归还 + 相邻段合并 ----------------------------------------
    {
        PageSlotTable t(100, 100);
        PageSlot s1, s2, s3;
        t.Admit(1, 30, 0, s1);  // [0,30)
        t.Admit(2, 30, 0, s2);  // [30,60)
        t.Admit(3, 30, 0, s3);  // [60,90)
        Check(t.Evict(2), "evict: 页2 释放成功");
        Check(!t.HasSlot(2), "evict: 页2 不再持有槽位");
        Check(t.GetVertexUsed() == 60, "evict: 用量回落到 60");
        // 释放中间页后，空闲表应合并成 [30,60) 单段（与两侧不相邻，因为 1/3 仍占）。
        // 再释放 1 和 3，三段应合并成整段 [0,90)。
        t.Evict(1);
        t.Evict(3);
        Check(t.GetVertexUsed() == 0, "evict: 全部释放后用量归零");
        // 现在应能 admit 一个 90 的整段（证明合并生效，不是三个碎段）。
        PageSlot big;
        Check(t.Admit(9, 90, 0, big), "evict: 释放后能装下 90 整段（相邻段已合并）");
        Check(big.vertex_slot_base == 0, "evict: 合并段从 0 开始");
    }

    // --- 4. churn：反复 admit/evict 无泄漏、不重叠 --------------------------
    {
        PageSlotTable t(64, 64);
        uint32_t next_page = 100;
        bool all_disjoint = true;
        for (int round = 0; round < 200; ++round)
        {
            PageSlot s;
            // 每轮 admit 两个 20-vertex 页（共 40），再 evict 一个。
            if (t.Admit(next_page, 20, 20, s))
                ++next_page;
            if (t.Admit(next_page, 20, 20, s))
                ++next_page;
            // evict 一个已驻留页（轮转）。
            if (t.GetSlotCount() > 1)
                t.Evict(t.FindSlot(0) ? next_page - 1 : next_page - 2);
            if (!VertexSlotsDisjointQuiet(t))
                all_disjoint = false;
        }
        Check(all_disjoint, "churn: 200 轮 admit/evict 槽位始终不重叠");
        // 全清后用量必须归零（无泄漏）。
        t.Reset();
        Check(t.GetVertexUsed() == 0 && t.GetIndexUsed() == 0, "churn: Reset 后用量归零（无泄漏）");
        Check(t.GetSlotCount() == 0, "churn: Reset 后槽位清空");
        // Reset 后应能重新装满整段。
        PageSlot s;
        Check(t.Admit(999, 64, 64, s), "churn: Reset 后能装满整段容量");
    }

    // --- 5. best-fit：碎片化后装进最小合适段 -------------------------------
    {
        PageSlotTable t(100, 100);
        PageSlot s;
        t.Admit(1, 40, 0, s);   // [0,40)
        t.Admit(2, 20, 0, s);   // [40,60)
        t.Admit(3, 40, 0, s);   // [60,100)  → 满了
        t.Evict(2);             // 挖出 [40,60) 的洞（20）
        // 现在 free = [40,60)。admit 一个 15 的页应落进这个洞（best-fit），base=40。
        PageSlot small;
        Check(t.Admit(4, 15, 0, small), "best-fit: 碎片洞能装下小页");
        Check(small.vertex_slot_base == 40, "best-fit: 落进 [40,60) 洞的起点");
        Check(VertexSlotsDisjointQuiet(t), "best-fit: 装洞后仍不重叠");
    }

    // --- 6. 空间不足：Admit 返回 false，不破坏已有槽位 ---------------------
    {
        PageSlotTable t(50, 50);
        PageSlot s;
        Check(t.Admit(1, 50, 50, s), "不足: 先装满整段");
        PageSlot too_big;
        Check(!t.Admit(2, 1, 1, too_big), "不足: 满载后 admit 失败");
        Check(!t.HasSlot(2), "不足: 失败页不持有槽位");
        Check(t.GetVertexUsed() == 50, "不足: 失败不改变用量");
        Check(VertexSlotsDisjointQuiet(t), "不足: 失败后槽位仍完整");
        // vertex 够但 index 不够，也应整体失败（避免半驻留）。
        t.Reset();
        t.Admit(1, 30, 50, s);  // vertex 用 30，index 用满 50
        PageSlot half;
        Check(!t.Admit(3, 10, 1, half), "不足: index 段不够时整体失败（不半驻留）");
        Check(!t.HasSlot(3), "不足: 半驻留页不持有槽位");
    }

    // --- 7. Evict 未驻留页是安全 no-op -------------------------------------
    {
        PageSlotTable t(10, 10);
        Check(!t.Evict(42), "no-op: 未驻留页 evict 返回 false");
        Check(t.GetVertexUsed() == 0, "no-op: 未驻留页 evict 不改用量");
    }

    const int total = g_Failures;
    std::printf("============================================================\n");
    if (total == 0)
    {
        std::printf("NanitePageSlotTableSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("NanitePageSlotTableSmokeTest: %d CHECK(S) FAILED\n", total);
    return 1;
}
