// =============================================================================
// 固定时间步进（FixedUpdate）冒烟测试。
//
//   cmake -B build -DZENGINE_BUILD_FIXED_TIME_STEP_SMOKE_TEST=ON
//   cmake --build build --target FixedTimeStepSmokeTest
//   ctest --test-dir build -C Debug -R FixedTimeStepSmokeTest
//
// 为什么非要这个测试
// ------------------
// 固定步进的整个价值就一句话：**模拟推进多少，与帧率无关**。这句话成立与否
// 完全取决于一个累加器的算术，而算错了**没有任何症状** —— 物理照样跑、角色
// 照样动、编译照样绿。它只在"换台机器复现不了 / 联网对不上 / 回放对不上"的
// 时候才显形，而那时没人会怀疑到 TimeState 头上来。
//
// 所以这里钉的是：
//
//   B 组（最重要）  同一段墙钟时间用不同帧率喂进去，总步数必须一致。
//                   这是"与帧率解耦"的直接断言，也是整个特性的定义。
//   A 组           累加器的零头要跨帧攒着 —— 只断言"偏移为 0 的那条"分不出
//                   对错，所以每个用例都带非零头样本。
//   C 组           死亡螺旋闸门：一帧卡 10 秒不能真的去补 640 步。
//   D/E 组         timeScale 与暂停都必须作用在累加器上（暂停是"时间停了"，
//                   不是"攒着等恢复那一帧一起补"）。
//   F 组           配置夹取。0/负数步长会让 while 变死循环 —— 那是挂起，
//                   比返回错误糟糕得多，必须夹住。
//   G 组           Reset 归零时间但不冲掉配置。
//   H 组           源码级接线门：特性实现了但没接进主循环 = 没有特性。
//
// 浮点选择
// --------
// B 组用的是 2 的幂（1/32、1/64、1/128）：它们在二进制里是精确的，若干次加法
// 之后仍是精确的 1.0，于是"总步数 == 64"是**严格**断言。1/60 这类日常帧率不
// 精确，同一条断言只能给 ±1 容差 —— 两组都留着，前一组守正确性，后一组守
// "真实帧率下也不离谱"。
// =============================================================================

#include "Runtime/Core/Time/TimeState.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-5s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-5s %s\n", id, what);
        }
    }

    void CheckEq(long long got, long long want, const char* id, const char* what)
    {
        ++g_Checks;
        if (got == want)
        {
            std::printf("[ ok ] %-5s %s (=%lld)\n", id, what, got);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-5s %s got=%lld want=%lld\n", id, what, got, want);
        }
    }

    void CheckNear(float got, float want, float tol, const char* id, const char* what)
    {
        ++g_Checks;
        const bool ok = std::fabs(got - want) <= tol;
        if (ok)
        {
            std::printf("[ ok ] %-5s %s (=%.9g)\n", id, what, static_cast<double>(got));
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-5s %s got=%.9g want=%.9g tol=%g\n",
                        id, what, static_cast<double>(got), static_cast<double>(want),
                        static_cast<double>(tol));
        }
    }

    // 精确二进制值，见文件头的"浮点选择"。
    constexpr float k1_32  = 1.0f / 32.0f;   // 0.03125
    constexpr float k1_64  = 1.0f / 64.0f;   // 0.015625
    constexpr float k1_128 = 1.0f / 128.0f;  // 0.0078125

    /// 用固定步长推进 n 帧（每帧 frame_dt），返回累计固定步数。
    int RunFrames(TimeState& ts, int frame_count, float frame_dt)
    {
        int total = 0;
        for (int i = 0; i < frame_count; ++i)
        {
            ts.BeginFrame(frame_dt, frame_dt, true);
            total += ts.GetFixedStepCount();
        }
        return total;
    }
}

