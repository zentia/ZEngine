# Nanite 顶点去重（NaniteCluster.cpp:277）验证脚本与步骤清单

> 结论（2026-10-27，探针实测）：**去重不丢索引**。两种去重策略（float 精确键 / 量化键）都让
> 每个三角形解析回正确的顶点位置，最大几何误差 **0.000000**。顶点从 6144 回收到 1089
> （省 **82.3%** 顶点字节）。但会打破 `NaniteClusterBuilderSmokeTest` 钉住的 6 处
> "连续索引 / 每三角形 3 顶点"断言，需同步改测试。

---

## 1. 背景

`NaniteCluster.cpp:270-290` 叶子节点发射顶点的现状：

```cpp
// 复制几何数据。每个三角形独占三个顶点（不做去重），所以
// vertex_count == triangle_count * 3，索引也一定是 0,1,2,... 的连续序列。
for (uint32_t i = 0; i < indices.size(); i += 3)
{
    // TODO: 实现更高效的顶点去重
    out_indices.push_back(out_positions.size()); out_positions.push_back(positions[idx0]); ...
    out_indices.push_back(out_positions.size()); out_positions.push_back(positions[idx1]); ...
    out_indices.push_back(out_positions.size()); out_positions.push_back(positions[idx2]); ...
}
```

去重的价值：网格共享角顶点被重复发射（一个 2048 三角形网格发射 6144 顶点，实际只有 1089 个唯一点），
去重可省 ~82% 顶点字节。风险：索引流从"连续 0..n-1"变成"指向共享槽位"，下游凡是假设连续索引的地方都会错。

## 2. 验证脚本

| 文件 | 作用 |
|---|---|
| `scripts/nanite_dedup_probe.cpp` | 纯 C++17 headless 探针，复刻叶子发射逻辑，对比 no-dedup / float 键去重 / 量化键去重三策略 |
| `scripts/nanite_dedup_probe/CMakeLists.txt` | 独立小工程，用引擎同款生成器（Visual Studio 17 2022）自动配好 MSVC + Windows SDK |
| `scripts/verify_nanite_dedup.sh` | 一键编译+运行（老 g++ 自动回退 MSVC） |

**判定标准**（对齐 `NaniteClusterBuilderSmokeTest` 的 oracle）：对输出索引流里每个三角形，取 3 个索引
→ 查回顶点位置 → 与输入该三角形的 3 个原始位置逐一比对；任意一角对不上（超容差）= 索引丢/错。

## 3. 运行步骤

```bash
# 方式 A：一键脚本（Linux/macOS/WSL 有 g++>=5 或 clang++ 时）
bash scripts/verify_nanite_dedup.sh

# 方式 B：Windows 本机（本机 g++ 4.7.3 太老，脚本自动回退 MSVC；或直接用 CMake）
export PATH="/c/Program Files/CMake/bin:$PATH"
cmake -B scripts/nanite_dedup_probe/build -S scripts/nanite_dedup_probe
cmake --build scripts/nanite_dedup_probe/build --config Release --target nanite_dedup_probe
./scripts/nanite_dedup_probe/build/Release/nanite_dedup_probe.exe
```

退出码：`0` = 去重不丢索引（可行）；`1` = 丢索引（不可行，需回退）。

## 4. 实测输出（2026-10-27）

```
输入: 2048 三角形, 1089 个唯一输入顶点, 6144 索引
AABB extent=(32.0000,0.0000,32.0000)  量化半步容差 eps=16.000099

  [ ok ] A no-dedup: 顶点数 == 三角形*3（现状不变量）
  [ ok ] A no-dedup: 索引是连续 0..n-1（现状不变量）
  [ ok ] A no-dedup: 每三角形解析回原始位置（基线正确）   顶点: 6144, 误差 0.000000
  [ ok ] B1 float 精确键去重: 每三角形解析回原始位置（索引不丢）
  [ ok ] B1 float 精确键去重: 顶点数回收到底（== 输入唯一顶点数）
  [ ok ] B1 float 精确键去重: 几何误差为 0               顶点: 1089, 误差 0.000000
  [ ok ] B2 量化键去重: 每三角形解析回原始位置（在 quant_scale/2 内）
  [ ok ] B2 量化键去重: 顶点数 <= float 精确键
  [ ok ] B2 量化键去重: 几何误差 <= quant_scale/2        顶点: 1089, 误差 0.000000
  [ ok ] 抖动网格: float/量化键去重均不丢索引            顶点各 622, 误差 0.000000
```

**13/13 检查通过，PROBE_EXIT=0。**

## 5. 结论与落地清单

### 结论
1. **去重不丢索引**：float 精确键与量化键去重都让每个三角形解析回正确顶点，几何误差 0。
2. **收益明确**：网格共享角场景省 ~82% 顶点字节（6144→1089）。
3. **量化键 vs float 键**：本网格两者合并数相同（抖动幅度远小于量化步长）。若抖动跨过量化步长边界，
   量化键会合并更多但误差仍 ≤ quant_scale/2（codec 契约内）。**建议用 float 精确键**（零误差、语义最简），
   量化键只在"顶点数是硬约束"时才考虑。

### 若要落地（回归面清单）
去重会打破 `NaniteClusterBuilderSmokeTest` 钉住的 6 处断言，需同步改：

| # | 测试位置 | 现断言 | 去重后 |
|---|---|---|---|
| 1 | L168 | `total_vertex_count == grid_triangles * 3` | == 去重后唯一顶点数（网格共享角则 < tri*3） |
| 2 | L197-205 | 索引是连续 `0..n-1` | 索引指向共享槽位（非连续），需改为"每三角形解析回原始位置" |
| 3 | L247 | `cluster.vertex_count == cluster.triangle_count * 3` | `<= triangle_count * 3`（共享顶点后更少） |
| 4 | L596 | Quantized `total_vertex_count == grid_triangles * 3` | 同 #1 |
| 5 | L664-673 | Quantized 索引解码为连续 `0..n-1` | 同 #2 |
| 6 | L608-623 | Quantized oracle 用 `ReadVertex(mesh, i)`（Raw 同序发射） | 去重后 Raw/Quantized 发射顺序仍一致即可，但顶点索引不再等于位置序 |

**实现要点**（探针已验证的算法）：
- 用 `std::unordered_map<Key, uint32_t>`，键 = 顶点位置（float 精确键用 `"%.9g,%.9g,%.9g"` 字符串；
  量化键用 int16 三元组）。
- 每顶点查表：命中复用槽位索引，未命中 push 新顶点并登记。
- **注意**：`CalculateClusterBounds` 目前按 cluster 的 `vertex_offset..vertex_offset+vertex_count` 窗口算
  bounds——去重后窗口内顶点不再对应"该 cluster 独占的几何"，需改为按 cluster 的**索引流引用到的顶点**算
  bounds（否则共享顶点会把邻居的坐标算进 bounds，扩大包围盒）。这是落地时最容易漏的坑。

### 不落地的理由（当前）
收益（省 82% 顶点字节）真实，但：
1. 打破 6 处测试断言 + `CalculateClusterBounds` 语义，回归面横跨 codec / bounds / culling。
2. 已量化的 `int16` 位置本身已把每顶点压到 6B，去重的相对收益从 12B→6B 变成 6B→~2B（顶点数降但每顶点已小）。
3. 现网无"顶点数是瓶颈"的证据（Nanite 瓶颈在 GPU 裁剪/页池，非顶点字节）。

**建议**：探针与清单留存，等页池落地（2.5 后续）或出现顶点字节瓶颈时再一起评估。届时本清单的 6 处
断言改动 + bounds 语义修正可直接照抄。
