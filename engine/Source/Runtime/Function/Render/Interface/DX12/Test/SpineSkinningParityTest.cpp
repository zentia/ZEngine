// =====================================================================
// GPU-Spine 验收夹具：CPU 参考实现 vs GPU 回读，逐顶点对拍
// ---------------------------------------------------------------------
// 规格见 doc/GPU_SPINE_INTERCHANGE_FORMAT.md。本文是那份规格的 §12
// 「如何证明两端一致」里第 1 条的可执行版本。
//
// 为什么需要它：
//   蒙皮是最典型的「能编译、能跑、悄悄错」代码。下面这些错误**全部不报任何错**，
//   只是形状微妙地不对：
//     * weighted 的绑定坐标其实是「每个影响一份」，只存一份 → 形状全错
//     * 先混合矩阵再变换（Σwᵢ·Bᵢ 再乘 v）而不是混合变换后的位置 → 数学不等价
//     * a/b/c/d 排错（2×3 仿射的行列顺序）→ 旋转方向反、斜切错位
//     * root 变换乘在蒙皮之前而不是之后 → 二次缩放
//     * 权重没归一化或归一化方式不同 → 顶点飘
//   只有把 GPU 算出来的数**回读出来和 CPU 逐个比**，才能抓住这些。
//
// 夹具刻意**不依赖 spine-cpp**：骨骼变换是手写的任意 2×3 仿射（含旋转、
// 缩放、镜像、斜切、大平移），因为本测试验证的是「蒙皮公式」，不是
// 「骨骼层级解算」——后者永远在 CPU（见规格 §0 非范围）。这样也避免了
// 「先 vendor 哪个版本的 spine-cpp」这个尚未拍板的问题阻塞住验收夹具。
//
// 覆盖范围：
//   * rigid（`_bones` 为空，整块由槽位骨骼单矩阵变换），4 个顶点各用不同骨骼
//     （单位 / 旋转 90° / 缩放+平移 / 镜像 X）
//   * weighted 1 / 2 / 3 / 4 个影响各一份，含权重不等与补零槽位
//   * 非等比的 root 变换（规格 §5.3），验证它施加在蒙皮**之后**
//
// Exit codes:
//   0  -- 全部顶点对拍通过
//   1  -- 有顶点超差（真失败，要查）
//   77 -- 这台机器没有可用的 D3D12 设备（CTest 的 skip 约定）
//
// Build:
//   cmake -B build -DZENGINE_BUILD_SPINE_SKINNING_PARITY_TEST=ON
//   cmake --build build --target SpineSkinningParityTest --config Debug
//   PATH=<repo>/bin/Debug:$PATH ./build/.../SpineSkinningParityTest.exe
// =====================================================================

#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/RenderType.h"
// 顶点/骨骼布局直接吃规格头文件，不在这里重抄一份 ——
// 否则头文件改了、夹具没改，static_assert 就形同虚设。
#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{

// ---------------------------------------------------------------------
// 顶点 / 骨骼布局：直接用规格头文件 SpineGpuFormat.h，不在本文件重抄。
// ---------------------------------------------------------------------
// 这样做是有意的：布局的 static_assert 全在头文件里，如果夹具自己再定义一份
// 结构体，头文件改了夹具也不会察觉，断言就成了摆设。
//
// 【版本锁定：Spine 3.8，规格 §1.1】bone_idx 是 **uint16**，不是随手选的，
// 是被 3.8 钉死的（`VertexAttachment.h:79-81` 的 `Vector<unsigned short> _bones`）。
// bindPos 放首位 + 步长 56 是为了 ByteAddressBuffer::Load2 的 8 字节对齐
// （步长 52 ⇒ 52 % 8 == 4，第二个及以后的顶点取不到对齐的 Load2）。
using VtxRigid = SpineRigidVertex;
using VtxWeighted = SpineWeightedVertex;
using Bone = SpineBoneTransform;

struct Float2
{
    float x = 0.0f, y = 0.0f;
};

constexpr std::uint32_t kBoneStride = SpineGpuConstants::kBoneStrideCompact;  // 24，6 float
constexpr std::uint32_t kRigidStride = SpineGpuConstants::kRigidVertexStride;        // 16
constexpr std::uint32_t kWeightedStride = SpineGpuConstants::kWeightedVertexStride;  // 56

