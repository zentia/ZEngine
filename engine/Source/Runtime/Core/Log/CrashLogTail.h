#pragma once

// ============================================================================
// CrashLogTail — crash-safe ring buffer of the most recent log records
// ---------------------------------------------------------------------------
// 为什么需要它（PHASE1_FOUNDATION_PLAN.md #11）：
//   BqLog 是异步的，`log.reliable_level=normal`。崩溃时崩溃处理器调用
//   `force_flush_all_logs()` 把异步缓冲刷盘——但这是**尽力而为**：若异步工作线程
//   来不及排空（堆已损坏、线程被饿死、或溢出丢记录），日志的**尾部**就丢了，
//   而那恰恰是崩溃前最后、最该看到的诊断记录。
//
//   本模块在进程级静态存储里维护一个最近 N 条**已渲染**日志的环形缓冲，独立于
//   BqLog 的缓冲与 LogSystem 单例。它由日志线程**同步**写入（无锁 seqlock，崩溃
//   路径上不分配内存），崩溃处理器在 `force_flush_all_logs()` 之后把它 dump 成
//   crash_log_tail.txt——flush 失败时的兜底，也为 #2 的 POSIX 信号处理器铺日志后路。
//
// 设计要点：
//   * 无锁、多写者安全：每个槽用奇偶 seq 标记「写入中/已完成」，写者独占自己
//     的槽（全局自增 seq 保证不同写者落到不同槽），读者（崩溃路径）双重读 seq
//     检测撕裂写并跳过。全程无 mutex——崩溃路径绝不能因持锁线程已死而 deadlock。
//   * 崩溃路径零分配：Record() 只往静态数组里 memcpy 预渲染文本；Dump() 用 C stdio
//     低层写盘，容忍并发写（读到撕裂槽就跳过）。
//   * 独立于 BqLog / LogSystem：进程级静态存储，单例不存在也能记录/ dump。
//
// 依赖刻意保持极轻（仅 <atomic>/<cstdint>/<cstring>/<string>），**禁止**在此头
// 引入 EASTL / <format> / windows.h——它要被 LogSystem.h（重依赖）与 CrashHandler.h
// （header-only、依赖极轻）同时包含。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace zengine::logtail
{
    // 最近 N 条。256 * 512B = 128KB 静态存储，覆盖崩溃前足够长的上下文。
    static constexpr uint32_t kCapacity = 256;
    // 单条已渲染文本上限（含可能的截断省略号）。日志行过长时保留**开头**——
    // 开头通常是时间戳/类别/主谓，最有用。
    static constexpr uint32_t kMaxTextLen = 512;

    // 与 LogSystem::LogLevel 的声明顺序保持一致（verbose=0 .. fatal=5）。
    // 这里不 include LogSystem.h（会拖入 EASTL/<format>），故自持一份数值约定。
    static constexpr uint8_t kLevelVerbose = 0;
    static constexpr uint8_t kLevelDebug   = 1;
    static constexpr uint8_t kLevelInfo    = 2;
    static constexpr uint8_t kLevelWarning = 3;
    static constexpr uint8_t kLevelError   = 4;
    static constexpr uint8_t kLevelFatal   = 5;

    namespace detail
    {
        struct Entry
        {
            // 奇 = 写入中（读者须跳过）；偶 = 已完成、可读；0 = 从未写过。
            // 写者独占自己分配到的槽（全局自增 seq ⇒ 不同写者落到不同槽），
            // 因此同一槽的奇→偶转换只由它的唯一写者发起，天然无竞争。
            std::atomic<uint64_t> seq;
            uint8_t level;
            char text[kMaxTextLen];
        };

        // 进程级静态存储。不随 LogSystem 单例生命周期——崩溃时单例可能已不存在。
        inline Entry (&Entries())[kCapacity]
        {
            static Entry s_entries[kCapacity];
            return s_entries;
        }

        inline std::atomic<uint64_t>& GlobalSeq()
        {
            // 初值 2：记录序号按 +2 递增（0,2,4,...），完成标记 = 该偶数值。
            // 从 2 起保证**第一条**记录的完成标记是 2（非 0）——否则首条完成标记
            // 会落为 0，被 Dump 当成"从未写过"而丢弃。ClearForTest 归零后下一次
            // fetch_add(2) 又回到 2，行为一致。
            static std::atomic<uint64_t> s_seq{2};
            return s_seq;
        }
    }  // namespace detail

    // 记录一条已渲染的日志。由 LogSystem::log 在派发给 BqLog 后调用。
    // 无锁、无崩溃路径分配：只往静态数组 memcpy。text 会被拷贝，调用后可立即失效。
    // level 取 kLevel* 常量（与 LogSystem::LogLevel 顺序一致）。
    //
    // seqlock 语义：槽的 seq 字段奇 = 写入中（读者跳过），偶 = 已完成（可读）。
    // 关键：计数器**每次 +2**，使「写入中」= my|1（奇）、「完成」= my（偶）——两者
    // 恒异奇偶，读者据奇偶判定永不误判。若像最初那样 +1 且用 seq 本身作完成标记，
    // 奇数序号的完成标记仍是奇数，会被读者当成"仍在写"而永久丢弃（丢一半记录）。
    inline void Record(uint8_t level, const char* text)
    {
        if (text == nullptr)
        {
            text = "";
        }

        // 全局唯一偶数基号（0,2,4,...）。acq_rel 保证不同写者拿到互异基号。
        const uint64_t my = detail::GlobalSeq().fetch_add(2, std::memory_order_acq_rel);
        // 槽索引按记录序号 (my/2) 取模，用满全部 kCapacity 个槽（而非只用偶数槽）。
        const uint64_t idx = (my >> 1) % static_cast<uint64_t>(kCapacity);

        detail::Entry& e = detail::Entries()[idx];

        // 写入中：奇数（my|1）。release 把后续 level/text 写排在其后。
        e.seq.store(my | 1ULL, std::memory_order_release);
        e.level = level;

        uint32_t n = 0;
        while (n + 1 < kMaxTextLen && text[n] != '\0')
        {
            ++n;
        }
        std::memcpy(e.text, text, n);
        e.text[n] = '\0';

        // 完成：偶数（my）。读者见此偶数值即认为整条记录一致可读。
        e.seq.store(my, std::memory_order_release);
    }

    // 把环形缓冲 dump 成 <dir>/crash_log_tail.txt。由崩溃处理器在
    // force_flush_all_logs() 之后调用。用 C stdio 低层写盘，容忍并发写：
    // 读到撕裂（奇数 seq 或双重读不一致）的槽就跳过，绝不输出半条记录。
    // 返回写出的记录条数（不含头）。dir 为空则不落盘、返回 0。
    //
    // 定义在 CrashLogTail.cpp（非 inline）：dump 要走文件系统，实现集中一处，
    // 也让本头保持纯声明、被 CrashHandler.h 包含时无额外编译负担。
    uint32_t Dump(const std::string& dir);

    // 测试专用：清空所有槽（seq 归零），让冒烟测试能在干净的缓冲上隔离验证。
    // 生产代码从不调用它（真实崩溃时缓冲里就该有崩溃前的记录）。放在头里 inline
    // 以便测试 TU 直接调用，无需改 CrashLogTail.cpp。
    inline void ClearForTest()
    {
        for (detail::Entry& e : detail::Entries())
        {
            e.seq.store(0, std::memory_order_relaxed);
            e.level = 0;
            e.text[0] = '\0';
        }
        detail::GlobalSeq().store(2, std::memory_order_relaxed);
    }
}  // namespace zengine::logtail
