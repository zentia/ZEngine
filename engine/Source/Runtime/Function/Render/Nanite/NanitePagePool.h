#pragma once

// ============================================================================
// NanitePagePool — 页池簿记层（纯 CPU，header-only，无 RHI/无引擎依赖）
// ----------------------------------------------------------------------------
// UpdateStreaming 现状（NaniteCluster.cpp:574）是**策略/可见性闸门**：按相机距离标
// cluster_loaded_flags，但所有 cluster 共享一段 depth-first 字节流，中间 cluster 没法
// 单独释放（卸载会破坏邻居 offset）。注释里承诺的"真正的按页驻留要等页池落地"就是本文件。
//
// 这里把"页池的簿记"抽成不依赖任何 GPU/引擎类型的纯逻辑，可 headless 单测
// （NanitePagePoolSmokeTest）：
//   - 页（page）= 一组 cluster 的驻留单位。池子有固定字节预算（budget_bytes）。
//   - 驻留判据仍是距离（与 UpdateStreaming 同一语义），但**进池要过预算闸**：
//     想驻留的叶子页按"距离优先 + LRU 破平"排序，预算装不下的被驱逐（evict）。
//   - 驱逐只动叶子页；结构页（internal）永远驻留（与 UpdateStreaming 的"内部节点
//     永远驻留"不变量一致）——卸载它们没有渲染收益，只会让层次查询拿到空缺。
//   - LRU：每页一个 last_used 时间戳；预算紧张时优先驱逐最久未用的叶子页。
//
// 本层**只算簿记**（谁该驻留、谁被驱逐、换入/换出队列），不碰 GPU 上传——把换入/换出
// 队列真正拷进/释放显存是渲染绑定，留真机清单。语义从 UpdateStreaming 的"标不标"
// 升级为"装不装得进预算池"。
//
// 进/出队列的判定：Update 结束时快照一份"当前驻留页集合"，下一帧 Update 与之比对——
// 新进池的进 admitted，不再驻留的进 evicted。这样调用方每帧拿到的是**增量**，不是全量。
// ============================================================================

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nanite_pages
{
    // 一页的簿记元数据。page_id 由调用方（manager）分配，本层只透传。
    struct PageRecord
    {
        uint32_t page_id {0};
        uint64_t mesh_id {0};
        uint32_t byte_size {0};     // 该页几何字节（vertex+index），进池预算按此计
        bool is_leaf {true};        // 结构页(internal)永不驱逐
        float distance {0.0f};      // 到相机的距离（越小越优先驻留）
        double last_used {0.0};     // LRU 时间戳（越大越新）
    };

    // 一次 Update 的簿记结果。换入/换出队列供渲染层真正拷贝/释放显存。
    struct PoolUpdate
    {
        std::vector<uint32_t> evicted;   // 本帧被驱逐出池的叶子 page_id
        std::vector<uint32_t> admitted;  // 本帧新进池的叶子 page_id
        uint64_t resident_bytes {0};     // 池内当前驻留字节（含结构页）
        size_t resident_count {0};       // 池内当前驻留页数
    };

    class PagePool
    {
    public:
        explicit PagePool(uint64_t budget_bytes) { SetBudget(budget_bytes); }

        void SetBudget(uint64_t budget_bytes) { m_Budget = budget_bytes > 0 ? budget_bytes : 1; }
        uint64_t GetBudget() const { return m_Budget; }
        uint64_t GetResidentBytes() const { return m_ResidentBytes; }
        size_t GetResidentCount() const { return m_ResidentCount; }

        // 注册/更新一页的元数据（manager 每帧喂进来）。不改变驻留状态——驻留由 Update
        // 决定。**保留已有的 last_used**：Upsert 是"元数据刷新"，不是生命周期重置，否则
        // 每帧喂元数据会把 LRU 时间戳清零、让 Touch 失效。新页 last_used 从 rec 带入（默认 0）。
        void UpsertPage(const PageRecord& rec)
        {
            auto it = m_Pages.find(rec.page_id);
            if (it != m_Pages.end())
            {
                const double last_used = it->second.rec.last_used;
                it->second.rec = rec;
                it->second.rec.last_used = last_used;
            }
            else
            {
                m_Pages[rec.page_id] = {rec, false};
            }
        }

        void RemovePage(uint32_t page_id) { m_Pages.erase(page_id); }

        bool IsResident(uint32_t page_id) const
        {
            auto it = m_Pages.find(page_id);
            return it != m_Pages.end() && it->second.resident;
        }

        // 刷新某页的 LRU 时间戳（渲染层真正用到该页时调用，防被预算闸驱逐）。
        void Touch(uint32_t page_id, double now)
        {
            auto it = m_Pages.find(page_id);
            if (it != m_Pages.end())
                it->second.rec.last_used = now;
        }

        // 核心簿记。want_resident_ids = 本帧"期望驻留"的叶子页 id（manager 按距离算出）。
        // now = 本帧时间戳（驱动 LRU）。返回增量换入/换出队列 + 池内驻留统计。
        PoolUpdate Update(const std::vector<uint32_t>& want_resident_ids, double now)
        {
            // 1) 结构页强制驻留（不参与驱逐、不进换入/换出队列）。
            for (auto& kv : m_Pages)
            {
                if (!kv.second.rec.is_leaf)
                    kv.second.resident = true;
            }

            // 2) 收集本帧 want 的叶子页。**不**在这里刷 last_used——LRU 时间戳只由
            //    Touch（渲染层真正用到该页）推进，否则"被 want"会让所有页时间戳塌成
            //    同一帧，LRU 破平失效。want 只表达"这帧想驻留"，不等于"这帧被渲染用"。
            std::vector<uint32_t> want_leafs;
            want_leafs.reserve(want_resident_ids.size());
            for (uint32_t id : want_resident_ids)
            {
                auto it = m_Pages.find(id);
                if (it == m_Pages.end() || !it->second.rec.is_leaf)
                    continue;
                want_leafs.push_back(id);
            }

            // 3) 想驻留的叶子页按距离升序（近的优先），距离相同按 LRU（新的优先）。
            std::sort(want_leafs.begin(), want_leafs.end(), [&](uint32_t a, uint32_t b) {
                const Entry& ea = m_Pages[a];
                const Entry& eb = m_Pages[b];
                if (ea.rec.distance != eb.rec.distance)
                    return ea.rec.distance < eb.rec.distance;
                return ea.rec.last_used > eb.rec.last_used;
            });

            // 4) 先清空所有叶子页的驻留位，再按预算从近到远重新装填。
            //    这样"本帧不再 want"的叶子页自然落榜 → 被驱逐；预算装不下的远页也被拒。
            for (auto& kv : m_Pages)
            {
                if (kv.second.rec.is_leaf)
                    kv.second.resident = false;
            }

            uint64_t used = 0;
            for (auto& kv : m_Pages)
            {
                if (!kv.second.rec.is_leaf)
                    used += kv.second.rec.byte_size;  // 结构页占预算（永不驱逐）
            }

            for (uint32_t id : want_leafs)
            {
                Entry& e = m_Pages[id];
                if (used + e.rec.byte_size <= m_Budget)
                {
                    e.resident = true;
                    used += e.rec.byte_size;
                }
                // 装不下：保持 resident=false（被预算闸拒绝），不 break——后面可能有更近的
                // 小页能装下（want_leafs 已按距离排序，但字节大小不一，跳过大的留机会给小的）。
            }

            // 5) 预算仍超（结构页本身超预算的极端情况）：按 LRU 驱逐叶子页直到不超。
            if (used > m_Budget)
            {
                std::vector<uint32_t> resident_leafs;
                for (auto& kv : m_Pages)
                    if (kv.second.rec.is_leaf && kv.second.resident)
                        resident_leafs.push_back(kv.first);
                std::sort(resident_leafs.begin(), resident_leafs.end(), [&](uint32_t a, uint32_t b) {
                    return m_Pages[a].rec.last_used < m_Pages[b].rec.last_used;  // 最久未用先驱逐
                });
                for (uint32_t id : resident_leafs)
                {
                    if (used <= m_Budget)
                        break;
                    m_Pages[id].resident = false;
                    used -= m_Pages[id].rec.byte_size;
                }
            }

            // 6) 与上一帧快照比对，产出增量换入/换出队列；刷新快照与统计。
            PoolUpdate result;
            result.resident_bytes = 0;
            result.resident_count = 0;
            std::unordered_set<uint32_t> now_resident;
            for (auto& kv : m_Pages)
            {
                if (!kv.second.resident)
                    continue;
                now_resident.insert(kv.first);
                result.resident_bytes += kv.second.rec.byte_size;
                ++result.resident_count;
                if (m_PrevResident.count(kv.first) == 0 && kv.second.rec.is_leaf)
                    result.admitted.push_back(kv.first);
            }
            for (uint32_t prev : m_PrevResident)
            {
                if (now_resident.count(prev) == 0)
                {
                    auto it = m_Pages.find(prev);
                    if (it != m_Pages.end() && it->second.rec.is_leaf)
                        result.evicted.push_back(prev);
                }
            }
            m_PrevResident.swap(now_resident);
            m_ResidentBytes = result.resident_bytes;
            m_ResidentCount = result.resident_count;
            return result;
        }

    private:
        // 内部条目：调用方面向的 PageRecord + 池内驻留位（UpsertPage 不动 resident）。
        struct Entry
        {
            PageRecord rec;
            bool resident {false};
        };
        std::unordered_map<uint32_t, Entry> m_Pages;
        std::unordered_set<uint32_t> m_PrevResident;  // 上一帧 Update 结束时的驻留页集合
        uint64_t m_Budget {1};
        uint64_t m_ResidentBytes {0};
        size_t m_ResidentCount {0};
    };
}  // namespace nanite_pages
