#pragma once

// ============================================================================
// NanitePageSlotTable — 页槽位分配器（纯 CPU，header-only，无 RHI/引擎依赖）
// ----------------------------------------------------------------------------
// 这是页池（NanitePagePool.h）的 admitted/evicted 增量队列与**真实 RHI 上传/回收**
// 之间唯一 CPU 可验的桥。
//
// 现状（为什么需要它）：NaniteBuildSceneGPUData（NaniteGPUData.h:202）把**整个场景**
// 的几何摊成一段稠密 positions/indices，NaniteRenderer（:1081-1111）每帧把**全量**
// UploadBytes 到 GPU。cluster_loaded_flags 只闸渲染提交（PrepareRenderData:431 跳过
// 未驻留叶子），**不闸上传** —— 所以 GPU 始终物理持有全部几何，页池的 admitted/evicted
// 队列没有东西可拷/可释放。
//
// 要让"拷进/释放显存"成立，GPU 数据布局必须从"整场景稠密"改成"持久页槽位 + 间接寻址"：
//   - 持久 GPU vertex/index 缓冲，容量 = 页池预算（不是整场景）。
//   - 本表维护 page_id → (vertex_slot_base, index_slot_base) 的稳定映射 + 空闲表。
//     admitted 页认领一个槽位（渲染层随后把该页几何切片拷进对应 GPU 偏移）；
//     evicted 页把槽位还回空闲表（渲染层随后标记该段可复用）。
//   - shader 侧间接寻址：cluster geometry 经页表解析，而非稠密 vertex_base。
//
// 本层**只管槽位簿记**：稳定句柄、空闲表回收、churn 下不泄漏不重叠。真正把几何切片
// 拷进/清出 GPU 缓冲是渲染绑定（RHI 上传/回收），留真机。
//
// 槽位以"元素"计（vertex 槽 / index 槽各自独立容量），调用方按自己的元素大小换算字节。
// 一个页同时占 vertex 槽和 index 槽（两者必须同时拿到才 admit，避免半驻留）。
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nanite_pages
{
    // 一个已驻留页在 GPU 持久缓冲里的槽位。base 是元素偏移（不是字节）。
    struct PageSlot
    {
        uint32_t page_id {0};
        uint32_t vertex_slot_base {0};  // 该页顶点在持久 vertex 缓冲里的起始元素下标
        uint32_t vertex_slot_count {0};
        uint32_t index_slot_base {0};   // 该页索引在持久 index 缓冲里的起始元素下标
        uint32_t index_slot_count {0};
    };

    class PageSlotTable
    {
    public:
        // vertex_capacity / index_capacity = 持久 GPU 缓冲的元素容量（= 页池预算换算的元素数）。
        explicit PageSlotTable(uint32_t vertex_capacity, uint32_t index_capacity)
            : m_VertexCapacity(vertex_capacity), m_IndexCapacity(index_capacity)
        {
            ResetFreeLists();
        }

        uint32_t GetVertexCapacity() const { return m_VertexCapacity; }
        uint32_t GetIndexCapacity() const { return m_IndexCapacity; }
        uint32_t GetVertexUsed() const { return m_VertexUsed; }
        uint32_t GetIndexUsed() const { return m_IndexUsed; }
        size_t GetSlotCount() const { return m_Slots.size(); }

        bool HasSlot(uint32_t page_id) const
        {
            return m_PageToSlot.find(page_id) != m_PageToSlot.end();
        }

        // 查某页的槽位（未驻留返回 nullptr）。
        const PageSlot* FindSlot(uint32_t page_id) const
        {
            auto it = m_PageToSlot.find(page_id);
            return it != m_PageToSlot.end() ? &m_Slots[it->second] : nullptr;
        }

        // 为一页认领槽位。需要 vertex_count + index_count 连续空间。
        // 成功返回 true 并填 out_slot；空间不足返回 false（本帧不 admit，调用方可降级/告警）。
        // 已有槽位的页重复调用是 no-op（返回既有槽位，不重复分配）。
        bool Admit(uint32_t page_id, uint32_t vertex_count, uint32_t index_count, PageSlot& out_slot)
        {
            auto existing = m_PageToSlot.find(page_id);
            if (existing != m_PageToSlot.end())
            {
                out_slot = m_Slots[existing->second];
                return true;
            }

            // 首选空闲段（best-fit：选能装下的最小段，减少碎片）。
            int v_seg = FindFreeSegment(m_VertexFree, vertex_count, m_VertexCapacity);
            int i_seg = FindFreeSegment(m_IndexFree, index_count, m_IndexCapacity);
            if (v_seg < 0 || i_seg < 0)
                return false;  // 空间不足，本帧不 admit（调用方决定降级）

            const size_t slot_idx = m_Slots.size();
            PageSlot slot;
            slot.page_id = page_id;
            slot.vertex_slot_base = m_VertexFree[v_seg].start;
            slot.vertex_slot_count = vertex_count;
            slot.index_slot_base = m_IndexFree[i_seg].start;
            slot.index_slot_count = index_count;
            m_Slots.push_back(slot);
            m_PageToSlot[page_id] = slot_idx;

            CarveSegment(m_VertexFree, v_seg, vertex_count);
            CarveSegment(m_IndexFree, i_seg, index_count);
            m_VertexUsed += vertex_count;
            m_IndexUsed += index_count;

            out_slot = slot;
            return true;
        }

        // 释放某页的槽位回空闲表。未驻留的页是安全 no-op（返回 false）。
        bool Evict(uint32_t page_id)
        {
            auto it = m_PageToSlot.find(page_id);
            if (it == m_PageToSlot.end())
                return false;

            const PageSlot& s = m_Slots[it->second];
            ReleaseSegment(m_VertexFree, s.vertex_slot_base, s.vertex_slot_count, m_VertexCapacity);
            ReleaseSegment(m_IndexFree, s.index_slot_base, s.index_slot_count, m_IndexCapacity);
            m_VertexUsed -= s.vertex_slot_count;
            m_IndexUsed -= s.index_slot_count;

            // 用末尾元素填洞，保持 m_Slots 稠密；同步被移动页的索引。
            const size_t last = m_Slots.size() - 1;
            if (it->second != last)
            {
                m_Slots[it->second] = m_Slots[last];
                m_PageToSlot[m_Slots[it->second].page_id] = it->second;
            }
            m_Slots.pop_back();
            m_PageToSlot.erase(it);
            return true;
        }

        // 全清（clear() 时用）。所有槽位归还，空闲表重置为整段。
        void Reset()
        {
            m_Slots.clear();
            m_PageToSlot.clear();
            m_VertexUsed = 0;
            m_IndexUsed = 0;
            ResetFreeLists();
        }

    private:
        struct Segment
        {
            uint32_t start {0};
            uint32_t length {0};
        };

        void ResetFreeLists()
        {
            m_VertexFree.clear();
            m_IndexFree.clear();
            if (m_VertexCapacity > 0)
                m_VertexFree.push_back({0, m_VertexCapacity});
            if (m_IndexCapacity > 0)
                m_IndexFree.push_back({0, m_IndexCapacity});
        }

        // best-fit：找能装下 need 的最小空闲段，返回下标；没有返回 -1。
        int FindFreeSegment(const std::vector<Segment>& free_list, uint32_t need, uint32_t) const
        {
            int best = -1;
            uint32_t best_len = 0xFFFFFFFFu;
            for (size_t i = 0; i < free_list.size(); ++i)
            {
                if (free_list[i].length >= need && free_list[i].length < best_len)
                {
                    best = static_cast<int>(i);
                    best_len = free_list[i].length;
                    if (best_len == need)
                        break;  // 完美匹配，不用再找
                }
            }
            return best;
        }

        // 从 free_list[seg] 前部切走 need，剩余部分留在原段或删除。
        void CarveSegment(std::vector<Segment>& free_list, int seg, uint32_t need)
        {
            Segment& s = free_list[static_cast<size_t>(seg)];
            s.start += need;
            s.length -= need;
            if (s.length == 0)
                free_list.erase(free_list.begin() + seg);
        }

        // 把 [start, start+length) 还回空闲表，并合并相邻段（防碎片）。
        void ReleaseSegment(std::vector<Segment>& free_list, uint32_t start, uint32_t length, uint32_t)
        {
            if (length == 0)
                return;
            // 插入后按 start 排序并合并相邻。
            free_list.push_back({start, length});
            // 小规模：插入排序维持有序（空闲段通常个位数到几十个）。
            for (size_t i = free_list.size(); i > 1 && free_list[i - 1].start < free_list[i - 2].start; --i)
                std::swap(free_list[i - 1], free_list[i - 2]);

            std::vector<Segment> merged;
            merged.reserve(free_list.size());
            for (Segment& seg : free_list)
            {
                if (!merged.empty() && merged.back().start + merged.back().length == seg.start)
                    merged.back().length += seg.length;  // 相邻，合并
                else
                    merged.push_back(seg);
            }
            free_list.swap(merged);
        }

        uint32_t m_VertexCapacity {0};
        uint32_t m_IndexCapacity {0};
        uint32_t m_VertexUsed {0};
        uint32_t m_IndexUsed {0};
        std::vector<PageSlot> m_Slots;                       // 稠密槽位数组
        std::unordered_map<uint32_t, size_t> m_PageToSlot;   // page_id → m_Slots 下标
        std::vector<Segment> m_VertexFree;                   // 空闲 vertex 段（有序、已合并）
        std::vector<Segment> m_IndexFree;                    // 空闲 index 段（有序、已合并）
    };
}  // namespace nanite_pages