int main()
{
    // MSVC 的 stdout 在 CI 上是全缓冲的，崩在半路会丢掉全部已跑过的断言。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("\n=== FixedTimeStep smoke test ===\n\n");

    // -------------------------------------------------------------------------
    // A. 累加器基本行为（零头必须跨帧攒着）
    // -------------------------------------------------------------------------
    {
        std::printf("-- A. 累加器与零头 --\n");
        TimeState& ts = TimeState::Get();
        ts.Reset();
        ts.SetFixedDeltaTime(k1_64);
        ts.SetMaxSubsteps(5);

        // 一帧 1/32 = 两个固定步，正好吃光，零头为 0。
        ts.BeginFrame(k1_32, k1_32, true);
        CheckEq(ts.GetFixedStepCount(), 2, "A1", "1/32 帧 = 2 步（1/64 步长）");
        CheckNear(ts.GetFixedAccumulator(), 0.0f, 1e-9f, "A2", "零头被吃光");
        CheckNear(ts.GetFixedStepAlpha(), 0.0f, 1e-6f, "A3", "alpha = 0");

        // 一帧 1/128 不够一步：0 步，零头留着（**非零头样本** —— 只测整数倍
        // 帧的用例抓不到"累加器根本没生效"这种回归）。
        ts.BeginFrame(k1_128, k1_128, true);
        CheckEq(ts.GetFixedStepCount(), 0, "A4", "1/128 帧 < 1 步 => 0 步");
        CheckNear(ts.GetFixedAccumulator(), k1_128, 1e-9f, "A5", "零头留到下一帧");
        CheckNear(ts.GetFixedStepAlpha(), 0.5f, 1e-6f, "A6", "alpha = 0.5");

        // 再一帧 1/128 凑满一步。
        ts.BeginFrame(k1_128, k1_128, true);
        CheckEq(ts.GetFixedStepCount(), 1, "A7", "两帧 1/128 凑成 1 步");
        CheckNear(ts.GetFixedAccumulator(), 0.0f, 1e-9f, "A8", "零头归零");

        // GetFixedTime 必须与步数一致（不是帧时间）。
        CheckNear(ts.GetFixedTime(), 3.0f * k1_64, 1e-9f, "A9", "fixedTime = 步数 x 步长");
        CheckEq(ts.GetFixedStepTotal(), 3, "A10", "总步数累计正确");
    }

    // -------------------------------------------------------------------------
    // B. 与帧率解耦（本特性存在的理由）
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- B. 同一段墙钟时间，不同帧率 => 同样的总步数 --\n");

        // 严格组：二进制精确，1.0 秒 / (1/64) = 64 步。
        struct ExactCase { const char* name; int frames; float dt; };
        const ExactCase exact_cases[] = {
            {"32 fps  x 1/32",  32,  k1_32},
            {"64 fps  x 1/64",  64,  k1_64},
            {"128 fps x 1/128", 128, k1_128},
        };

        for (const ExactCase& c : exact_cases)
        {
            TimeState& ts = TimeState::Get();
            ts.Reset();
            ts.SetFixedDeltaTime(k1_64);
            ts.SetMaxSubsteps(8);
            const int steps = RunFrames(ts, c.frames, c.dt);
            CheckEq(steps, 64, "B1", c.name);
        }

        // 现实组：1/60、1/144 不精确，容差 ±1。
        // 单独看这一组是弱断言（±1 里能藏真 bug），所以上面那组严格断言是
        // 必须的 —— 这组只是确认真实帧率下不会离谱。
        struct RealCase { const char* name; int frames; };
        const RealCase real_cases[] = {
            {" 30 fps",  30},
            {" 60 fps",  60},
            {" 90 fps",  90},
            {"144 fps", 144},
            {"240 fps", 240},
        };

        int lo = 1 << 30;
        int hi = -1;
        for (const RealCase& c : real_cases)
        {
            TimeState& ts = TimeState::Get();
            ts.Reset();
            ts.SetFixedDeltaTime(k1_64);
            ts.SetMaxSubsteps(8);
            const int steps = RunFrames(ts, c.frames, 1.0f / static_cast<float>(c.frames));
            lo = steps < lo ? steps : lo;
            hi = steps > hi ? steps : hi;
            Check(std::abs(steps - 64) <= 1, "B2", c.name);
        }
        Check(hi - lo <= 1, "B3", "各帧率之间总步数相差不超过 1");
        std::printf("       (现实帧率下总步数区间 [%d, %d]，理论 64)\n", lo, hi);
    }

    // -------------------------------------------------------------------------
    // C. 死亡螺旋闸门
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- C. 单帧步数上限 --\n");
        TimeState& ts = TimeState::Get();
        ts.Reset();
        ts.SetFixedDeltaTime(k1_64);
        ts.SetMaxSubsteps(5);

        // 卡了 10 秒：理论上 640 步。不夹住就会"越补越慢"，帧率永久下降。
        ts.BeginFrame(10.0f, 10.0f, true);
        CheckEq(ts.GetFixedStepCount(), 5, "C1", "卡 10 秒只跑 5 步（上限）");
        CheckNear(ts.GetFixedAccumulator(), 0.0f, 1e-9f, "C2", "追不上时丢弃积压");

        // 丢完之后下一帧必须是干净的 —— 不能继续背着债。
        ts.BeginFrame(k1_64, k1_64, true);
        CheckEq(ts.GetFixedStepCount(), 1, "C3", "丢压之后下一帧正常走 1 步");
    }

    // -------------------------------------------------------------------------
    // D. timeScale 必须作用到累加器
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- D. timeScale --\n");
        TimeState& ts = TimeState::Get();
        ts.Reset();
        ts.SetFixedDeltaTime(k1_64);
        ts.SetMaxSubsteps(8);

        // 半速：1.0 秒墙钟只推进 0.5 秒逻辑时间 => 32 步。
        ts.SetTimeScale(0.5f);
        CheckEq(RunFrames(ts, 64, k1_64), 32, "D1", "半速 => 步数减半");

        // 冻结：一步都不跑，零头也不涨。
        ts.Reset();
        ts.SetFixedDeltaTime(k1_64);
        ts.SetTimeScale(0.0f);
        CheckEq(RunFrames(ts, 64, k1_64), 0, "D2", "timeScale=0 => 0 步");
        CheckNear(ts.GetFixedAccumulator(), 0.0f, 1e-9f, "D3", "timeScale=0 时零头不涨");

        ts.ResetTimeScale();
    }

    // -------------------------------------------------------------------------
    // E. 暂停（should_update_logic = false）
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- E. 编辑器暂停 --\n");
        TimeState& ts = TimeState::Get();
        ts.Reset();
        ts.SetFixedDeltaTime(k1_64);
        ts.SetMaxSubsteps(8);

        // 先攒一个非零头，才能验证"暂停没有把它冲掉也没让它继续涨"。
        ts.BeginFrame(k1_128, k1_128, true);
        CheckEq(ts.GetFixedStepCount(), 0, "E1", "暂停前攒下零头（0 步）");
        const float acc_before = ts.GetFixedAccumulator();
        const int total_before = ts.GetFixedStepTotal();

        // 暂停帧用 1/128 且取奇数帧：留**非零**余量。
        //
        // ⚠️ 用整数倍帧（比如 16 帧 × 1/32）时，即使累加器真的在暂停期间偷偷
        // 跑，走完也刚好回到余量 0 —— 断言"余量不变"照样绿。这是老坑：
        // 只断言"偏移为 0 的那条"分不出对错。奇数个半步能把这个洞堵上。
        for (int i = 0; i < 13; ++i)
        {
            ts.BeginFrame(k1_128, k1_128, false);
        }
        CheckEq(ts.GetFixedStepCount(), 0, "E2", "暂停期间 0 步");
        CheckEq(ts.GetFixedStepTotal(), total_before, "E3", "暂停期间总步数不变");
        CheckNear(ts.GetFixedAccumulator(), acc_before, 1e-9f, "E4",
                  "暂停不吞掉也不增长零头（恢复后不补帧）");

        // 恢复后接着走，而不是把暂停的墙钟一次性倾泻出来。
        ts.BeginFrame(k1_128, k1_128, true);
        CheckEq(ts.GetFixedStepCount(), 1, "E5", "恢复后只走该走的 1 步");
    }

    // -------------------------------------------------------------------------
    // F. 配置夹取（防死循环）
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- F. 配置夹取 --\n");
        TimeState& ts = TimeState::Get();
        ts.Reset();

        ts.SetFixedDeltaTime(0.0f);
        CheckNear(ts.GetFixedDeltaTime(), TimeState::kMinFixedDeltaTime, 0.0f, "F1",
                  "步长 0 被夹到下限（否则 while 死循环）");

        ts.SetFixedDeltaTime(-1.0f);
        CheckNear(ts.GetFixedDeltaTime(), TimeState::kMinFixedDeltaTime, 0.0f, "F2",
                  "负步长同样被夹");

        // 夹完之后必须真的还能跑，且不会爆步数。
        ts.SetMaxSubsteps(5);
        ts.BeginFrame(1.0f / TimeState::kMinFixedDeltaTime, 0.016f, true);
        CheckEq(ts.GetFixedStepCount(), 5, "F3", "最小步长下仍受上限约束");

        ts.SetMaxSubsteps(0);
        CheckEq(ts.GetMaxSubsteps(), TimeState::kMinSubstepsCap, "F4", "上限 0 被夹到 1");

        ts.SetMaxSubsteps(100000);
        CheckEq(ts.GetMaxSubsteps(), TimeState::kMaxSubstepsCap, "F5", "上限被夹到 64");
    }

    // -------------------------------------------------------------------------
    // G. Reset 语义：时间归零，配置不归零
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- G. Reset --\n");
        TimeState& ts = TimeState::Get();
        ts.Reset();
        ts.SetFixedDeltaTime(k1_64);
        ts.SetMaxSubsteps(3);
        RunFrames(ts, 10, k1_32);

        ts.Reset();
        CheckEq(ts.GetFixedStepTotal(), 0, "G1", "总步数归零");
        CheckNear(ts.GetFixedAccumulator(), 0.0f, 0.0f, "G2", "累加器归零");
        CheckNear(ts.GetFixedTime(), 0.0f, 0.0f, "G3", "fixedTime 归零");
        CheckNear(ts.GetFixedDeltaTime(), k1_64, 0.0f, "G4",
                  "步长是配置，不随 Reset 回默认");
        CheckEq(ts.GetMaxSubsteps(), 3, "G5", "上限是配置，不随 Reset 回默认");
    }

    // -------------------------------------------------------------------------
    // H. 接线门：实现了但没接进主循环 = 没有特性
    //
    // 上面 A~G 全绿只证明 TimeState 的算术对。真正决定"FixedUpdate 存在"的是
    // 三处接线，而它们全都是**能编译、能运行、静默失效**的形态：
    //   - LogicalTick 不按步数循环 => 固定步一次都不跑，没人报错。
    //   - 物理留在 Level::Tick 里   => FixedUpdate 照跑，但物理还是吃帧间隔，
    //                                 改了个寂寞。
    // 所以这里做源码级断言。
    // -------------------------------------------------------------------------
    {
        std::printf("\n-- H. 接线门（源码级） --\n");

        auto read_file = [](const std::string& path) -> std::string
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };

        const std::string root = ZENGINE_ENGINE_ROOT_DIR;
        const std::string app_src   = read_file(root + "/Source/Runtime/Application/Application.cpp");
        const std::string level_src = read_file(root + "/Source/Runtime/Function/Framework/Level/Level.cpp");

        // H1: LogicalTick 真的按 GetFixedStepCount() 循环分发。
        const size_t logical_tick = app_src.find("void Application::LogicalTick");
        Check(logical_tick != std::string::npos, "H1", "能定位 Application::LogicalTick");
        if (logical_tick != std::string::npos)
        {
            const std::string body = app_src.substr(logical_tick, 3000);
            Check(body.find("GetFixedStepCount()") != std::string::npos, "H2",
                  "LogicalTick 按固定步数循环");
            Check(body.find("FixedTick(") != std::string::npos, "H3",
                  "LogicalTick 调用 WorldManager::FixedTick");
            // 步长必须是固定步长，不能顺手把帧 delta 传下去。
            Check(body.find("GetFixedDeltaTime()") != std::string::npos, "H4",
                  "分发用的是固定步长而非帧 delta");
        }

        // H5/H6: 物理在 FixedTick 里、且**不在** Tick 里。
        const size_t fixed_pos = level_src.find("void Level::FixedTick");
        const size_t tick_pos  = level_src.find("void Level::Tick");
        Check(fixed_pos != std::string::npos && tick_pos != std::string::npos, "H5",
              "能定位 Level::Tick 与 Level::FixedTick");
        if (fixed_pos != std::string::npos && tick_pos != std::string::npos)
        {
            const std::string tick_body = level_src.substr(tick_pos, fixed_pos - tick_pos);
            const std::string fixed_body = level_src.substr(fixed_pos, 2500);
            Check(fixed_body.find("physics_scene->Tick") != std::string::npos, "H6",
                  "物理在 Level::FixedTick 里推进");
            Check(tick_body.find("physics_scene->Tick") == std::string::npos, "H7",
                  "物理已移出 Level::Tick（留着 => 改了个寂寞）");
        }
    }

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
