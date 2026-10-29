#pragma once

// =====================================================================
// GPUParticle 容器格式（ZEngine ⇄ Unity 共用）
// ---------------------------------------------------------------------
// 本头文件**只定义数据长什么样**：不含发射逻辑、不含渲染逻辑、不依赖 RHI、
// 不依赖任何引擎。ZEngine 导入器（C++）与 Unity 导入器（C#）必须产出逐字节
// 相同的容器，所以每个结构的尺寸都用 static_assert 钉住——布局错一个字节不会
// 报错，只会在 GPU 上读到别人的字段（GPU-Spine 那边已经吃过这个亏）。
//
// 为什么容器要引擎中立：美术在 Unity ParticleSystem 里出效果（OSGame 只能用
// ParticleSystem，VFX 要 compute shader，有机型没有），ZEngine 编辑器要能吃
// 同一份产物，两边的曲线/渐变求值语义必须一致。见
// doc/GPU_PARTICLE_DESIGN.md。
//
// 端序：一律小端（与 SpineGpuFormat.h 同款约定）。
// =====================================================================

#include <cstdint>

// 'ZPGP'
static constexpr uint32_t kParticleGpuMagic = 0x5A504750u;
static constexpr uint32_t kParticleGpuVersion = 1u;

// 曲线模式，与 Unity 的 ParticleSystemCurveMode 一一对应（常量 / 曲线 /
// 两常量之间随机 / 两曲线之间随机）。随机器由粒子自己的 seed 驱动，所以同一
// 个粒子在 CPU、GPU、C# 三端必须取到同一个值。
enum class ParticleGpuCurveMode : uint32_t
{
    Constant = 0,
    Curve = 1,
    RandomBetweenTwoConstants = 2,
    RandomBetweenTwoCurves = 3,
    Invalid = 4
};

// 发射器模式
enum class ParticleGpuEmitMode : uint32_t
{
    ConstantRate = 0,  // 恒定速率：发射时刻可闭式算，存活窗口 O(1)
    Burst = 1,         // 一次性爆发：整批同生同死
    Invalid = 2
};

struct ParticleGpuHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t emitterCount;
    uint32_t curveCount;
    uint32_t gradientCount;
    uint32_t flags;
    uint32_t reserved[2];
};
static_assert(sizeof(ParticleGpuHeader) == 32, "ParticleGpuHeader must be 8 x u32");

enum class ParticleGpuChunkId : uint32_t
{
    EmitterDescs = 1,
    CurveRanges = 2,
    CurveKeys = 3,
    GradientColorKeys = 4,
    GradientAlphaKeys = 5,
    GradientRanges = 6,
    End = 0xFFFFFFFFu
};

struct ParticleGpuChunkHeader
{
    uint32_t id;            // ParticleGpuChunkId
    uint32_t elementSize;   // 单元素字节数（跨语言校验用）
    uint32_t elementCount;
    uint32_t reserved;
};
static_assert(sizeof(ParticleGpuChunkHeader) == 16, "ParticleGpuChunkHeader must be 4 x u32");

// ---------------------------------------------------------------------
// 发射器描述（作者数据，离线烘焙进容器）
// ---------------------------------------------------------------------
struct ParticleGpuEmitterDesc
{
    float origin[3];        // 发射器原点（局部空间）
    float spread;           // 出生位置的球面半径（0 = 点发射器）
    float velocity[3];      // 初速
    float velocitySpread;   // 初速随机扰动幅度（0 = 无扰动）
    float acceleration[3];  // 常加速度（重力等）—— 闭式积分要求它是常数
    float lifetime;         // 生命（秒）
    float startSize;        // 初始尺寸
    float rate;             // 每秒发射数（ConstantRate）
    uint32_t capacity;      // 环形缓冲容量 = 同时在册粒子数上限
    uint32_t mode;          // ParticleGpuEmitMode
    uint32_t burstCount;    // Burst 一次发多少
    uint32_t seed;          // 确定性随机种子
    float reserved[2];      // 补齐到 16 字节倍数（16 float + 8 u32 = 96 B）
    uint32_t sizeCurve;     // 进 CurveRanges 的下标
    uint32_t colorGradient; // 进 GradientRanges 的下标
    uint32_t flags;
    uint32_t reservedU;
};
static_assert(sizeof(ParticleGpuEmitterDesc) == 96, "ParticleGpuEmitterDesc must be 96 B (16 f32 + 8 u32)");

