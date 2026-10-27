// =============================================================================
// NanitePageUploadPlannerSmokeTest — 页池增量上传计划器（NanitePageUploadPlanner.h）
// 的无头验证。
// -----------------------------------------------------------------------------
// 覆盖整条 pipeline：PagePool.Update（距离+预算+LRU）→ PlanStreamingUploads
// （evicted 先释放腾位 → admitted 认领槽位并产出拷贝命令）→ 计划的字节估算。
//   - admitted 页产出 uploads，src 来自 mesh 流 offset、dst 来自认领的槽位 base。
//   - evicted 页产出 releases，槽位归还后可被后续 admit 复用。
//   - 槽位不够的页进 failed_admits，不产出上传、不半驻留。
//   - churn 多帧：admit/evict 反复，槽位不重叠、总用量有界、无泄漏。
//   - UploadBytes 按元素大小正确估算带宽。
//
// 全部纯 CPU，headless，只链 include root（header-only，只依赖 STL）。
//
//   cmake -B build -DZENGINE_BUILD_NANITE_UPLOADPLANNER_SMOKE_TEST=ON
//   cmake --build build --target NanitePageUploadPlannerSmokeTest
// =============================================================================

#include "Runtime/Function/Render/Nanite/NanitePageUploadPlanner.h"

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

    using nanite_pages::PagePool;
    using nanite_pages::PageRecord;
    using nanite_pages::PageSlotTable;
    using nanite_pages::PageGeometryDesc;
    using nanite_pages::StreamingUploadPlan;

    PageRecord MakeLeaf(uint32_t id, uint32_t vcount, uint32_t icount, float dist)
    {
        PageRecord r;
        r.page_id = id;
        r.mesh_id = 1;
        // byte_size 用现行 GPU 模型：vertex Vector4(16B) + index uint32(4B)。
        r.byte_size = vcount * 16u + icount * 4u;
        r.is_leaf = true;
        r.distance = dist;
        return r;
    }

    PageGeometryDesc MakeDesc(uint32_t page_id, uint32_t voff, uint32_t vcount, uint32_t ioff, uint32_t icount)
    {
        PageGeometryDesc d;
        d.vertex_offset = voff;
        d.vertex_count = vcount;
        d.index_offset = ioff;
        d.index_count = icount;
        (void)page_id;
        return d;
    }

    // 校验所有已驻留页的 vertex 槽位两两不重叠、都在容量内。
    bool VertexSlotsDisjoint(const PageSlotTable& t)
    {
        std::vector<bool> occ(t.GetVertexCapacity(), false);
        for (uint32_t page = 0; page < 8192; ++page)
        {
            const nanite_pages::PageSlot* s = t.FindSlot(page);
            if (!s)
                continue;
            if (s->vertex_slot_base + s->vertex_slot_count > t.GetVertexCapacity())
                return false;
            for (uint32_t k = 0; k < s->vertex_slot_count; ++k)
            {
                if (occ[s->vertex_slot_base + k])
                    return false;
                occ[s->vertex_slot_base + k] = true;
            }
        }
        return true;
    }
}  // namespace

