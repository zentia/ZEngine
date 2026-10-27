#pragma once

// ============================================================================
// NanitePageUploadPlanner — 页池增量上传计划器（纯 CPU，header-only，无 RHI/引擎依赖）
// ----------------------------------------------------------------------------
// 这是页池（NanitePagePool.h 的 admitted/evicted 增量队列）+ 页槽位分配器
// （NanitePageSlotTable.h）与**渲染器真实 UploadBytes** 之间最后一层 CPU 可验的桥。
//
// 输入：本帧的 PoolUpdate（admitted/evicted page_id）+ 每页的几何描述符
//       （source 在 mesh 几何流里的 offset/count）+ 页槽位表。
// 输出：一份**具体到字节段**的上传/释放计划：
//   - uploads  ：admitted 页 → 认领了哪个 GPU 槽位（dst）+ 从 mesh 流哪段拷（src）。
//   - releases ：evicted 页 → 哪段槽位已归还（渲染器可标记 GPU 段可复用）。
//   - failed   ：想 admit 但槽位不够的页（预算装不下，本帧不拷、保守不渲染）。
//
// 渲染器拿到计划后，把 uploads 逐条 UploadBytes 进持久页缓冲（dst slot_base ← src 几何切片）、
// 把 releases 的槽位标记空闲。**这一步拷贝是渲染绑定**（需真机 RHI），本层只算计划。
//
// 本层刻意不碰任何引擎/RHI 类型：几何描述符由调用方（manager）从 mesh resource 填好喂进来，
// 所以可 headless 单测（NanitePageUploadPlannerSmokeTest）。
// ============================================================================

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "NanitePagePool.h"
#include "NanitePageSlotTable.h"

namespace nanite_pages
{
    // 一页的几何描述符（source，在 mesh 的 vertex/index 流里）。由 manager 从
    // NaniteMeshResource + NaniteCluster 填好喂进来，本层不认识这些类型。
    // mesh_id 带上：渲染器要用它取 mesh resource 作为拷贝源。
    struct PageGeometryDesc
    {
        uint64_t mesh_id {0};
        uint32_t vertex_offset {0};  // 该页顶点在 mesh vertex 流里的起始元素下标
        uint32_t vertex_count {0};
        uint32_t index_offset {0};   // 该页索引在 mesh index 流里的起始元素下标
        uint32_t index_count {0};
    };

    // 一条上传命令：把 mesh 流 [src_*_offset, +count) 拷进持久 GPU 缓冲的 [dst_*_slot_base, +count)。
    struct PageUploadCmd
    {
        uint32_t page_id {0};
        uint64_t mesh_id {0};  // 拷贝源 mesh（渲染器用它取 NaniteMeshResource）
        uint32_t src_vertex_offset {0};
        uint32_t dst_vertex_slot_base {0};
        uint32_t vertex_count {0};
        uint32_t src_index_offset {0};
        uint32_t dst_index_slot_base {0};
        uint32_t index_count {0};
    };

    // 一条释放命令：该页的 GPU 槽位已归还，渲染器可标记这段可复用（不必清零，下次 admit 会覆盖）。
    struct PageReleaseCmd
    {
        uint32_t page_id {0};
        uint32_t vertex_slot_base {0};
        uint32_t vertex_slot_count {0};
        uint32_t index_slot_base {0};
        uint32_t index_slot_count {0};
    };

    struct StreamingUploadPlan
    {
        std::vector<PageUploadCmd> uploads;    // admitted：拷几何进这些槽位
        std::vector<PageReleaseCmd> releases;  // evicted：这些槽位已归还
        std::vector<uint32_t> failed_admits;   // 想进但槽位不够的页（本帧不拷、保守不渲染）

        // 计划涉及的拷贝字节数（渲染器用它估带宽 / 决定是否分帧）。element 大小由调用方给：
        // 现行 GPU 模型 vertex 是 Vector4(16B)、index 是 uint32(4B)。
        uint64_t UploadBytes(uint32_t vertex_elem_size, uint32_t index_elem_size) const
        {
            uint64_t bytes = 0;
            for (const PageUploadCmd& u : uploads)
                bytes += static_cast<uint64_t>(u.vertex_count) * vertex_elem_size +
                         static_cast<uint64_t>(u.index_count) * index_elem_size;
            return bytes;
        }
    };

    // 核心计划器。pool = 本帧页池增量队列；geometry = page_id → 几何描述符（manager 填）；
    // slots = 渲染器持有的页槽位表（会被本函数更新：admit 认领、evict 归还）。
    inline StreamingUploadPlan PlanStreamingUploads(
        const PoolUpdate& pool,
        const std::unordered_map<uint32_t, PageGeometryDesc>& geometry,
        PageSlotTable& slots)
    {
        StreamingUploadPlan plan;

        // 1) 先释放 evicted 页的槽位（腾出空间给本帧 admit，减少误拒）。
        for (uint32_t page_id : pool.evicted)
        {
            const PageSlot* s = slots.FindSlot(page_id);
            if (s == nullptr)
                continue;  // 本来就没占槽（如结构页或上一帧已释放），安全跳过
            PageReleaseCmd rel;
            rel.page_id = page_id;
            rel.vertex_slot_base = s->vertex_slot_base;
            rel.vertex_slot_count = s->vertex_slot_count;
            rel.index_slot_base = s->index_slot_base;
            rel.index_slot_count = s->index_slot_count;
            plan.releases.push_back(rel);
            slots.Evict(page_id);
        }

        // 2) 为 admitted 页认领槽位并产出上传命令。
        for (uint32_t page_id : pool.admitted)
        {
            auto it = geometry.find(page_id);
            if (it == geometry.end())
                continue;  // 没有几何描述符（异常登记），跳过
            const PageGeometryDesc& desc = it->second;

            PageSlot slot;
            if (!slots.Admit(page_id, desc.vertex_count, desc.index_count, slot))
            {
                plan.failed_admits.push_back(page_id);  // 槽位不够：本帧不拷、保守不渲染
                continue;
            }

            PageUploadCmd up;
            up.page_id = page_id;
            up.mesh_id = desc.mesh_id;
            up.src_vertex_offset = desc.vertex_offset;
            up.dst_vertex_slot_base = slot.vertex_slot_base;
            up.vertex_count = desc.vertex_count;
            up.src_index_offset = desc.index_offset;
            up.dst_index_slot_base = slot.index_slot_base;
            up.index_count = desc.index_count;
            plan.uploads.push_back(up);
        }

        return plan;
    }
}  // namespace nanite_pages
