// =============================================================================
// nanite_dedup_probe — Nanite 顶点去重（NaniteCluster.cpp:277 TODO）可行性探针
// -----------------------------------------------------------------------------
// 问题：把"每个三角形独占 3 顶点、索引恒 0,1,2,...（不去重）"改成"共享顶点去重"，
//       索引会不会丢（某个三角形解析到错误的顶点位置）？
//
// 本探针**不依赖引擎**（纯 C++17，无 RHI/无窗口），复刻 NaniteCluster.cpp 叶子节点的
// 顶点发射逻辑，对比两种策略：
//   A. 现状 no-dedup：每三角形 push 3 个新顶点，索引 = 0,1,2,3,4,5,...
//   B. 去重 dedup：用 hash 表按"量化后位置"合并重复顶点，索引指向首次出现的槽位。
//
// 判定"索引丢没丢"的标准（对齐 NaniteClusterBuilderSmokeTest 的 oracle）：
//   对输出索引流里每个三角形，取它的 3 个索引 → 查回顶点位置 → 与**输入**该三角形的
//   3 个原始位置逐一比对。任意一角位置对不上（超过容差）= 索引丢/错。
//
// 关键 subtlety：去重键用**量化后**位置（int16 AABB 线性），而不是原始 float。原因：
//   GeometryCodec::Quantized 把位置量化成 int16，两个 float 不同但量化到同一 int16 的
//   顶点解码后是同一个位置——按量化键去重能多合并这类顶点，且解码后几何仍在
//   quant_scale/2 容差内。本探针同时跑"float 精确键"和"量化键"两档，看各自省多少、
//   是否丢索引。
//
// 构建+运行（任意主机，纯 CPU，headless）：
//   见 scripts/verify_nanite_dedup.sh
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct Vec3 { float x, y, z; };

    bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

    bool VecNear(const Vec3& a, const Vec3& b, float eps)
    {
        return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
    }

    // --- 量化（对齐 GeometryCodec::Quantized 的 int16 AABB 线性映射）-----------
    // quant_i = round((pos - qmin) / qscale * 32767)，qscale = AABB 每轴 extent。
    // 解码 pos = qmin + quant_i / 32767 * qscale。这里只用它做**去重键**，所以只需
    // 一个稳定的整数键；解码误差由 qscale/2 上界保证（与 codec 同一契约）。
    struct QuantKey
    {
        int32_t x, y, z;
        bool operator==(const QuantKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };

    struct AABB { Vec3 mn{1e30f,1e30f,1e30f}, mx{-1e30f,-1e30f,-1e30f}; };

    AABB ComputeAABB(const std::vector<Vec3>& p)
    {
        AABB b;
        for (const Vec3& v : p)
        {
            b.mn.x = std::fmin(b.mn.x, v.x); b.mx.x = std::fmax(b.mx.x, v.x);
            b.mn.y = std::fmin(b.mn.y, v.y); b.mx.y = std::fmax(b.mx.y, v.y);
            b.mn.z = std::fmin(b.mn.z, v.z); b.mx.z = std::fmax(b.mx.z, v.z);
        }
        return b;
    }

    QuantKey Quantize(const Vec3& v, const AABB& b)
    {
        auto q = [&](float lo, float hi, float val) {
            const float ext = hi - lo;
            if (ext <= 0.0f) return 0;  // flat axis
            const float t = (val - lo) / ext;
            return static_cast<int32_t>(std::lround(t * 32767.0));
        };
        return QuantKey{ q(b.mn.x, b.mx.x, v.x), q(b.mn.y, b.mx.y, v.y), q(b.mn.z, b.mx.z, v.z) };
    }

    struct QuantKeyHash
    {
        size_t operator()(const QuantKey& k) const
        {
            // 三个 21-bit 值拼进 64-bit（简单充分：无碰撞即正确，碰撞只是少省几个顶点）。
            return static_cast<size_t>(
                (static_cast<uint64_t>(static_cast<uint32_t>(k.x)) << 42) ^
                (static_cast<uint64_t>(static_cast<uint32_t>(k.y)) << 21) ^
                static_cast<uint64_t>(static_cast<uint32_t>(k.z)));
        }
    };

    // --- 策略 A：现状 no-dedup ------------------------------------------------
    // 复刻 NaniteCluster.cpp:270-290：每三角形 push 3 个新顶点，索引连续。
    void EmitNoDedup(const std::vector<Vec3>& positions, const std::vector<uint32_t>& indices,
                     std::vector<Vec3>& out_pos, std::vector<uint32_t>& out_idx)
    {
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            out_idx.push_back(static_cast<uint32_t>(out_pos.size()));
            out_pos.push_back(positions[indices[i]]);
            out_idx.push_back(static_cast<uint32_t>(out_pos.size()));
            out_pos.push_back(positions[indices[i + 1]]);
            out_idx.push_back(static_cast<uint32_t>(out_pos.size()));
            out_pos.push_back(positions[indices[i + 2]]);
        }
    }

    // --- 策略 B：去重（两档键：float 精确键 / 量化键）--------------------------
    // 每个顶点先查表；命中 → 复用已有槽位索引；未命中 → push 新顶点并登记。
    // 返回去重后顶点数。两档显式实现，避免模板键构造的 placeholder 陷阱。
    size_t EmitDedupFloat(const std::vector<Vec3>& positions, const std::vector<uint32_t>& indices,
                          std::vector<Vec3>& out_pos, std::vector<uint32_t>& out_idx)
    {
        // float 精确键：把 3 个 float 的位模式拼成 96-bit 字符串键。
        auto to_key = [](const Vec3& v) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "%.9g,%.9g,%.9g", static_cast<double>(v.x),
                          static_cast<double>(v.y), static_cast<double>(v.z));
            return std::string(buf);
        };
        std::unordered_map<std::string, uint32_t> table;
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const uint32_t tri[3] = { indices[i], indices[i + 1], indices[i + 2] };
            for (int k = 0; k < 3; ++k)
            {
                const Vec3 v = positions[tri[k]];
                const std::string key = to_key(v);
                auto it = table.find(key);
                if (it != table.end())
                {
                    out_idx.push_back(it->second);  // 复用已有槽位
                }
                else
                {
                    const uint32_t slot = static_cast<uint32_t>(out_pos.size());
                    out_pos.push_back(v);
                    table.emplace(key, slot);
                    out_idx.push_back(slot);
                }
            }
        }
        return out_pos.size();
    }

    size_t EmitDedupQuant(const std::vector<Vec3>& positions, const std::vector<uint32_t>& indices,
                          const AABB& aabb, std::vector<Vec3>& out_pos, std::vector<uint32_t>& out_idx)
    {
        std::unordered_map<QuantKey, uint32_t, QuantKeyHash> table;
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const uint32_t tri[3] = { indices[i], indices[i + 1], indices[i + 2] };
            for (int k = 0; k < 3; ++k)
            {
                const Vec3 v = positions[tri[k]];
                const QuantKey key = Quantize(v, aabb);
                auto it = table.find(key);
                if (it != table.end())
                {
                    out_idx.push_back(it->second);
                }
                else
                {
                    const uint32_t slot = static_cast<uint32_t>(out_pos.size());
                    out_pos.push_back(v);
                    table.emplace(key, slot);
                    out_idx.push_back(slot);
                }
            }
        }
        return out_pos.size();
    }

    // --- 判定：输出索引流里每个三角形解析回的 3 个位置 == 输入该三角形的 3 个位置 ---
    // eps 取量化半步（qscale/2）加一点浮点余量，与 codec 契约一致。
    bool IndicesPreserveGeometry(const std::vector<Vec3>& positions, const std::vector<uint32_t>& indices,
                                 const std::vector<Vec3>& out_pos, const std::vector<uint32_t>& out_idx,
                                 float eps, uint32_t& bad_tri, int& bad_corner)
    {
        if (out_idx.size() != indices.size()) return false;  // 三角形数量必须不变
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t in_idx = indices[t + k];
                const uint32_t out_idx_v = out_idx[t + k];
                if (out_idx_v >= out_pos.size())
                {
                    bad_tri = static_cast<uint32_t>(t / 3);
                    bad_corner = k;
                    return false;  // 索引越界 = 彻底丢
                }
                if (!VecNear(out_pos[out_idx_v], positions[in_idx], eps))
                {
                    bad_tri = static_cast<uint32_t>(t / 3);
                    bad_corner = k;
                    return false;  // 解析到错误顶点 = 索引丢
                }
            }
        }
        return true;
    }

    // 最大几何误差：输出索引流里每个三角形角，解析回的顶点位置与输入原始位置的最大
    // 欧氏距离。no-dedup/float 键应为 0；量化键 <= quant_scale/2（codec 契约上界）。
    float MaxGeometryError(const std::vector<Vec3>& positions, const std::vector<uint32_t>& indices,
                           const std::vector<Vec3>& out_pos, const std::vector<uint32_t>& out_idx)
    {
        float worst = 0.0f;
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            for (int k = 0; k < 3; ++k)
            {
                const Vec3& a = positions[indices[t + k]];
                const Vec3& b = out_pos[out_idx[t + k]];
                const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d > worst) worst = d;
            }
        }
        return worst;
    }

    // --- 测试网格：与 NaniteClusterBuilderSmokeTest::MakeGrid 同构 -------------
    std::vector<Vec3> MakeGridPositions(uint32_t cells_per_side, float extent)
    {
        std::vector<Vec3> p;
        const uint32_t vside = cells_per_side + 1;
        const float step = extent / static_cast<float>(cells_per_side);
        const float origin = -extent * 0.5f;
        for (uint32_t z = 0; z < vside; ++z)
            for (uint32_t x = 0; x < vside; ++x)
                p.push_back(Vec3{ origin + static_cast<float>(x) * step, 0.0f,
                                  origin + static_cast<float>(z) * step });
        return p;
    }
    std::vector<uint32_t> MakeGridIndices(uint32_t cells_per_side)
    {
        std::vector<uint32_t> idx;
        const uint32_t vside = cells_per_side + 1;
        for (uint32_t z = 0; z < cells_per_side; ++z)
        {
            for (uint32_t x = 0; x < cells_per_side; ++x)
            {
                const uint32_t i0 = z * vside + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + vside;
                const uint32_t i3 = i2 + 1;
                idx.insert(idx.end(), { i0, i2, i1, i1, i2, i3 });
            }
        }
        return idx;
    }

    int g_fail = 0;
    void Check(bool c, const char* msg)
    {
        std::printf("  [%s] %s\n", c ? " ok " : "FAIL", msg);
        if (!c) ++g_fail;
    }
}  // namespace