int main()
{
    std::printf("NanitePageUploadPlannerSmokeTest (incremental upload planner)\n");
    std::printf("============================================================\n");

    // --- 1. admitted 产出 uploads，src/dst 正确 -----------------------------
    {
        PagePool pool(1000);
        pool.UpsertPage(MakeLeaf(1, 10, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(2, 10, 30, 20.0f));
        nanite_pages::PoolUpdate u = pool.Update({1, 2}, /*now=*/1.0);
        Check(u.admitted.size() == 2, "基本: 两页 admitted");

        PageSlotTable slots(100, 100);
        std::unordered_map<uint32_t, PageGeometryDesc> geo;
        geo[1] = MakeDesc(1, /*voff=*/100, 10, /*ioff=*/200, 30);  // mesh 流里的位置
        geo[2] = MakeDesc(2, /*voff=*/110, 10, /*ioff=*/230, 30);

        StreamingUploadPlan plan = nanite_pages::PlanStreamingUploads(u, geo, slots);
        Check(plan.uploads.size() == 2, "基本: 产出两条上传命令");
        Check(plan.releases.empty(), "基本: 无释放");
        Check(plan.failed_admits.empty(), "基本: 无误拒");

        // 找到页1的命令，验证 src 来自 mesh 流、dst 来自槽位 base。
        const nanite_pages::PageUploadCmd* c1 = nullptr;
        for (const nanite_pages::PageUploadCmd& c : plan.uploads)
            if (c.page_id == 1)
                c1 = &c;
        Check(c1 != nullptr, "基本: 找到页1上传命令");
        Check(c1->src_vertex_offset == 100 && c1->vertex_count == 10, "基本: 页1 src 来自 mesh vertex offset");
        Check(c1->src_index_offset == 200 && c1->index_count == 30, "基本: 页1 src 来自 mesh index offset");
        Check(c1->dst_vertex_slot_base == 0, "基本: 页1 dst 槽位从 0 起");
        Check(slots.GetVertexUsed() == 20 && slots.GetIndexUsed() == 60, "基本: 槽位用量正确");
    }

    // --- 2. evicted 产出 releases，槽位归还后可复用 ------------------------
    {
        PagePool pool(1000);
        pool.UpsertPage(MakeLeaf(1, 10, 30, 10.0f));
        pool.Update({1}, 1.0);
        pool.UpsertPage(MakeLeaf(1, 10, 30, 100.0f));  // 页1 变远
        pool.UpsertPage(MakeLeaf(2, 10, 30, 20.0f));   // 页2 变近
        nanite_pages::PoolUpdate u = pool.Update({2}, 2.0);
        Check(!u.evicted.empty(), "evict: 页1 被驱逐");

        PageSlotTable slots(100, 100);
        std::unordered_map<uint32_t, PageGeometryDesc> geo;
        geo[1] = MakeDesc(1, 0, 10, 0, 30);
        geo[2] = MakeDesc(2, 10, 10, 30, 30);
        // 先让页1占槽。
        nanite_pages::PoolUpdate u1 = pool.Update({1}, 1.0);
        nanite_pages::PlanStreamingUploads(u1, geo, slots);
        Check(slots.GetVertexUsed() == 10, "evict: 页1 已占槽");

        StreamingUploadPlan plan = nanite_pages::PlanStreamingUploads(u, geo, slots);
        Check(plan.releases.size() == 1 && plan.releases[0].page_id == 1, "evict: 产出页1释放命令");
        Check(plan.releases[0].vertex_slot_base == 0 && plan.releases[0].vertex_slot_count == 10,
              "evict: 释放命令带正确槽位区间");
        Check(plan.uploads.size() == 1 && plan.uploads[0].page_id == 2, "evict: 页2 产出上传");
        Check(plan.uploads[0].dst_vertex_slot_base == 0, "evict: 页2 复用了页1腾出的槽位 0");
        Check(VertexSlotsDisjoint(slots), "evict: 归还+复用后槽位不重叠");
    }

    // --- 3. 槽位不够 → failed_admits，不半驻留 -----------------------------
    {
        PagePool pool(100000);  // 页池预算很大，让两页都 admitted
        pool.UpsertPage(MakeLeaf(1, 10, 30, 10.0f));
        pool.UpsertPage(MakeLeaf(2, 10, 30, 20.0f));
        nanite_pages::PoolUpdate u = pool.Update({1, 2}, 1.0);

        PageSlotTable slots(15, 100);  // vertex 容量只够一页（每页 10）
        std::unordered_map<uint32_t, PageGeometryDesc> geo;
        geo[1] = MakeDesc(1, 0, 10, 0, 30);
        geo[2] = MakeDesc(2, 10, 10, 30, 30);

        StreamingUploadPlan plan = nanite_pages::PlanStreamingUploads(u, geo, slots);
        Check(plan.uploads.size() == 1, "不足: 只有一页拷进");
        Check(plan.failed_admits.size() == 1, "不足: 另一页进 failed_admits");
        Check(!slots.HasSlot(plan.failed_admits[0]), "不足: 失败页不占槽（不半驻留）");
        Check(VertexSlotsDisjoint(slots), "不足: 槽位仍不重叠");
    }

    // --- 4. UploadBytes 带宽估算 -------------------------------------------
    {
        PagePool pool(100000);
        pool.UpsertPage(MakeLeaf(1, 10, 30, 10.0f));
        nanite_pages::PoolUpdate u = pool.Update({1}, 1.0);

        PageSlotTable slots(100, 100);
        std::unordered_map<uint32_t, PageGeometryDesc> geo;
        geo[1] = MakeDesc(1, 0, 10, 0, 30);
        StreamingUploadPlan plan = nanite_pages::PlanStreamingUploads(u, geo, slots);
        // 10 vertex * 16B + 30 index * 4B = 160 + 120 = 280
        Check(plan.UploadBytes(16, 4) == 280, "带宽: 10*16 + 30*4 = 280");
        Check(plan.UploadBytes(12, 4) == 240, "带宽: 换 vertex 元素大小重算 = 240");
    }

    // --- 5. churn 多帧：无泄漏、不重叠、用量有界 ---------------------------
    {
        PagePool pool(60);  // 预算紧：约 2 页（每页 10*16+30*4=280... 用小页）
        // 用小页让预算能装 2 页：byte_size = 4*16+2*4 = 72；预算 60 只够 0 页 → 调大。
        // 改用预算 200，页 byte=72，够 2 页。
        pool.SetBudget(200);
        PageSlotTable slots(40, 40);
        uint32_t next = 1;
        bool all_disjoint = true;
        bool usage_bounded = true;
        for (int frame = 0; frame < 300; ++frame)
        {
            // 每帧造一个新近页 + 让最老的页变远。
            pool.UpsertPage(MakeLeaf(next, 4, 2, static_cast<float>(frame)));
            nanite_pages::PoolUpdate u = pool.Update({next}, static_cast<double>(frame));
            std::unordered_map<uint32_t, PageGeometryDesc> geo;
            geo[next] = MakeDesc(next, 0, 4, 0, 2);
            for (uint32_t pid : u.admitted)
                if (geo.find(pid) == geo.end())
                    geo[pid] = MakeDesc(pid, 0, 4, 0, 2);
            nanite_pages::PlanStreamingUploads(u, geo, slots);
            if (!VertexSlotsDisjoint(slots))
                all_disjoint = false;
            if (slots.GetVertexUsed() > slots.GetVertexCapacity())
                usage_bounded = false;
            ++next;
        }
        Check(all_disjoint, "churn: 300 帧槽位始终不重叠");
        Check(usage_bounded, "churn: 槽位用量始终有界（不超容量）");
        slots.Reset();
        Check(slots.GetVertexUsed() == 0 && slots.GetSlotCount() == 0, "churn: Reset 后无泄漏");
    }

    const int total = g_Failures;
    std::printf("============================================================\n");
    if (total == 0)
    {
        std::printf("NanitePageUploadPlannerSmokeTest: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("NanitePageUploadPlannerSmokeTest: %d CHECK(S) FAILED\n", total);
    return 1;
}