// ---------------------------------------------------------------------
// CPU 参考实现 —— 逐行对应 spine-cpp
//   rigid    : VertexAttachment.cpp:83-84
//   weighted : VertexAttachment.cpp:108-109
//
// 实测（RTX 5080 + MSVC Debug，引擎开 AVX2）：GPU 与 CPU **不能**逐位一致，
// 最大偏差 0.68 ulp。试过三种「关收缩」手段，结论记录在此，免得后面再试一遍：
//   * HLSL 侧加 `precise`（已保留在下面的 shader 里）→ 最大偏差从 3.05e-05
//     降到 7.63e-06，有效但**不够**；
//   * MSVC `#pragma fp_contract(off)` → **完全无效**（不报错也不生效）；
//   * MSVC `/fp:strict` → **无效**（数字一字未变）。
// 所以剩下的差不是某一侧的收缩开关能消掉的，是 DXC 与 MSVC 在 mul+add 上的
// 固有多余舍入。0.68 ulp 对视觉完全无意义，故验收用容差而不是逐位一致 ——
// 容差判断足以抓住所有真正的错误（公式写错通常是数量级级别的差）。
//
// ⚠️ 一个容易误判的陷阱（我踩过）：夹具里 rigid 全对、只有多影响的 weighted
//   差，**看起来**像「CPU 在收缩」，其实是因为 rigid 那几个数在 f32 下恰好
//   精确可表示（整数 × 2.0 缩放 + 整数平移），有没有 FMA 都一样。别拿
//   「rigid 逐位一致」去推断收缩发生在哪一侧。
// ---------------------------------------------------------------------
Float2 BoneApply(const Bone& bone, Float2 p)
{
    return Float2 {p.x * bone.a + p.y * bone.b + bone.world_x,
                   p.x * bone.c + p.y * bone.d + bone.world_y};
}

Float2 SkinRigidCpu(const VtxRigid& v, const std::vector<Bone>& bones, std::uint16_t bone_index)
{
    return BoneApply(bones[bone_index], Float2 {v.bind_pos[0], v.bind_pos[1]});
}

Float2 SkinWeightedCpu(const VtxWeighted& v, const std::vector<Bone>& bones)
{
    float wx = 0.0f;
    float wy = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        const Bone& bone = bones[v.bone_idx[i]];
        const float w = static_cast<float>(v.weight[i]) / 255.0f;
        wx += (v.bind_pos[i][0] * bone.a + v.bind_pos[i][1] * bone.b + bone.world_x) * w;
        wy += (v.bind_pos[i][0] * bone.c + v.bind_pos[i][1] * bone.d + bone.world_y) * w;
    }
    return Float2 {wx, wy};
}

// ---------------------------------------------------------------------
// compute shader —— 与上面的 CPU 参考实现逐字同构
//
// 故意用 ByteAddressBuffer 而不是 StructuredBuffer：root descriptor 不带
// descriptor，因而没有 stride 信息，只能配 raw 视图。副作用是字节偏移全部
// 显式写出来 —— 对一个「字节布局是否符合规格」的测试来说，这是优点。
// ---------------------------------------------------------------------
const char* kSkinningCs = R"(
ByteAddressBuffer gRigid      : register(t0);
ByteAddressBuffer gWeighted   : register(t1);
ByteAddressBuffer gBones      : register(t2);
ByteAddressBuffer gRigidBone  : register(t3);
RWByteAddressBuffer gOut      : register(u0);

cbuffer gConsts : register(b0)
{
    uint  gRigidCount;
    uint  gWeightedCount;
    uint  gRigidStride;
    uint  gWeightedStride;
    float gRootA, gRootB, gRootC, gRootD, gRootX, gRootY;
};

// `precise`：让 DXC 不要重排、也不要把 `x*a + y*b` 收缩成 fma。
// 实测有效但**不足以**拿到逐位一致：最大偏差由 3.05e-05 降到 7.63e-06
// （详见 CPU 参考实现上方的实测记录）。仍然保留，因为它确实把差缩小了 4 倍，
// 而且它表达的是「这里的运算顺序是有意义的，别动」。
precise float2 LoadBoneApply(uint bone_index, float2 bind_pos)
{
    const uint o = bone_index * 24;
    const float a = asfloat(gBones.Load(o + 0));
    const float b = asfloat(gBones.Load(o + 4));
    const float c = asfloat(gBones.Load(o + 8));
    const float d = asfloat(gBones.Load(o + 12));
    const float x = asfloat(gBones.Load(o + 16));
    const float y = asfloat(gBones.Load(o + 20));
    return float2(bind_pos.x * a + bind_pos.y * b + x,
                  bind_pos.x * c + bind_pos.y * d + y);
}

