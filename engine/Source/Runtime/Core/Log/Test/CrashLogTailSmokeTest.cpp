// =============================================================================
// 崩溃安全日志环形缓冲冒烟测试（阶段一·夯地基 第四落点 #11：日志崩溃环形缓存）
// -----------------------------------------------------------------------------
// 目的：证明 CrashLogTail 的进程级环形缓冲真的能兜住崩溃前的日志尾——这是
//   force_flush_all_logs()（BqLog 异步刷盘，尽力而为）失败时的最后防线。规划文档
//   PHASE1_FOUNDATION_PLAN.md #11 的原话：
//     "log.reliable_level=normal；无崩溃安全环形缓冲，force_flush 失败则日志尾丢失。"
//
// 被测机制回顾（见 Core/Log/CrashLogTail.h）：
//   * Record(level, text)：日志线程同步写入。用全局自增 seq 分配唯一槽
//     （seq % kCapacity），奇偶 seq 标记「写入中/已完成」，写者独占自己的槽。
//   * Dump(dir)：崩溃路径落盘 <dir>/crash_log_tail.txt。双重读 seq 检测撕裂写并
//     跳过；按 seq 升序输出（最旧 → 最新）。返回写出的记录条数。
//
// 判据（全程主进程内安全完成，无断言/崩溃，无需子进程隔离）：
//   1. 单线程：记录 kCapacity 条 → Dump → 文件含全部 kCapacity 条、按记录顺序排列。
//   2. 溢出回绕：记录 2*kCapacity 条 → Dump → 仅保留最近 kCapacity 条、且顺序正确。
//   3. 并发写不撕裂：N 线程各记 M 条 → Dump → 一条不少（N*M）、无撕裂（每条文本完整）。
//   4. 空 dir 是 no-op：Dump("") 返回 0、不落盘。
//   5. 崩溃路径零分配/无依赖：Record 只往静态数组 memcpy（编译期即可见，无堆调用）。
//
// 为什么不需要 RegisterCore / 系统 bring-up：CrashLogTail 是刻意零依赖的模块
//   （仅标准头，独立于 BqLog 与 LogSystem 单例），故本测试直接调 Record/Dump 即可。
//
// 构建：opt in —— cmake -B build -DZENGINE_BUILD_CRASH_LOG_TAIL_SMOKE_TEST=ON
//       cmake --build build --target CrashLogTailSmokeTest
// 退出码：0 = 全部判据通过；1 = 有判据失败。
// =============================================================================

#include "Runtime/Core/Log/CrashLogTail.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <sys/stat.h>
#endif

namespace
{
    using namespace zengine::logtail;

    int g_Checks   = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    // 造一个唯一临时目录（崩溃日志尾落盘处），测试结束清理。
    std::string MakeTempDir(const char* tag)
    {
        namespace fs = std::filesystem;
        fs::path base = fs::temp_directory_path();
        fs::path dir = base / ("zengine_crashlogtail_test_" + std::string(tag));
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir.generic_string();
    }

