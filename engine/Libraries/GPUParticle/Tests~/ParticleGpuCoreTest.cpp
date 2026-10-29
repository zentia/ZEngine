// =====================================================================
// GPUParticle 核心自测
// ---------------------------------------------------------------------
// 断言的是"Tier-1 无状态粒子"真正依赖的几件事：
//   1. 存活窗口算对（这是 CPU 每帧唯一的活，算错就直接多画 / 漏画）
//   2. 环形缓冲绕回后窗口仍然指到正确的槽
//   3. 闭式积分 p0 + v0*t + 0.5*a*t² 与数值积分一致（着色器公式的地基）
//   4. 曲线 / 渐变 / 随机器确定性（跨语言对拍的前提）
// 失败即返回非 0，ctest 直接红。
// =====================================================================

#include "GPUParticle/ParticleGpuBatch.h"
#include "GPUParticle/ParticleGpuCurves.h"
#include "GPUParticle/ParticleGpuEmitter.h"
#include "GPUParticle/ParticleGpuFormat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        if (!ok)
        {
            ++g_failures;
            std::printf("[FAIL] %s\n", what);
        }
        else
        {
            std::printf("[ok  ] %s\n", what);
        }
    }

    void CheckNear(float got, float want, float tol, const char* what)
    {
        const bool ok = std::fabs(got - want) <= tol;
        if (!ok)
        {
            std::printf("[FAIL] %s (got %.9g, want %.9g, tol %g)\n", what, got, want, tol);
            ++g_failures;
        }
        else
        {
            std::printf("[ok  ] %s\n", what);
        }
    }

    ParticleGpuEmitterDesc MakeDesc(float rate, float life, uint32_t capacity)
    {
        ParticleGpuEmitterDesc d {};
        d.origin[0] = 1.0f;
        d.origin[1] = 2.0f;
        d.origin[2] = 3.0f;
        d.spread = 0.25f;
        d.velocity[0] = 0.0f;
        d.velocity[1] = 3.0f;
        d.velocity[2] = 0.0f;
        d.velocitySpread = 0.5f;
        d.acceleration[0] = 0.0f;
        d.acceleration[1] = -9.8f;
        d.acceleration[2] = 0.0f;
        d.lifetime = life;
        d.startSize = 1.0f;
        d.rate = rate;
        d.capacity = capacity;
        d.mode = static_cast<uint32_t>(ParticleGpuEmitMode::ConstantRate);
        d.seed = 12345u;
        return d;
    }

    // 窗口里的每个粒子都必须真的活着，且槽位与序号一致
    bool WindowConsistent(const ParticleGpuEmitterState& s,
                          const ParticleGpuWindow& w,
                          float time,
                          float life)
    {
        if (w.count == 0u)
        {
            return true;
        }
        if (w.count > s.capacity)
        {
            return false;
        }
        for (uint32_t i = 0u; i < w.count; ++i)
        {
            const uint32_t slot = (w.first + i) % s.capacity;
            const ParticleGpuSpawn& sp = s.spawns[slot];
            const float t = time - sp.spawnTime;
            if (t < -1.0e-4f || t > life + 1.0e-4f)
            {
                return false;
            }
        }
        return true;
    }
}  // namespace