// 规格 §5.3：root 变换施加在蒙皮**之后**，不能预乘进骨骼表。
float2 RootApply(float2 p)
{
    return float2(p.x * gRootA + p.y * gRootB + gRootX,
                  p.x * gRootC + p.y * gRootD + gRootY);
}

[numthreads(64, 1, 1)]
void SkinMain(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;

    if (i < gRigidCount)
    {
        const uint base = i * gRigidStride;
        const float2 bind_pos = asfloat(gRigid.Load2(base));
        const uint bone_index = gRigidBone.Load(i * 4);
        const float2 p = RootApply(LoadBoneApply(bone_index, bind_pos));
        gOut.Store2(i * 8, asuint(p));
    }

    if (i < gWeightedCount)
    {
        const uint base = i * gWeightedStride;
        const uint packed_idx0 = gWeighted.Load(base + 40);
        const uint packed_idx1 = gWeighted.Load(base + 44);
        const uint packed_w    = gWeighted.Load(base + 48);

        // 规格 §6：混合的是「变换后的位置」，不是矩阵 —— 绝不能先 Σwᵢ·Bᵢ。
        // 累加顺序按影响下标升序，与 spine-cpp 一致（f32 加法不满足结合律）。
        precise float wx = 0.0;
        precise float wy = 0.0;
        [unroll]
        for (int k = 0; k < 4; ++k)
        {
            const uint packed = (k < 2) ? packed_idx0 : packed_idx1;
            const uint bone_index = (packed >> (16 * (k & 1))) & 0xFFFFu;
            const float w = float((packed_w >> (8 * k)) & 0xFFu) / 255.0;
            const float2 bind_pos = asfloat(gWeighted.Load2(base + k * 8));
            const float2 skinned = LoadBoneApply(bone_index, bind_pos);
            wx += skinned.x * w;
            wy += skinned.y * w;
        }

        const float2 p = RootApply(float2(wx, wy));
        gOut.Store2((gRigidCount + i) * 8, asuint(p));
    }
}
)";

// ---------------------------------------------------------------------
// D3D12 样板
// ---------------------------------------------------------------------
struct DeviceBundle
{
    // IDXGIFactory6（不是 4）：EnumAdapterByGpuPreference 在 6 上才有。
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
};

bool CreateDeviceBundle(DeviceBundle& out)
{
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&out.factory))))
    {
        std::printf("  [info] CreateDXGIFactory2 failed\n");
        return false;
    }

    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> candidate;
        if (out.factory->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc {};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;  // 第一轮跳过 WARP，最后再兜底
        }

        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out.device))))
        {
            std::printf("  device: hardware adapter '%ls'\n", desc.Description);
            return true;
        }
    }

    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(out.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
        SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out.device))))
    {
        std::printf("  device: WARP software adapter\n");
        return true;
    }

    return false;
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device,
                                    std::uint64_t size,
                                    D3D12_HEAP_TYPE heap,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    // 每个 CreateCommittedResource 的基址都是 64 KB 对齐的，所以
    // root CBV 要求的 256 B 对齐自然满足，不需要自己管 suballocation。
    //
    // 手写而不用 CD3DX12_*：仓库里没有 d3dx12.h（只有 DX12RHI.cpp 自己拼
    // 这些结构），这个 exe 也刻意只依赖 d3d12.h。
    D3D12_HEAP_PROPERTIES props {};
    props.Type = heap;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> buffer;
    const D3D12_RESOURCE_STATES initial = (heap == D3D12_HEAP_TYPE_UPLOAD)
                                              ? D3D12_RESOURCE_STATE_GENERIC_READ
                                              : D3D12_RESOURCE_STATE_COMMON;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
                                               IID_PPV_ARGS(&buffer))))
    {
        return nullptr;
    }
    return buffer;
}

// ---------------------------------------------------------------------
// 夹具数据
// ---------------------------------------------------------------------
struct Fixture
{
    std::vector<Bone> bones;
    std::vector<VtxRigid> rigid;
    std::vector<std::uint16_t> rigid_bone;
    std::vector<VtxWeighted> weighted;
    Bone root {};
};