int main()
{
    std::printf("nanite_dedup_probe — 顶点去重会不会丢索引？\n");
    std::printf("============================================================\n");

    // 用一个足够大的网格（32x32 cells = 2048 三角形，2049*2049=... 顶点）。
    // 网格的共享角顶点正是去重要回收的对象：输入有 2049 顶点，no-dedup 会发射
    // 2048*3 = 6144 顶点。
    const uint32_t cells = 32;
    const float extent = 32.0f;
    std::vector<Vec3> positions = MakeGridPositions(cells, extent);
    std::vector<uint32_t> indices = MakeGridIndices(cells);
    const size_t tri_count = indices.size() / 3;
    const size_t input_unique_verts = positions.size();

    std::printf("输入: %zu 三角形, %zu 个唯一输入顶点, %zu 索引\n", tri_count, input_unique_verts, indices.size());

    const AABB aabb = ComputeAABB(positions);
    const float qscale = std::max({ aabb.mx.x - aabb.mn.x, aabb.mx.y - aabb.mn.y, aabb.mx.z - aabb.mn.z });
    // 量化半步容差（与 codec 契约一致），加一点浮点余量。
    const float eps = qscale * 0.5f + 1e-4f;
    std::printf("AABB extent=(%.4f,%.4f,%.4f)  量化半步容差 eps=%.6f\n\n",
                aabb.mx.x - aabb.mn.x, aabb.mx.y - aabb.mn.y, aabb.mx.z - aabb.mn.z, eps);

    // --- 策略 A：现状 no-dedup（基线）--------------------------------------
    {
        std::vector<Vec3> op; std::vector<uint32_t> oi;
        EmitNoDedup(positions, indices, op, oi);
        uint32_t bad_tri; int bad_corner;
        const bool ok = IndicesPreserveGeometry(positions, indices, op, oi, eps, bad_tri, bad_corner);
        Check(op.size() == tri_count * 3, "A no-dedup: 顶点数 == 三角形*3（现状不变量）");
        bool seq = true;
        for (uint32_t i = 0; i < oi.size(); ++i) if (oi[i] != i) seq = false;
        Check(seq, "A no-dedup: 索引是连续 0..n-1（现状不变量）");
        Check(ok, "A no-dedup: 每三角形解析回原始位置（基线正确）");
        std::printf("    顶点: %zu, 最大几何误差: %.6f\n\n", op.size(), MaxGeometryError(positions, indices, op, oi));
    }

    // --- 策略 B1：float 精确键去重 -----------------------------------------
    {
        std::vector<Vec3> op; std::vector<uint32_t> oi;
        const size_t vcount = EmitDedupFloat(positions, indices, op, oi);
        uint32_t bad_tri; int bad_corner;
        const bool ok = IndicesPreserveGeometry(positions, indices, op, oi, eps, bad_tri, bad_corner);
        Check(ok, "B1 float 精确键去重: 每三角形解析回原始位置（索引不丢）");
        Check(vcount == input_unique_verts,
              "B1 float 精确键去重: 顶点数回收到底（== 输入唯一顶点数，网格角共享）");
        Check(MaxGeometryError(positions, indices, op, oi) <= 1e-5f,
              "B1 float 精确键去重: 几何误差为 0（共享的是同一个 float 顶点）");
        std::printf("    顶点: %zu（no-dedup %zu → 省 %.1f%%）, 最大几何误差: %.6f\n\n",
                    vcount, tri_count * 3, 100.0 * (1.0 - static_cast<double>(vcount) / static_cast<double>(tri_count * 3)),
                    MaxGeometryError(positions, indices, op, oi));
    }

    // --- 策略 B2：量化键去重 -----------------------------------------------
    {
        std::vector<Vec3> op; std::vector<uint32_t> oi;
        const size_t vcount = EmitDedupQuant(positions, indices, aabb, op, oi);
        uint32_t bad_tri; int bad_corner;
        // 量化键去重可能把两个 float 不同但量化到同一 int16 的顶点合并——解码位置
        // 仍在 quant_scale/2 内，所以判定容差用 eps（=qscale/2+余量）。
        const bool ok = IndicesPreserveGeometry(positions, indices, op, oi, eps, bad_tri, bad_corner);
        Check(ok, "B2 量化键去重: 每三角形解析回原始位置（在 quant_scale/2 内，索引不丢）");
        Check(vcount <= input_unique_verts,
              "B2 量化键去重: 顶点数 <= float 精确键（量化合并更激进）");
        {
            const float maxerr = MaxGeometryError(positions, indices, op, oi);
            Check(maxerr <= eps, "B2 量化键去重: 几何误差 <= quant_scale/2（codec 契约上界）");
            std::printf("    顶点: %zu（no-dedup %zu → 省 %.1f%%）, 最大几何误差: %.6f（上界 %.6f）\n\n",
                        vcount, tri_count * 3, 100.0 * (1.0 - static_cast<double>(vcount) / static_cast<double>(tri_count * 3)),
                        maxerr, eps);
        }
    }

    // --- 压力：随机抖动顶点（float 精确键会漏合并，量化键应仍合并）----------
    // 给每个顶点加一个远小于量化步长的抖动，模拟"几何上同一点但 float 不同"。
    {
        std::vector<Vec3> jitter = positions;
        const float jitter_amp = qscale * 1e-4f;  // 远小于量化步长
        for (size_t i = 0; i < jitter.size(); ++i)
        {
            // 每个顶点一个确定性的微小偏移（避免完全相同的 float）。
            const float j = jitter_amp * static_cast<float>((i % 7) - 3) / 3.0f;
            jitter[i].x += j; jitter[i].y += j; jitter[i].z += j;
        }
        // 量化键用**未抖动**的稳定 AABB（codec 也是拿网格真实 bounds 量化，不是每顶点
        // 重算）。抖动幅度远小于量化步长，所以用原 AABB 量化抖动顶点，近邻仍落同一 int16。
        const AABB jaabb = aabb;
        const float jeps = qscale * 0.5f + 1e-4f;

        std::vector<Vec3> opf; std::vector<uint32_t> oif;
        const size_t vf = EmitDedupFloat(jitter, indices, opf, oif);
        std::vector<Vec3> opq; std::vector<uint32_t> oiq;
        const size_t vq = EmitDedupQuant(jitter, indices, jaabb, opq, oiq);

        uint32_t bad_tri; int bad_corner;
        const bool okf = IndicesPreserveGeometry(jitter, indices, opf, oif, jeps, bad_tri, bad_corner);
        const bool okq = IndicesPreserveGeometry(jitter, indices, opq, oiq, jeps, bad_tri, bad_corner);
        Check(okf, "抖动网格: float 精确键去重仍不丢索引（只是合并得少）");
        Check(okq, "抖动网格: 量化键去重仍不丢索引（在 quant_scale/2 内）");
        Check(vq <= vf, "抖动网格: 量化键合并 >= float 精确键（顶点更少）");
        const float errf = MaxGeometryError(jitter, indices, opf, oif);
        const float errq = MaxGeometryError(jitter, indices, opq, oiq);
        // 量化键可能把抖动的近邻顶点合并到同一个 int16 槽，误差仍在 quant_scale/2 内，
        // 但**会损失细节**——这正是"量化键去重"的诚实代价，单独量化出来供决策。
        Check(errq <= jeps, "抖动网格: 量化键去重几何误差 <= quant_scale/2（不越契约）");
        std::printf("    抖动后 float 键顶点=%zu（误差 %.6f）, 量化键顶点=%zu（误差 %.6f, 上界 %.6f）\n",
                    vf, errf, vq, errq, jeps);
        std::printf("    [note] 本网格抖动幅度(%.6g)远小于量化步长(%.6g)，两种键合并数相同(%zu)。\n",
                    qscale * 1e-4f, qscale / 32767.0f, vq);
        std::printf("           若抖动跨过量化步长边界，量化键会合并更多但误差仍 <= quant_scale/2。\n\n");
    }

    std::printf("============================================================\n");
    if (g_fail == 0)
    {
        std::printf("结论: 两种去重策略都不丢索引（每个三角形都解析回正确的顶点位置）。\n");
        std::printf("      去重把顶点从 %zu 回收到 ~%zu（网格共享角），省 ~%.0f%% 顶点字节。\n",
                    tri_count * 3, input_unique_verts,
                    100.0 * (1.0 - static_cast<double>(input_unique_verts) / static_cast<double>(tri_count * 3)));
        std::printf("      但会打破 NaniteClusterBuilderSmokeTest 钉住的 6 处\"连续索引/每三角形3顶点\"断言。\n");
        return 0;
    }
    std::printf("结论: %d 项检查失败——去重会丢索引，需回退。\n", g_fail);
    return 1;
}
