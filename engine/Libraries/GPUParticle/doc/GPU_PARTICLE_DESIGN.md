# GPUParticle 设计

目标：一套粒子系统，**同时**能在 ZEngine 和 Unity 里跑，并且在**没有 compute
shader 的机型**上也能跑（GLES3.0 / WebGL2 级）。

约束来源：OSGame 只能用 Unity 的 ParticleSystem，不能用 VFX Graph —— VFX 的
模拟全在 compute kernel 里，而有一部分机型没有 compute shader。Unity 自己的
ParticleSystem 之所以到处能跑，是因为它的模拟在 CPU（`Modules/ParticleSystem/
ParticleSystemGeometryJob.cpp`，SIMD + SoA），GPU 只负责画 billboard。

## 一、三档能力分级（一个资产格式、一套模拟语义）

| 档 | 手段 | 需要的能力 | 适用 |
|---|---|---|---|
| Tier 0 | CPU 模拟 + billboard（等价 Unity ParticleSystem） | 无 | 保底 / 需要碰撞回调 |
| **Tier 1** | **无状态 GPU：VS 闭式积分** | 实例化属性 + VTF | GLES3.0 / WebGL2 / 无 compute 的设备 |
| Tier 2 | compute（emit/simulate + indirect dispatch） | SSBO + indirect dispatch | 高端桌面 / 旗舰机 |

档位由 RHI 能力谓词选（`RHI::supportsComputeShaders()` 等，见 AGENTS.md §2.43），
**不是**由 `GraphicsAPI` 枚举猜。

## 二、Tier 1 为什么不需要 compute

粒子**没有状态**。出生那一刻烘焙成一条 `ParticleGpuSpawn`（p0 / v0 / a /
spawnTime / life / seed / 曲线下标），此后每帧顶点着色器按闭式解求位置：

```
t   = time - spawnTime
pos = p0 + v0 * t + 0.5 * a * t²
```

由此全部消失：emit / simulate / kickoff 三条 compute pass、alive-dead 链表、
counter buffer、indirect dispatch 参数。

CPU 剩下的活只有"这一帧谁活着"。恒定速率发射器的第 k 个粒子出生在 `k/rate`
秒，所以：

- **发射**：已知 `time`，缺哪几个序号补哪几个。与 dt 无关，不会有浮点累积漂移
  （`carry += rate*dt` 那种写法帧率一变节奏就变）。
- **存活窗口**：`k ∈ ((t-life)*rate, t*rate]`，O(1) 取整，不必遍历。
- **绘制**：实例数是 CPU 已知的，所以 `CmdDrawInstanced(count)` 即可 —— 没有
  indirect dispatch 也照样画。

CPU 开销从 Unity 的 O(存活粒子) 降到 O(新增粒子)，恒定速率下是 O(1)。

## 三、数据通道：一条路径通吃所有后端

出生记录布局刻意做成 **4 × vec4**（64 B），两种取址方式共用同一份字节：

- 没有 SSBO 的 GLES3.0 / WebGL2：当**实例属性**（`vertexAttribDivisor`，ES3.0 core）。
  窗口在环形缓冲里会绕回，适配器要么把缓冲写两遍，要么在 VS 里取模。
- Vulkan / DX12 / Metal：SSBO 或 VTF 纹理，VS 里 `slot = (first + instanceID) % capacity`。

曲线 / 渐变：烘焙成图集纹理，VS 用 `texelFetch` 采样。ES3.0 / WebGL2 保证
`MAX_VERTEX_TEXTURE_IMAGE_UNITS >= 16`，顶点纹理取样是 core，不需要扩展。

## 四、core / adapter 边界（照 GPUSpine）

```
Libraries/GPUParticle/
├─ Runtime/GPUParticle/     # std-only 核心，不碰 RHI
│   ParticleGpuFormat.h     # 容器格式，尺寸全部 static_assert 钉住
│   ParticleGpuCurves.*     # 曲线 / 渐变（语义对齐 Unity）
│   ParticleGpuEmitter.*    # 发射调度：O(1) 存活窗口
│   ParticleGpuBatch.*      # 拼包 + VS 的 CPU 镜像
├─ Runtime/CSharp/          # netstandard2.1 镜像（第 3 步后半）
├─ Tests~/
└─ CMakeLists.txt           # GPUParticle::Core，PIC ON
```

留在引擎里的只有两层薄适配器：ZEngine 侧 `Function/Particle/ParticleManager.cpp`
（IEngineSystem → 调 core）与 `Function/Render/Passes/ParticlePass.cpp`（RHI：
建缓冲 / 上传 / 录绘制）。着色器公式抽成 `engine/shader/include/particle_sim.h`
（第 4 步），与 `spine_skinning.h` 同构：一份文件供 GLSL / HLSL / Unity 三消费。

## 五、跨语言一致性

`ParticleGpuRandom01` / `ParticleGpuSpawnSeed` 用 lowbias32：只要 uint32 溢出是
wrap 的，任何语言都能复现（C# 侧必须 `unchecked`）。种子掩码到 24 位，因为它的
载体是 `ParticleGpuSpawn::seed`（float —— 出生记录要走实例属性通道，那条通道没有
uint），float 只能精确表示到 2^24。

## 六、还没做

- Unity 导入器：把 ParticleSystem 的模块参数（Main / Shape / Velocity /
  ColorOverLifetime / SizeOverLifetime / Noise / Collision / Trails）烘焙成容器。
  曲线语义必须与 Unity 的 `ParticleSystemCurves.cpp` / `ParticleSystemGradients.cpp`
  对齐（min-max 双曲线、gradient）—— 这是跨语言对拍真正值钱的地方。
- C# 镜像与逐字节对拍（照 `GPUSpine/Runtime/CSharp` + `RunParityTest.cmake`）。
- 碰撞：VS 可以采深度做"一次性贴合"，但不能回写。真碰撞要 Tier 0 / Tier 2，
  所以资产在 cook 期要声明所需 feature 并标出最低档位，设备不满足时降级而不是
  整个特效消失。