Fixture BuildFixture()
{
    Fixture f;

    // 手写的任意 2×3 仿射 —— 覆盖会暴露「顺序写反 / 符号写反」的形状
    f.bones.push_back(Bone {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});                    // 0 单位
    f.bones.push_back(Bone {0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f});                   // 1 旋转 90°
    f.bones.push_back(Bone {2.0f, 0.0f, 0.0f, 2.0f, 100.0f, -50.0f});                // 2 缩放 + 平移
    f.bones.push_back(Bone {-1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f});                   // 3 镜像 X
    f.bones.push_back(Bone {-1.5f, 0.25f, 0.5f, 2.0f, -300.0f, 700.0f});             // 4 斜切 + 镜像 + 大平移
    f.bones.push_back(Bone {0.866f, -0.5f, 0.5f, 0.866f, 1000.0f, 1000.0f});         // 5 旋转 30° + 大平移

    // 非等比 root：a=1.5, d=0.5 —— 蒙皮之后再乘，若被预乘进骨骼表会二次缩放
    f.root = Bone {1.5f, 0.0f, 0.0f, 0.5f, -20.0f, 33.0f};

    // rigid：一个四边形，四个角各用一根不同的骨骼（单位 / 旋转 / 缩放 / 镜像）
    const float quad[4][2] = {{10.0f, 20.0f}, {-10.0f, 20.0f}, {-10.0f, -20.0f}, {10.0f, -20.0f}};
    for (int i = 0; i < 4; ++i)
    {
        VtxRigid v {};
        v.bind_pos[0] = quad[i][0];
        v.bind_pos[1] = quad[i][1];
        v.uv[0] = (i & 1) ? 1.0f : 0.0f;
        v.uv[1] = (i & 2) ? 1.0f : 0.0f;
        f.rigid.push_back(v);
        f.rigid_bone.push_back(static_cast<std::uint16_t>(i));  // 骨骼 0..3
    }

    // weighted：1 / 2 / 3 / 4 个影响
    auto make_weighted = [](const float bind[4][2],
                            const std::uint16_t idx[4],
                            const std::uint8_t w[4]) {
        VtxWeighted v {};
        for (int i = 0; i < 4; ++i)
        {
            v.bind_pos[i][0] = bind[i][0];
            v.bind_pos[i][1] = bind[i][1];
            v.bone_idx[i] = idx[i];
            v.weight[i] = w[i];
        }
        v.uv[0] = 0.25f;
        v.uv[1] = 0.75f;
        return v;
    };

    // 1 个影响（补零槽位 3 个）
    {
        const float bind[4][2] = {{5.0f, 7.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
        const std::uint16_t idx[4] = {1, 0, 0, 0};
        const std::uint8_t w[4] = {255, 0, 0, 0};
        f.weighted.push_back(make_weighted(bind, idx, w));
    }
    // 2 个影响，权重不等
    {
        const float bind[4][2] = {{-30.0f, 12.0f}, {45.0f, -8.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
        const std::uint16_t idx[4] = {2, 3, 0, 0};
        const std::uint8_t w[4] = {128, 127, 0, 0};
        f.weighted.push_back(make_weighted(bind, idx, w));
    }
    // 3 个影响（和恰好 255）
    {
        const float bind[4][2] = {{1.0f, 2.0f}, {3.0f, 4.0f}, {5.0f, 6.0f}, {0.0f, 0.0f}};
        const std::uint16_t idx[4] = {0, 4, 5, 0};
        const std::uint8_t w[4] = {85, 85, 85, 0};
        f.weighted.push_back(make_weighted(bind, idx, w));
    }
    // 4 个影响，权重不等（64+64+64+63 = 255）
    {
        const float bind[4][2] = {{-100.0f, 250.0f}, {7.5f, -3.25f}, {0.5f, 0.5f}, {-1.0f, 1.0f}};
        const std::uint16_t idx[4] = {1, 2, 3, 4};
        const std::uint8_t w[4] = {64, 64, 64, 63};
        f.weighted.push_back(make_weighted(bind, idx, w));
    }
    // 4 个影响，含旋转 30° + 大平移（量级到 1e3，顺带看精度）
    {
        const float bind[4][2] = {{-512.0f, 512.0f}, {256.0f, -256.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}};
        const std::uint16_t idx[4] = {5, 5, 1, 3};
        const std::uint8_t w[4] = {100, 80, 50, 25};
        f.weighted.push_back(make_weighted(bind, idx, w));
    }

    return f;
}

}  // namespace

int main()
{
    const Fixture fixture = BuildFixture();
    const std::uint32_t rigid_count = static_cast<std::uint32_t>(fixture.rigid.size());
    const std::uint32_t weighted_count = static_cast<std::uint32_t>(fixture.weighted.size());
    const std::uint32_t total = rigid_count + weighted_count;

    std::printf("=== GPU-Spine skinning parity (CPU reference vs GPU readback) ===\n");
    std::printf("  bones=%zu rigid=%u weighted=%u\n", fixture.bones.size(), rigid_count, weighted_count);

    // ---- 1. CPU 参考结果 ------------------------------------------------
    std::vector<Float2> expected(total);
    for (std::uint32_t i = 0; i < rigid_count; ++i)
    {
        const Float2 skinned = SkinRigidCpu(fixture.rigid[i], fixture.bones, fixture.rigid_bone[i]);
        expected[i] = BoneApply(fixture.root, skinned);
    }
    for (std::uint32_t i = 0; i < weighted_count; ++i)
    {
        const Float2 skinned = SkinWeightedCpu(fixture.weighted[i], fixture.bones);
        expected[rigid_count + i] = BoneApply(fixture.root, skinned);
    }

    // ---- 2. DXC：编译 compute shader -----------------------------------
    DX12ShaderCompiler compiler;
    DX12ShaderCompileResult cs = compiler.CompileFromSource(kSkinningCs, ShaderStage::Compute,
                                                                  "spine_skinning_parity", {}, {},
                                                                  "SkinMain", "cs_6_0");
    if (!cs.success)
    {
        std::printf("  [FAIL] DXC:\n%s\n", cs.error_message.c_str());
        return 1;
    }
    std::printf("  [ OK ] DXC -> DXIL %zu B\n", cs.dxil_code.size());

    // ---- 3. D3D12 设备 --------------------------------------------------
    DeviceBundle bundle;
    if (!CreateDeviceBundle(bundle))
    {
        std::printf("  [skip] no usable D3D12 adapter on this host\n");
        return 77;
    }
    ID3D12Device* device = bundle.device.Get();

    ComPtr<ID3D12CommandQueue> queue;
    {
        const D3D12_COMMAND_QUEUE_DESC desc {D3D12_COMMAND_LIST_TYPE_DIRECT, 0,
                                             D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
        {
            std::printf("  [FAIL] CreateCommandQueue\n");
            return 1;
        }
    }

    // Root signature：全部用 inline root descriptor（root SRV/UAV/CBV），
    // 因此**完全不需要 descriptor heap**。代价是只能配 raw 视图，shader 侧
    // 就得用 ByteAddressBuffer —— 上面已经这么写了。
    ComPtr<ID3D12RootSignature> root_signature;
    {
        D3D12_ROOT_PARAMETER params[6] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[0].Descriptor.ShaderRegister = 0;  // gRigid
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 1;  // gWeighted
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 2;  // gBones
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor.ShaderRegister = 3;  // gRigidBone
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].Descriptor.ShaderRegister = 0;  // gOut
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[5].Descriptor.ShaderRegister = 0;  // gConsts
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        const D3D12_ROOT_SIGNATURE_DESC desc {6, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};

        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        // D3D_ROOT_SIGNATURE_VERSION_1（d3dcommon.h 的枚举，不是 D3D12_ 前缀）
        // 配 D3D12_ROOT_SIGNATURE_DESC 的 1.0 版。
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        {
            std::printf("  [FAIL] SerializeRootSignature: %s\n",
                        error ? static_cast<const char*>(error->GetBufferPointer()) : "");
            return 1;
        }
        if (FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root_signature))))
        {
            std::printf("  [FAIL] CreateRootSignature\n");
            return 1;
        }
    }

    ComPtr<ID3D12PipelineState> pso;
    {
        const D3D12_COMPUTE_PIPELINE_STATE_DESC desc {root_signature.Get(),
                                                      {cs.dxil_code.data(), cs.dxil_code.size()},
                                                      0, nullptr, 0};
        if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso))))
        {
            std::printf("  [FAIL] CreateComputePipelineState\n");
            return 1;
        }
    }
    std::printf("  [ OK ] compute PSO\n");

    // ---- 4. 缓冲 --------------------------------------------------------
    const std::uint64_t rigid_bytes = static_cast<std::uint64_t>(rigid_count) * kRigidStride;
    const std::uint64_t weighted_bytes = static_cast<std::uint64_t>(weighted_count) * kWeightedStride;
    const std::uint64_t bone_bytes = static_cast<std::uint64_t>(fixture.bones.size()) * kBoneStride;
    const std::uint64_t rigid_bone_bytes = static_cast<std::uint64_t>(rigid_count) * sizeof(std::uint32_t);
    const std::uint64_t out_bytes = static_cast<std::uint64_t>(total) * sizeof(float) * 2;

    ComPtr<ID3D12Resource> buf_rigid = CreateBuffer(device, rigid_bytes, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> buf_weighted = CreateBuffer(device, weighted_bytes, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> buf_bones = CreateBuffer(device, bone_bytes, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> buf_rigid_bone = CreateBuffer(device, rigid_bone_bytes, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> buf_consts = CreateBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> buf_out = CreateBuffer(device, out_bytes, D3D12_HEAP_TYPE_DEFAULT,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> buf_readback = CreateBuffer(device, out_bytes, D3D12_HEAP_TYPE_READBACK);
    if (!buf_rigid || !buf_weighted || !buf_bones || !buf_rigid_bone || !buf_consts || !buf_out ||
        !buf_readback)
    {
        std::printf("  [FAIL] buffer allocation\n");
        return 1;
    }

    // 静态 VB（按规格 §3.1 的字节布局填入，weight 已归一到和 = 255）
    auto upload = [](ID3D12Resource* target, const void* data, std::uint64_t size) {
        void* mapped = nullptr;
        const D3D12_RANGE read_range {0, 0};  // 我们不读
        if (FAILED(target->Map(0, &read_range, &mapped)))
        {
            return false;
        }
        std::memcpy(mapped, data, static_cast<std::size_t>(size));
        target->Unmap(0, nullptr);
        return true;
    };

    if (!upload(buf_rigid.Get(), fixture.rigid.data(), rigid_bytes) ||
        !upload(buf_weighted.Get(), fixture.weighted.data(), weighted_bytes) ||
        !upload(buf_bones.Get(), fixture.bones.data(), bone_bytes))
    {
        std::printf("  [FAIL] upload static buffers\n");
        return 1;
    }

    // rigid 的骨骼来自槽位（规格 §4 的 DrawItem.slot_bone）；夹具里退化成
    // 每顶点一个 u32（shader 侧按 4 B 步长 Load）。
    {
        std::vector<std::uint32_t> rigid_bone_u32(rigid_count);
        for (std::uint32_t i = 0; i < rigid_count; ++i)
        {
            rigid_bone_u32[i] = fixture.rigid_bone[i];
        }
        if (!upload(buf_rigid_bone.Get(), rigid_bone_u32.data(), rigid_bone_bytes))
        {
            std::printf("  [FAIL] upload rigid bone indices\n");
            return 1;
        }
    }

    {
        struct Consts
        {
            std::uint32_t rigid_count;
            std::uint32_t weighted_count;
            std::uint32_t rigid_stride;
            std::uint32_t weighted_stride;
            float root_a, root_b, root_c, root_d, root_x, root_y;
        } consts {};
        consts.rigid_count = rigid_count;
        consts.weighted_count = weighted_count;
        consts.rigid_stride = kRigidStride;
        consts.weighted_stride = kWeightedStride;
        consts.root_a = fixture.root.a;
        consts.root_b = fixture.root.b;
        consts.root_c = fixture.root.c;
        consts.root_d = fixture.root.d;
        consts.root_x = fixture.root.world_x;
        consts.root_y = fixture.root.world_y;
        if (!upload(buf_consts.Get(), &consts, sizeof(consts)))
        {
            std::printf("  [FAIL] upload constants\n");
            return 1;
        }
    }

    // ---- 5. 录制 + 执行 + 回读 ------------------------------------------
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        std::printf("  [FAIL] command objects\n");
        return 1;
    }

    const std::uint32_t group_count = (total + 63) / 64;

    cmd->SetComputeRootSignature(root_signature.Get());
    cmd->SetPipelineState(pso.Get());
    cmd->SetComputeRootShaderResourceView(0, buf_rigid->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(1, buf_weighted->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, buf_bones->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(3, buf_rigid_bone->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, buf_out->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(5, buf_consts->GetGPUVirtualAddress());
    cmd->Dispatch(group_count, 1, 1);

    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buf_out.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &barrier);
    }
    cmd->CopyResource(buf_readback.Get(), buf_out.Get());

    if (FAILED(cmd->Close()))
    {
        std::printf("  [FAIL] Close command list\n");
        return 1;
    }

    {
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
    }
    if (FAILED(queue->Signal(fence.Get(), 1)))
    {
        std::printf("  [FAIL] Signal\n");
        return 1;
    }

    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
    {
        std::printf("  [FAIL] CreateEvent\n");
        return 1;
    }
    fence->SetEventOnCompletion(1, event);
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    // ---- 6. 对拍 --------------------------------------------------------
    const D3D12_RANGE read_range {0, static_cast<SIZE_T>(out_bytes)};
    void* mapped = nullptr;
    if (FAILED(buf_readback->Map(0, &read_range, &mapped)))
    {
        std::printf("  [FAIL] map readback buffer\n");
        return 1;
    }
    const auto* gpu = static_cast<const float*>(mapped);

    int failures = 0;
    double max_abs_error = 0.0;
    double max_ulp = 0.0;
    bool bit_exact = true;
    std::vector<std::uint32_t> differing;

    for (std::uint32_t i = 0; i < total; ++i)
    {
        const float gx = gpu[i * 2 + 0];
        const float gy = gpu[i * 2 + 1];
        const float ex = expected[i].x;
        const float ey = expected[i].y;

        const double dx = std::fabs(static_cast<double>(gx) - static_cast<double>(ex));
        const double dy = std::fabs(static_cast<double>(gy) - static_cast<double>(ey));
        const double worst = (dx > dy) ? dx : dy;
        if (worst > max_abs_error)
        {
            max_abs_error = worst;
        }
        if (gx != ex || gy != ey)
        {
            bit_exact = false;
        }

        // 坐标量级到 1e3（骨骼表里有 1000 的平移），用相对 + 绝对双阈值。
        const double scale = std::fmax(1.0, std::fabs(static_cast<double>(ex)));

        // 用 ulp 报告而不是只报绝对误差：FMA 收缩的特征就是「差不到 1 ulp」。
        // 看到 <= 1 ulp 基本可以断定是某一边把 mul+add 合并了，而不是公式写错
        // （公式写错通常是量级错误，差几个数量级）。
        const double ulp = worst / (static_cast<double>(FLT_EPSILON) * scale);
        if (ulp > max_ulp)
        {
            max_ulp = ulp;
        }
        if (!bit_exact)
        {
            differing.push_back(i);
        }

        if (worst > 1e-3 * scale && worst > 1e-3)
        {
            ++failures;
            const bool is_rigid = i < rigid_count;
            const std::uint32_t local = is_rigid ? i : (i - rigid_count);
            std::printf("  [FAIL] %s[%u] gpu=(%.6f, %.6f) cpu=(%.6f, %.6f) diff=%.3e\n",
                        is_rigid ? "rigid" : "weighted", local, gx, gy, ex, ey, worst);
        }
    }

    buf_readback->Unmap(0, nullptr);

    std::printf("  max abs error: %.3e   max %.2f ulp   %s\n", max_abs_error, max_ulp,
                bit_exact ? "(bit-exact with the CPU reference)"
                          : "(sub-ulp -- one side fused mul+add; see spec §6 #3)");

    if (!bit_exact)
    {
        // 定位到具体顶点：如果只有 weighted 差、rigid 全对（或反过来），
        // 收缩就只发生在那一条路径上。这比笼统地说「有 ulp 差」有用得多。
        std::printf("  bit-exact on %zu/%u vertices; differing: ", total - differing.size(), total);
        for (std::uint32_t index : differing)
        {
            const bool is_rigid = index < rigid_count;
            std::printf("%s[%u] ", is_rigid ? "rigid" : "weighted",
                        is_rigid ? index : (index - rigid_count));
        }
        std::printf("\n");
    }
    std::printf("\n=== verdict: %s (%u/%u vertices) ===\n", failures == 0 ? "PASS" : "FAIL",
                total - static_cast<std::uint32_t>(failures), total);

    return failures == 0 ? 0 : 1;
}
