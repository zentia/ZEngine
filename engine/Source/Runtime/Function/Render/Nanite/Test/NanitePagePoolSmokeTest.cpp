// =============================================================================
// NanitePagePoolSmokeTest — 页池簿记层（NanitePagePool.h）的无头验证
// -----------------------------------------------------------------------------
// 覆盖 nanite_pages::PagePool：
//   - 预算闸：预算装不下的远页被拒（近页优先）。
//   - 距离优先 + LRU 破平：距离相同时优先留最近使用的页。
//   - 结构页(internal)永不驱逐、永远驻留。
//   - 增量换入/换出队列：admitted/evicted 只含本帧变化的页。
//   - LRU 驱逐：预算超限时最久未用的叶子页先被驱逐。
//   - RemovePage / Touch / SetBudget 边界。
//
// 全部纯 CPU，headless，只链 include root（NanitePagePool.h header-only，只依赖 STL）。
//
// 构建（任意主机，纯 CPU，headless）：
//   cmake -B build -DZENGINE_BUILD_NANITE_PAGEPOOL_SMOKE_TEST=ON
//   cmake --build build --target NanitePagePoolSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Nanite/NanitePagePool.h"

#include <cstdio>

namespace
{
    int g_Failures = 0;

    void Check(bool cond, const char* msg)
    {
        std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", msg);
        if (!cond)
            ++g_Failures;
    }

    using nanite_pages::PagePool;
    using nanite_pages::PageRecord;

    PageRecord MakeLeaf(uint32_t id, uint32_t bytes, float dist)
    {
        PageRecord r;
        r.page_id = id;
        r.mesh_id = 1;
        r.byte_size = bytes;
        r.is_leaf = true;
        r.distance = dist;
        return r;
    }
    PageRecord MakeInternal(uint32_t id, uint32_t bytes)
    {
        PageRecord r;
        r.page_id = id;
        r.mesh_id = 1;
        r.byte_size = bytes;
        r.is_leaf = false;
        return r;
    }
}  // namespace