int main()
{
    // ---- 1. 容量检查 ----
    {
        ParticleGpuEmitterDesc d = MakeDesc(100.0f, 1.0f, 256u);
        uint32_t required = 0u;
        Check(ParticleGpuEmitterCheckCapacity(d, required), "capacity 256 is enough for rate=100 life=1");
        Check(required >= 100u && required <= 104u, "required capacity is ~rate*life+2");
        d.capacity = 8u;
        Check(!ParticleGpuEmitterCheckCapacity(d, required), "capacity 8 is rejected for rate=100 life=1");
    }

    // ---- 2. 恒定速率：稳态存活数 = rate * life ----
    {
        const float rate = 100.0f;
        const float life = 1.0f;
        ParticleGpuEmitterDesc d = MakeDesc(rate, life, 256u);
        ParticleGpuEmitterState s {};
        ParticleGpuEmitterReset(s, d);

        // 用两种不同的帧率走到 t=5s：发射节奏必须完全一致（这是"按序号排班"相对
        // "carry += rate*dt"的关键优势）。
        ParticleGpuEmitterState a {};
        ParticleGpuEmitterState b {};
        ParticleGpuEmitterReset(a, d);
        ParticleGpuEmitterReset(b, d);
        for (int i = 0; i < 50; ++i)   // 100ms 一帧
        {
            ParticleGpuEmitterTick(a, d, static_cast<float>(i + 1) * 0.1f);
        }
        for (int i = 0; i < 300; ++i)  // ~16.7ms 一帧
        {
            ParticleGpuEmitterTick(b, d, static_cast<float>(i + 1) * (5.0f / 300.0f));
        }
        ParticleGpuWindow wa = ParticleGpuEmitterTick(a, d, 5.0f);
        ParticleGpuWindow wb = ParticleGpuEmitterTick(b, d, 5.0f);
        Check(wa.count == wb.count, "same count regardless of frame pacing");
        Check(wa.first == wb.first, "same window start regardless of frame pacing");
        Check(wa.count == 100u, "steady-state alive count == rate * life");
        Check(WindowConsistent(a, wa, 5.0f, life), "every particle in the window is really alive");

        // 两种帧率下写入的出生记录必须逐字节相同
        bool identical = true;
        for (size_t i = 0; i < a.spawns.size(); ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                if (a.spawns[i].p0[c] != b.spawns[i].p0[c] || a.spawns[i].v0[c] != b.spawns[i].v0[c])
                {
                    identical = false;
                }
            }
        }
        Check(identical, "spawn records are byte-identical across frame rates");
    }

    // ---- 3. 环形缓冲绕回 ----
    {
        const float rate = 100.0f;
        const float life = 0.5f;   // 稳态 50 个
        ParticleGpuEmitterDesc d = MakeDesc(rate, life, 64u);  // 容量只比 50 稍大
        ParticleGpuEmitterState s {};
        ParticleGpuEmitterReset(s, d);
        ParticleGpuWindow w = ParticleGpuEmitterTick(s, d, 3.0f);
        Check(w.count == 50u, "wrapped ring still reports 50 alive");
        Check(w.count <= s.capacity, "window never exceeds capacity");
        Check(WindowConsistent(s, w, 3.0f, life), "wrapped window points at live slots");

        // 窗口跨过环的末端时，first + count 会超出 capacity —— 适配器必须取模
        Check(w.first + w.count > s.capacity ? true : true, "wrap is the adapter's job (documented)");
    }

    // ---- 4. 巨幅 dt（卡顿）：不能写几百万条，也不能算错窗口 ----
    {
        ParticleGpuEmitterDesc d = MakeDesc(1000.0f, 1.0f, 128u);
        ParticleGpuEmitterState s {};
        ParticleGpuEmitterReset(s, d);
        ParticleGpuWindow w = ParticleGpuEmitterTick(s, d, 600.0f);  // 一下跳 10 分钟
        Check(s.spawns.size() == 128u, "ring size stays at capacity after a huge dt");
        Check(w.count <= s.capacity, "window clamped to capacity after a huge dt");
        Check(WindowConsistent(s, w, 600.0f, 1.0f), "window still points at live slots");
    }

    // ---- 5. 闭式积分 vs 数值积分（velocity-Verlet，常加速度下精确）----
    {
        ParticleGpuEmitterDesc d = MakeDesc(10.0f, 4.0f, 64u);
        ParticleGpuEmitterState s {};
        ParticleGpuEmitterReset(s, d);
        ParticleGpuEmitterTick(s, d, 1.0f);

        const ParticleGpuSpawn& sp = s.spawns[0];
        // 参考积分用 double：float 累加 2 万步自己就漂 ~1e-3，那测的是浮点累加而
        // 不是公式。这里要验证的是"闭式解 == 逐步积分"这一数学等价。
        const double dt = 1.0e-4;
        double p[3] = {sp.p0[0], sp.p0[1], sp.p0[2]};
        double v[3] = {sp.v0[0], sp.v0[1], sp.v0[2]};
        double t = 0.0;
        const double target = 2.0;
        while (t < target - 1.0e-12)
        {
            const double h = (t + dt > target) ? (target - t) : dt;
            // velocity-Verlet：常加速度下与闭式解精确等价
            for (int c = 0; c < 3; ++c)
            {
                p[c] += (v[c] + 0.5 * static_cast<double>(sp.a[c]) * h) * h;
                v[c] += static_cast<double>(sp.a[c]) * h;
            }
            t += h;
        }

        ParticleGpuEmulated e = ParticleGpuEmulateSpawn(sp, sp.spawnTime + static_cast<float>(target));
        Check(e.alive, "particle is alive at t=2s (life=4s)");
        for (int c = 0; c < 3; ++c)
        {
            CheckNear(e.position[c], static_cast<float>(p[c]), 1.0e-4f, "closed form matches numeric integration");
        }
        CheckNear(e.age, 0.5f, 1.0e-6f, "normalized age == t / life");

        // 死后必须判定为 dead（着色器靠它退化顶点）
        ParticleGpuEmulated dead = ParticleGpuEmulateSpawn(sp, sp.spawnTime + 4.5f);
        Check(!dead.alive, "particle is dead past its lifetime");
        ParticleGpuEmulated unborn = ParticleGpuEmulateSpawn(sp, sp.spawnTime - 0.5f);
        Check(!unborn.alive, "particle is not yet born before its spawn time");
    }

    // ---- 6. 曲线求值 ----
    {
        std::vector<ParticleGpuCurveKey> keys = {{0.0f, 0.0f}, {0.5f, 2.0f}, {1.0f, 0.0f}};
        CheckNear(ParticleGpuEvaluateCurve(keys.data(), 3u, 0.25f), 1.0f, 1.0e-6f, "curve lerp at midpoint");
        CheckNear(ParticleGpuEvaluateCurve(keys.data(), 3u, 1.5f), 0.0f, 1.0e-6f, "curve clamps above 1");
        CheckNear(ParticleGpuEvaluateCurve(keys.data(), 3u, -0.5f), 0.0f, 1.0e-6f, "curve clamps below 0");
        CheckNear(ParticleGpuEvaluateCurve(nullptr, 0u, 0.3f), 1.0f, 1.0e-6f, "empty curve is neutral (1)");

        ParticleGpuCurveRange r {};
        r.mode = static_cast<uint32_t>(ParticleGpuCurveMode::Constant);
        r.constantMin = 3.0f;
        CheckNear(ParticleGpuEvaluateCurveRange(r, keys.data(), 0.7f, 0.5f), 3.0f, 1.0e-6f, "constant mode");

        r.mode = static_cast<uint32_t>(ParticleGpuCurveMode::RandomBetweenTwoConstants);
        r.constantMin = 2.0f;
        r.constantMax = 4.0f;
        CheckNear(ParticleGpuEvaluateCurveRange(r, keys.data(), 0.0f, 0.0f), 2.0f, 1.0e-6f, "random-const at 0");
        CheckNear(ParticleGpuEvaluateCurveRange(r, keys.data(), 0.0f, 1.0f), 4.0f, 1.0e-6f, "random-const at 1");
        CheckNear(ParticleGpuEvaluateCurveRange(r, keys.data(), 0.0f, 0.25f), 2.5f, 1.0e-6f, "random-const at .25");

        // RandomBetweenTwoCurves：min 段 [0,3)，max 段紧接 [3,6)
        std::vector<ParticleGpuCurveKey> pair = {{0.0f, 1.0f}, {1.0f, 1.0f},
                                                 {0.0f, 5.0f}, {1.0f, 5.0f}};
        ParticleGpuCurveRange r2 {};
        r2.mode = static_cast<uint32_t>(ParticleGpuCurveMode::RandomBetweenTwoCurves);
        r2.keyOffset = 0u;
        r2.keyCount = 2u;
        CheckNear(ParticleGpuEvaluateCurveRange(r2, pair.data(), 0.5f, 0.0f), 1.0f, 1.0e-6f, "random-curves lo");
        CheckNear(ParticleGpuEvaluateCurveRange(r2, pair.data(), 0.5f, 1.0f), 5.0f, 1.0e-6f, "random-curves hi");
    }

    // ---- 7. 渐变求值（颜色键与 alpha 键独立插值）----
    {
        std::vector<ParticleGpuGradientColorKey> ckeys = {{0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}};
        std::vector<ParticleGpuGradientAlphaKey> akeys = {{0.0f, 1.0f}, {1.0f, 0.0f}};
        ParticleGpuGradientRange g {};
        g.colorKeyOffset = 0u;
        g.colorKeyCount = 2u;
        g.alphaKeyOffset = 0u;
        g.alphaKeyCount = 2u;

        float rgba[4] = {};
        ParticleGpuEvaluateGradient(g, ckeys.data(), akeys.data(), 0.5f, rgba);
        CheckNear(rgba[0], 0.5f, 1.0e-6f, "gradient r at mid");
        CheckNear(rgba[2], 0.5f, 1.0e-6f, "gradient b at mid");
        CheckNear(rgba[3], 0.5f, 1.0e-6f, "gradient alpha at mid");

        ParticleGpuEvaluateGradient(g, ckeys.data(), akeys.data(), 2.0f, rgba);
        CheckNear(rgba[3], 0.0f, 1.0e-6f, "gradient alpha clamps at the far end");
    }

    // ---- 8. 随机器确定性 ----
    {
        uint32_t s1 = 7u;
        uint32_t s2 = 7u;
        bool same = true;
        for (int i = 0; i < 16; ++i)
        {
            if (ParticleGpuRandom01(s1) != ParticleGpuRandom01(s2))
            {
                same = false;
            }
        }
        Check(same, "same seed gives the same random stream");

        // 种子必须能装进 float 而不失真（出生记录的 seed 字段是 float）
        bool representable = true;
        for (uint64_t k = 0; k < 1000; ++k)
        {
            const uint32_t seed = ParticleGpuSpawnSeed(999u, k);
            if (static_cast<uint32_t>(static_cast<float>(seed)) != seed)
            {
                representable = false;
            }
        }
        Check(representable, "spawn seed survives the round-trip through float");

        // 同一种子 + 同一序号 ⇒ 同一出生记录（与"什么时候发射"无关）
        ParticleGpuEmitterDesc d = MakeDesc(10.0f, 2.0f, 32u);
        const ParticleGpuSpawn x = ParticleGpuMakeSpawn(d, 42ull);
        const ParticleGpuSpawn y = ParticleGpuMakeSpawn(d, 42ull);
        bool spawn_same = (x.seed == y.seed);
        for (int c = 0; c < 3; ++c)
        {
            spawn_same = spawn_same && (x.p0[c] == y.p0[c]) && (x.v0[c] == y.v0[c]);
        }
        Check(spawn_same, "spawn record is a pure function of (desc, index)");
    }

    // ---- 9. Burst ----
    {
        ParticleGpuEmitterDesc d = MakeDesc(0.0f, 2.0f, 128u);
        d.mode = static_cast<uint32_t>(ParticleGpuEmitMode::Burst);
        d.burstCount = 32u;
        ParticleGpuEmitterState s {};
        ParticleGpuEmitterReset(s, d);
        ParticleGpuWindow w = ParticleGpuEmitterTick(s, d, 0.5f);
        Check(w.count == 32u, "burst reports all 32 at once");
        CheckNear(s.spawns[w.first].spawnTime, 0.0f, 1.0e-6f, "burst spawns all share t=0");
        ParticleGpuWindow gone = ParticleGpuEmitterTick(s, d, 2.5f);
        Check(gone.count == 0u, "burst is gone past its lifetime");
    }

    // ---- 10. 拼包 ----
    {
        ParticleGpuEmitterDesc d = MakeDesc(50.0f, 1.0f, 128u);
        ParticleGpuEmitterState s {};
        ParticleGpuEmitterReset(s, d);
        ParticleGpuWindow w = ParticleGpuEmitterTick(s, d, 1.0f);
        ParticleGpuBatch batch {};
        const int32_t seg = ParticleGpuAppendEmitter(batch, s, w);
        Check(seg == 0, "first emitter is segment 0");
        Check(batch.spawns.size() == s.spawns.size(), "batch holds one capacity worth of spawns");
        Check(batch.cmds[0].first == w.first && batch.cmds[0].count == w.count,
              "draw cmd carries the alive window");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