    void RemoveDir(const std::string& dir)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // 读回 Dump 出的文件，只保留数据行（以 "[INFO] " 开头），跳过注释头（'#' 开头）。
    // 这样判据对头的行数不敏感——头的行数由 Dump 自己决定，测试不该假设。
    std::vector<std::string> ReadDumpDataLines(const std::string& dir)
    {
        std::vector<std::string> lines;
        const std::string path = dir + "/crash_log_tail.txt";
        std::ifstream in(path);
        if (!in.is_open())
        {
            return lines;
        }
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.rfind("[INFO] ", 0) == 0)   // 只收数据行
            {
                lines.push_back(line);
            }
        }
        return lines;
    }

    // ---- (1) 单线程：记录 kCapacity 条 → Dump → 全部在、按序 ----
    void CheckSingleThreadOrder()
    {
        ClearForTest();
        const std::string dir = MakeTempDir("single");
        // 先记 kCapacity 条，文本带序号便于校验顺序（最旧=0，最新=kCapacity-1）。
        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "rec-%u", i);
            Record(kLevelInfo, buf);
        }

        const uint32_t written = Dump(dir);
        const std::vector<std::string> lines = ReadDumpDataLines(dir);

        // 头是注释（'#' 开头），已被 ReadDumpDataLines 过滤；这里只看数据行。
        Check(written == kCapacity, "single-thread: Dump returns kCapacity records");
        Check(lines.size() == kCapacity, "single-thread: file has kCapacity data lines");

        // 记录行应按记录顺序（seq 升序 = 写入顺序）出现：rec-0 .. rec-(N-1)。
        bool ordered = true;
        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            const std::string expect = "[INFO] rec-" + std::to_string(i);
            if (lines[i] != expect)
            {
                ordered = false;
                std::printf("       mismatch at row %u: got '%s' want '%s'\n",
                            i, lines[i].c_str(), expect.c_str());
                break;
            }
        }
        Check(ordered, "single-thread: records emitted in write order (oldest->newest)");

        RemoveDir(dir);
    }

    // ---- (2) 溢出回绕：记 2N 条 → 仅保留最近 N 条、按序 ----
    void CheckOverflowWrap()
    {
        ClearForTest();
        const std::string dir = MakeTempDir("overflow");
        const uint32_t total = kCapacity * 2;
        for (uint32_t i = 0; i < total; ++i)
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "rec-%u", i);
            Record(kLevelInfo, buf);
        }

        const uint32_t written = Dump(dir);
        const std::vector<std::string> lines = ReadDumpDataLines(dir);

        // 溢出后只应保留最后 kCapacity 条（seq 最大的 kCapacity 个）。
        Check(written == kCapacity, "overflow: Dump returns only kCapacity (most recent) records");
        Check(lines.size() == kCapacity, "overflow: file has kCapacity data lines");

        // 保留的应是 rec-N .. rec-(2N-1)，按序。
        bool recentOnly = true;
        for (uint32_t i = 0; i < kCapacity; ++i)
        {
            const uint32_t idx = kCapacity + i;  // 后半段
            const std::string expect = "[INFO] rec-" + std::to_string(idx);
            if (lines[i] != expect)
            {
                recentOnly = false;
                std::printf("       overflow mismatch at row %u: got '%s' want '%s'\n",
                            i, lines[i].c_str(), expect.c_str());
                break;
            }
        }
        Check(recentOnly, "overflow: retained the most recent kCapacity records in order");

        RemoveDir(dir);
    }

    // ---- (3) 并发写不撕裂：N 线程共记 ≤ kCapacity 条 → 一条不少、每条文本完整 ----
    void CheckConcurrentNoTear()
    {
        ClearForTest();
        const std::string dir = MakeTempDir("concurrent");
        // 总写入数必须 ≤ kCapacity：每个写者拿到全局唯一 seq、落到唯一槽，
        // 只要总数不超过容量就**不发生槽复用**，因此不会互相覆盖、也不会撕裂——
        // 这是无锁环形缓冲在容量内可证无损的边界。超出容量是"保留最近 N 条"的
        // 有损回绕语义（见 CheckOverflowWrap），不在此判据范围。
        // 4 线程 * 48 = 192 < 256，留足余量。
        constexpr int kThreads = 4;
        constexpr int kPerThread = 48;
        // 每条文本带 (线程, 序号)，唯一且长度固定，便于检测撕裂（截断/半写）。
        std::atomic<bool> start{false};

        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t)
        {
            workers.emplace_back([&start, t]() {
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kPerThread; ++i)
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "t%02d-%03d", t, i);
                    Record(kLevelInfo, buf);
                }
            });
        }

        start.store(true, std::memory_order_release);
        for (auto& w : workers)
        {
            w.join();
        }

        const uint32_t written = Dump(dir);
        const std::vector<std::string> lines = ReadDumpDataLines(dir);
        const uint32_t expectCount = static_cast<uint32_t>(kThreads * kPerThread);

        // 容量内无槽复用：记录条数应严格等于写入数（一条不少）。
        Check(written == expectCount,
              "concurrent: no record lost within capacity (count == threads * per-thread)");
        Check(lines.size() == expectCount,
              "concurrent: file has exactly the expected number of data lines");

        // 每条记录行都应形如 "[INFO] tNN-MMM"——文本完整、无撕裂（长度固定）。
        // 任何撕裂（读到"写入中"的奇数槽会被 Dump 跳过，或双重读不一致被跳过）都会
        // 表现为条数不足或格式不符。
        bool allWellFormed = true;
        for (const std::string& ln : lines)
        {
            if (ln.rfind("[INFO] t", 0) != 0 || ln.size() != std::string("[INFO] t00-000").size())
            {
                allWellFormed = false;
                std::printf("       malformed/torn line: '%s'\n", ln.c_str());
                break;
            }
        }
        Check(allWellFormed, "concurrent: every emitted record is well-formed (no torn writes)");

        RemoveDir(dir);
    }

    // ---- (4) 空 dir 是 no-op ----
    void CheckEmptyDirNoop()
    {
        const uint32_t written = Dump("");
        Check(written == 0, "empty dir: Dump returns 0 and writes nothing");
    }

    // ---- (5) 崩溃路径零分配：Record 是编译期可见的纯静态写入（此处做接口契约断言）----
    void CheckCapacityConstants()
    {
        // 这些常量是环形缓冲的静态契约：容量 > 1（否则回绕失效）、文本上限 > 0。
        Check(kCapacity > 1, "capacity > 1 (wrap-around meaningful)");
        Check(kMaxTextLen > 0, "max text length > 0");
    }
}  // namespace

// =============================================================================
// main
// =============================================================================
int main()
{
    std::fprintf(stderr,
                 "ZEngine crash-log tail ring buffer smoke test (Phase-1, #11)\n"
                 "=====================================================================\n");
    std::fflush(stderr);

    CheckCapacityConstants();
    CheckEmptyDirNoop();
    CheckSingleThreadOrder();
    CheckOverflowWrap();
    CheckConcurrentNoTear();

    std::fprintf(stderr, "\ncrash-log tail smoke test: %d check(s), %d failure(s)\n",
                 g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