// ---------------------------------------------------------------------
// 出生记录：发射瞬间烘焙一次，之后**永不回写**（Tier-1 无状态粒子的全部输入）
//
// 布局刻意做成 4 x vec4：GPU 侧既能当实例属性（GLES3.0 / WebGL2 没有 SSBO，
// 只能走 vertexAttribDivisor），也能当 SSBO / VTF 纹理的一行（Vulkan / DX12
// / Metal）。两种取址方式共用同一份字节。
// ---------------------------------------------------------------------
struct ParticleGpuSpawn
{
    float p0[3];        // 出生位置
    float spawnTime;    // 出生时刻（秒，绝对时间）
    float v0[3];        // 出生速度
    float life;         // 生命（秒）
    float a[3];         // 常加速度
    float seed;         // 该粒子的随机种子（bit 级，float 承载以便走属性通道）
    float size0;        // 出生尺寸
    float sizeCurve;    // 进 CurveRanges
    float colorGradient;// 进 GradientRanges
    float flags;
};
static_assert(sizeof(ParticleGpuSpawn) == 64, "ParticleGpuSpawn must be 4 x vec4 (64 B)");

// ---------------------------------------------------------------------
// 曲线 / 渐变
// ---------------------------------------------------------------------
struct ParticleGpuCurveRange
{
    uint32_t mode;        // ParticleGpuCurveMode
    uint32_t keyOffset;   // 进 CurveKeys
    uint32_t keyCount;
    float constantMin;    // Constant / RandomBetweenTwoConstants 的下界
    float constantMax;    // RandomBetweenTwoConstants 的上界
};
static_assert(sizeof(ParticleGpuCurveRange) == 20, "ParticleGpuCurveRange must be 20 B");

// 分段线性关键帧（Unity 的 min-max 曲线在烘焙期已按固定步长重采样成等距键，
// 这样 GPU 侧一次 texelFetch + lerp 就够，不需要在着色器里做关键帧查找）。
struct ParticleGpuCurveKey
{
    float time;   // 归一化 [0,1]
    float value;
};
static_assert(sizeof(ParticleGpuCurveKey) == 8, "ParticleGpuCurveKey must be 8 B");

struct ParticleGpuGradientRange
{
    uint32_t colorKeyOffset;
    uint32_t colorKeyCount;
    uint32_t alphaKeyOffset;
    uint32_t alphaKeyCount;
};
static_assert(sizeof(ParticleGpuGradientRange) == 16, "ParticleGpuGradientRange must be 16 B");

struct ParticleGpuGradientColorKey
{
    float time;  // 归一化 [0,1]
    float r;
    float g;
    float b;
};
static_assert(sizeof(ParticleGpuGradientColorKey) == 16, "ParticleGpuGradientColorKey must be vec4");

struct ParticleGpuGradientAlphaKey
{
    float time;
    float a;
};
static_assert(sizeof(ParticleGpuGradientAlphaKey) == 8, "ParticleGpuGradientAlphaKey must be 8 B");

// 绘制命令：适配器只做"上传 + 按 cmd 画"，与 SpineGpuBatch 的 cmds 同款。
struct ParticleGpuDrawCmd
{
    uint32_t first;     // 存活窗口在环形缓冲里的起始槽位
    uint32_t count;     // 要画的实例数（= 存活粒子数）
    uint32_t capacity;  // 环形容量（VS 里取模用）
    uint32_t flags;
};
static_assert(sizeof(ParticleGpuDrawCmd) == 16, "ParticleGpuDrawCmd must be 4 x u32");
