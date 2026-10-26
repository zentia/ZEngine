#include "Runtime/Core/Log/CrashLogTail.h"

#include <cstdio>
#include <string>

// ============================================================================
// CrashLogTail::Dump — 把环形缓冲落盘为 <dir>/crash_log_tail.txt
// ---------------------------------------------------------------------------
// 崩溃路径上调用（见 CrashHandler.h）。约束：
//   * 不用任何可能阻塞或需要健康堆的设施——只用 C stdio 的 fopen/fwrite/fflush。
//   * 容忍并发写：Record() 可能仍在别的线程往缓冲写；这里对每个槽做「双重读 seq」
//     校验——读到奇数 seq（写入中）或两次读不一致（撕裂）就跳过该槽，绝不输出
//     半条记录。崩溃路径上少记几条好过记了条坏的误导排查。
//   * 按槽的**逻辑顺序**（最旧 → 最新）输出，而非物理槽序，便于阅读。
// ============================================================================

namespace zengine::logtail
{
    namespace
    {
        const char* LevelName(uint8_t level)
        {
            switch (level)
            {
                case kLevelVerbose: return "VERBOSE";
                case kLevelDebug:   return "DEBUG";
                case kLevelInfo:    return "INFO";
                case kLevelWarning: return "WARNING";
                case kLevelError:   return "ERROR";
                case kLevelFatal:   return "FATAL";
                default:            return "UNKNOWN";
            }
        }

        // 安全读一个槽：成功返回 true 并写出 level/text；槽正在被写或已撕裂返回 false。
        // 用 acquire 读 seq：既要看到写者 release 发布的最新值，也要把自己后续的
        // level/text 读排在它之后（防止编译器/CPU 把读提到 seq 之前而读到旧内容）。
        bool ReadSlot(const detail::Entry& e, uint8_t& level, char* text, uint32_t textCap)
        {
            const uint64_t s1 = e.seq.load(std::memory_order_acquire);
            if ((s1 & 1ULL) != 0)   // 奇数 = 写入中
            {
                return false;
            }
            const uint8_t lv = e.level;
            char buf[kMaxTextLen];
            uint32_t n = 0;
            while (n + 1 < textCap && e.text[n] != '\0')
            {
                buf[n] = e.text[n];
                ++n;
            }
            buf[n] = '\0';
            // 二次校验：若读 text 期间写者又改写了该槽（seq 变了或变奇），放弃。
            const uint64_t s2 = e.seq.load(std::memory_order_acquire);
            if (s2 != s1 || (s2 & 1ULL) != 0)
            {
                return false;
            }
            std::memcpy(text, buf, n + 1);
            level = lv;
            return true;
        }
    }  // namespace

    uint32_t Dump(const std::string& dir)
    {
        if (dir.empty())
        {
            return 0;
        }

        std::string path = dir;
        const char last = path.empty() ? '\0' : path.back();
        if (last != '/' && last != '\\')
        {
            path += "/";
        }
        path += "crash_log_tail.txt";

        std::FILE* fp = std::fopen(path.c_str(), "w");
        if (fp == nullptr)
        {
            return 0;
        }

        // 收集有效槽，按逻辑序号（seq）升序输出：最旧的记录最先，最新的在最后。
        // 崩溃前最后几条通常最关键，放末尾最显眼。
        struct Item
        {
            uint64_t seq;
            uint8_t level;
            char text[kMaxTextLen];
        };
        Item items[kCapacity];
        uint32_t count = 0;

        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            const detail::Entry& e = detail::Entries()[i];
            const uint64_t s1 = e.seq.load(std::memory_order_acquire);
            if ((s1 & 1ULL) != 0 || s1 == 0)   // 写入中 或 从未写过
            {
                continue;
            }
            uint8_t lv = 0;
            char text[kMaxTextLen] = {};
            if (!ReadSlot(e, lv, text, kMaxTextLen))
            {
                continue;   // 撕裂，跳过
            }
            items[count].seq = s1;
            items[count].level = lv;
            std::memcpy(items[count].text, text, kMaxTextLen);
            ++count;
        }

        std::fprintf(fp,
                     "# ZEngine crash-log tail (last %u records, ring buffer fallback)\n"
                     "# Produced because force_flush_all_logs() may have lost the async\n"
                     "# log tail; this is the synchronous fallback written on the crash\n"
                     "# path. Records are the most recent %u pre-rendered log lines.\n",
                     count, count);

        // 简单插入排序（count <= 256，崩溃路径上足够快）按 seq 升序。
        for (uint32_t i = 1; i < count; ++i)
        {
            const Item key = items[i];
            uint32_t j = i;
            while (j > 0 && items[j - 1].seq > key.seq)
            {
                items[j] = items[j - 1];
                --j;
            }
            items[j] = key;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            std::fprintf(fp, "[%s] %s\n", LevelName(items[i].level), items[i].text);
        }

        std::fflush(fp);
        std::fclose(fp);
        return count;
    }
}  // namespace zengine::logtail