int main()
{
    std::printf("NanitePagePoolSmokeTest (page-pool bookkeeping)\n");
    std::printf("============================================================\n");

    // --- 1. 预算闸：近页优先，远页被拒 --------------------------------------
    {
        PagePool pool(100);
        pool.UpsertPage(MakeLeaf(1, 40, 10.0f));  // 近
        pool.UpsertPage(MakeLeaf(2, 40, 20.0f));  // 中
        pool.UpsertPage(MakeLeaf(3, 40, 30.0f));  // 远（装不下：40*3=120 > 100）

        std::vector<uint32_t> want = {1, 2, 3};
        nanite_pages::PoolUpdate u = pool.Update(want, /*now=*/1.0);

        Check(pool.IsResident(1) && pool.IsResident(2), "预算闸: 两个近页驻留");
        Check(!pool.IsResident(3), "预算闸: 装不下的远页被拒");
        Check(u.admitted.size() == 2, "预算闸: admitted 只含两个新进池页");
        Check(pool.GetResidentBytes() == 80, "预算闸: 驻留字节 = 80");
    }

    // --- 2. 距离优先：预算紧张时近页胜出 ------------------------------------
    {
        PagePool pool(80);
        pool.UpsertPage(MakeLeaf(1, 50, 5.0f));   // 最近
        pool.UpsertPage(MakeLeaf(2, 50, 50.0f));  // 远（装不下：50+50=100 > 80）
        pool.UpsertPage(MakeLeaf(3, 20, 60.0f));  // 更远但小（跳过 2 后能装下）

        std::vector<uint32_t> want = {1, 2, 3};
        nanite_pages::PoolUpdate u = pool.Update(want, 1.0);

        Check(pool.IsResident(1), "距离优先: 最近页驻留");
        Check(!pool.IsResident(2), "距离优先: 大而远的页被预算闸拒绝");
        Check(pool.IsResident(3), "距离优先: 跳过大页后小页能装下");
        Check(u.admitted.size() == 2 && pool.GetResidentBytes() == 70,
              "距离优先: admitted=2, 驻留字节=70");
    }

    // --- 3. 结构页永不驱逐 --------------------------------------------------
    {
        // 预算 60 = 结构页(30) + 叶子(30)，两者都能装下。
        PagePool pool(60);
        pool.UpsertPage(MakeInternal(100, 30));  // 结构页
        pool.UpsertPage(MakeLeaf(1, 30, 10.0f));

        std::vector<uint32_t> want = {1};
        nanite_pages::PoolUpdate u = pool.Update(want, 1.0);

        Check(pool.IsResident(100), "结构页: internal 永远驻留");
        Check(pool.IsResident(1), "结构页: 叶子页正常驻留");
        Check(u.admitted.size() == 1, "结构页: 结构页不进 admitted（只有叶子进）");
        Check(pool.GetResidentBytes() == 60, "结构页: 驻留字节含结构页(30)+叶子(30)");

        // 结构页占预算后，叶子装不下 → 叶子被拒，但结构页仍驻留。
        PagePool pool2(40);  // 结构页30占掉，只剩10 < 叶子30
        pool2.UpsertPage(MakeInternal(100, 30));
        pool2.UpsertPage(MakeLeaf(1, 30, 10.0f));
        pool2.Update({1}, 1.0);
        Check(pool2.IsResident(100), "结构页: 预算紧张时 internal 仍驻留");
        Check(!pool2.IsResident(1), "结构页: 预算被结构页占满后叶子装不下");
    }

    // --- 4. 增量换入/换出：只含本帧变化的页 ---------------------------------
    {
        PagePool pool(100);
        pool.UpsertPage(MakeLeaf(1, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(2, 30, 20.0f));
        pool.Update({1, 2}, 1.0);  // 帧1：两页都进池

        // 帧2：相机远离，只有页1还 want。
        pool.UpsertPage(MakeLeaf(1, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(2, 30, 90.0f));  // 页2变远
        nanite_pages::PoolUpdate u2 = pool.Update({1}, 2.0);

        Check(pool.IsResident(1) && !pool.IsResident(2), "增量: 页2被换出");
        Check(u2.evicted.size() == 1 && u2.evicted[0] == 2, "增量: evicted 只含页2");
        Check(u2.admitted.empty(), "增量: 无新进池页");

        // 帧3：页2又变近 → 重新进池。
        pool.UpsertPage(MakeLeaf(2, 30, 15.0f));
        nanite_pages::PoolUpdate u3 = pool.Update({1, 2}, 3.0);
        Check(u3.admitted.size() == 1 && u3.admitted[0] == 2, "增量: 页2重新进池进 admitted");
        Check(u3.evicted.empty(), "增量: 无换出");
    }

    // --- 5. LRU 驱逐：预算超限时最久未用的先被驱逐 --------------------------
    {
        PagePool pool(90);
        pool.UpsertPage(MakeLeaf(1, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(2, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(3, 30, 10.0f));
        // 帧1：三页距离相同，全部驻留（90 == 预算）。给它们不同的 last_used。
        pool.Touch(1, 1.0);
        pool.Touch(2, 2.0);
        pool.Touch(3, 3.0);
        pool.Update({1, 2, 3}, /*now=*/1.0);
        Check(pool.GetResidentCount() == 3, "LRU: 三页都驻留（正好填满预算）");

        // 帧2：新增页4，预算装不下（4*30=120 > 90）。四页距离相同，按 LRU 应驱逐
        // 最久未用的。页1 last_used=1.0 最旧 → 被驱逐；页2(2.0)/页3(3.0)/页4(4.0)驻留。
        pool.UpsertPage(MakeLeaf(4, 30, 10.0f));
        pool.Touch(4, 4.0);
        nanite_pages::PoolUpdate u2 = pool.Update({1, 2, 3, 4}, 4.0);

        Check(!pool.IsResident(1), "LRU: 最久未用的页1被驱逐");
        Check(pool.IsResident(2) && pool.IsResident(3) && pool.IsResident(4),
              "LRU: 较新的页2/3/4驻留");
        bool evicted_is_1 = (u2.evicted.size() == 1 && u2.evicted[0] == 1);
        Check(evicted_is_1, "LRU: evicted 只含页1");
    }

    // --- 6. Touch 防驱逐 ----------------------------------------------------
    {
        PagePool pool(60);
        pool.UpsertPage(MakeLeaf(1, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(2, 30, 10.0f));
        // 帧1：两页都驻留。页1被渲染层频繁用（Touch 到很新），页2很久没用。
        pool.Touch(1, 1.0);
        pool.Touch(2, 1.0);
        pool.Update({1, 2}, 1.0);
        pool.Touch(1, 100.0);  // 页1刚被用，时间戳推到很新
        // 新页3进来（Touch 到比页2新），预算只够留两个 → 最旧的页2被驱逐。
        pool.UpsertPage(MakeLeaf(3, 30, 10.0f));
        pool.Touch(3, 50.0);
        pool.Update({1, 2, 3}, 5.0);
        // 页1=100.0（最新）、页3=50.0、页2=1.0（最旧）→ 页2被驱逐。
        Check(pool.IsResident(1) && pool.IsResident(3) && !pool.IsResident(2),
              "Touch: 被刷新的页1免于驱逐，最旧的页2被驱逐");
    }

    // --- 7. RemovePage / SetBudget 边界 -------------------------------------
    {
        PagePool pool(100);
        pool.UpsertPage(MakeLeaf(1, 60, 10.0f));
        pool.Update({1}, 1.0);
        Check(pool.GetResidentBytes() == 60, "RemovePage: 初始驻留 60");

        pool.SetBudget(30);  // 预算缩到比页1还小
        pool.Update({1}, 2.0);
        Check(!pool.IsResident(1), "SetBudget: 预算缩到装不下 → 页被驱逐");

        pool.RemovePage(1);
        Check(pool.GetResidentBytes() == 0, "RemovePage: 移除后驻留字节归零");
        Check(!pool.IsResident(1), "RemovePage: 移除后不再驻留");
    }

    // --- 8. 空池 / 零预算边界 -----------------------------------------------
    {
        PagePool pool(0);  // 零预算被夹成 1
        Check(pool.GetBudget() == 1, "零预算: 夹成 1");
        pool.UpsertPage(MakeLeaf(1, 5, 10.0f));
        nanite_pages::PoolUpdate u = pool.Update({1}, 1.0);
        Check(!pool.IsResident(1), "零预算: 任何叶子页都装不下");
        Check(u.evicted.empty() && u.admitted.empty(), "零预算: 无换入换出");
    }

    std::printf("============================================================\n");
    if (g_Failures == 0)
    {
        std::printf("NanitePagePoolSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("NanitePagePoolSmokeTest: %d CHECK(S) FAILED\n", g_Failures);
    return 1;
}
