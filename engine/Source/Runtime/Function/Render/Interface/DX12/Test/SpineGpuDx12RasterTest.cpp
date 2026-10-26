// =====================================================================
// GPU-Spine DX12 光栅对拍：引擎**真正用的** shader/hlsl/rp2/spine_gpu.{vert,frag}.hlsl
// 画进离屏目标、回读像素，与手算期望比。
// ---------------------------------------------------------------------
// 为什么要它（SpineSkinningParityTest 之外再加一个）：
//   那个测试用的是自己内嵌的 compute 版蒙皮，验证的是**公式**；DX12 渲染路径另有几处
//   "能编译、能跑、悄悄错"的地方，只有拿**同一份** VS/PS 真画一遍才抓得到：
//     * 绘制项下标：D3D12 的 SV_InstanceID 不含 StartInstanceLocation —— 用错就全画成第 0 项。
//       夹具让第 0 项画在右上角、且从不以 firstInstance = 0 去画它：右上角一旦有像素就是这个错。
//     * 结构化 SRV 的 stride（4/8/4/8/4/48/96/4/16）：给错就读到错位的数据（SpineGpuRenderer 的
//       kSet0Strides，这里**照抄同一张表**建 SRV）。
//     * 寄存器映射：set → space、binding → t（DX12RHI::CreatePipelineLayout 的规则；这里按同一规则
//       手搭根签名，一个 binding 一张表）。
//     * 裁剪空间 y 方向：DX12 NDC y 向上 ⇒ 渲染器给正的 sy（ClipSpaceYSign）；骨架空间左上象限的
//       项必须落在图像左上。
//     * weighted 权重（u8 / 255）、tint 的字节序（alpha 在最高字节）、tint black 公式、b8（CPU 裁剪顶点）。
//
// 夹具（64×64 RGBA8，清成 0；骨架空间 [-32,32]² 正好铺满）：
//   item0 rigid，骨骼 2（平移 +32）⇒ 右上象限，白 —— **永远不该出现**
//   item1 rigid，骨骼 0（单位）     ⇒ 左上象限，红
//   item2 weighted 0.6·B0 + 0.4·B1（B1 平移 (10,-10)）⇒ bind 偏 (+4,-4) 落到右下象限，蓝 + 暗色 (0.5,0,0)
//   item3 CPU 顶点（b8，flags bit3）⇒ 左下象限，绿，tint alpha 128
//
// Exit codes: 0 通过 / 1 失败 / 77 没有 D3D12 设备（CTest skip 约定）
// =====================================================================

#include "Runtime/Function/Render/Interface/DX12/DX12ShaderCompiler.h"
#include "Runtime/Function/Render/RenderType.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#ifndef ZENGINE_SPINE_HLSL_DIR
    #error "ZENGINE_SPINE_HLSL_DIR must point at engine/shader/hlsl/rp2"
#endif

namespace
{
// ---- 与 shader / 渲染器逐字段对齐的结构（改一边必须改另一边）----
struct GpuDraw
{
    uint32_t vertex_format;
    uint32_t uv_offset;
    uint32_t slot_bone;
    uint32_t bone_base;
    uint32_t inst_index;
    uint32_t tint;
    uint32_t flags;
    uint32_t weighted_base;
    uint32_t deform_base;
    float dark[3];
};
static_assert(sizeof(GpuDraw) == 48, "SpineDraw is 48 B");
struct GpuInst
{
    float clip_col[16];
    float root_abcd[4];
    float root_t[4];
};
static_assert(sizeof(GpuInst) == 96, "SpineInst is 96 B");
// = SpineGpuRenderer.cpp 的 kSet0Strides（binding 顺序）
constexpr uint32_t kSet0Strides[9] = {4u, 8u, 4u, 8u, 4u, 48u, 96u, 4u, 16u};

constexpr uint32_t kSize = 64;

struct DeviceBundle
{
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
};

bool CreateDeviceBundle(DeviceBundle& out)
{
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&out.factory))))
    {
        return false;
    }
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> candidate;
        if (out.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) ==
            DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        DXGI_ADAPTER_DESC1 desc {};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            continue;
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

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p {};
    p.Type = type;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heap, const void* init)
{
    D3D12_HEAP_PROPERTIES props = HeapProps(heap);
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    const D3D12_RESOURCE_STATES state =
        heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r))))
    {
        return nullptr;
    }
    if (init != nullptr)
    {
        void* dst = nullptr;
        const D3D12_RANGE none {0, 0};
        if (FAILED(r->Map(0, &none, &dst)))
        {
            return nullptr;
        }
        std::memcpy(dst, init, static_cast<size_t>(size));
        r->Unmap(0, nullptr);
    }
    return r;
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* device, uint32_t w, uint32_t h, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear)
{
    D3D12_HEAP_PROPERTIES props = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&r))))
    {
        return nullptr;
    }
    return r;
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

uint32_t FloatBits(float f)
{
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    return u;
}

int g_fail = 0;
int g_pass = 0;
void Check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}
}  // namespace

int main()
{
    std::printf("=== GPU-Spine DX12 raster parity (spine_gpu.*.hlsl) ===\n");

    // ---- 着色器：引擎实际加载的那两份文件 ----
    DX12ShaderCompiler compiler;
    const std::string dir = ZENGINE_SPINE_HLSL_DIR;
    // 公式层在 shader/include/spine_skinning.h（GLSL 与 HLSL 共用），编译时要带 include 路径。
    const std::vector<std::string> include_paths = {ZENGINE_SHADER_INCLUDE_DIR};
    const DX12ShaderCompileResult vs =
        compiler.CompileFromFile(dir + "/spine_gpu.vert.hlsl", ShaderStage::Vertex, include_paths);
    const DX12ShaderCompileResult ps =
        compiler.CompileFromFile(dir + "/spine_gpu.frag.hlsl", ShaderStage::Fragment, include_paths);
    if (!vs.success || !ps.success)
    {
        std::printf("  [FAIL] DXC: %s %s\n", vs.error_message.c_str(), ps.error_message.c_str());
        return 1;
    }

    DeviceBundle bundle;
    if (!CreateDeviceBundle(bundle))
    {
        std::printf("  [skip] no usable D3D12 adapter on this host\n");
        return 77;
    }
    ID3D12Device* device = bundle.device.Get();

    // ---- 夹具数据 ----
    // 骨骼：6 float/骨（a b c d wx wy）。前 3 根是**别的实例**的（平移到画面外很远），本实例的骨骼表
    // 从 bone_base = 3 开始 —— 着色器没加 bone_base 就会读到它们、什么都画不出来（多实例拼包）。
    constexpr uint32_t kBoneBase = 3u;
    const float bones[36] = {1, 0, 0, 1, 1000, 1000,  1, 0, 0, 1, 1000, 1000,  1, 0, 0, 1, 1000, 1000,
                             1, 0, 0, 1, 0, 0,  1, 0, 0, 1, 10, -10,  1, 0, 0, 1, 32, 0};
    // vertex_words：rigid 段 4 顶点（左上象限的方块，骨架坐标）；weighted 段从 word 8 起 (off, cnt)
    std::vector<uint32_t> words;
    const float rigid[8] = {-28, 4, -4, 4, -4, 28, -28, 28};
    for (float f : rigid)
    {
        words.push_back(FloatBits(f));
    }
    for (uint32_t v = 0; v < 4; ++v)
    {
        words.push_back(v * 2u);   // influence_offset
        words.push_back(2u);       // influence_count
    }
    // weighted：bind q，结果 = 0.6·q + 0.4·(q + (10,-10)) = q + (4,-4) ⇒ 目标 x∈[4,28]、y∈[-28,-4]
    const float q[8] = {0, -24, 24, -24, 24, 0, 0, 0};
    std::vector<float> infl_bind;
    std::vector<uint32_t> infl_bone;
    for (uint32_t v = 0; v < 4; ++v)
    {
        for (int k = 0; k < 2; ++k)
        {
            infl_bind.push_back(q[v * 2]);
            infl_bind.push_back(q[v * 2 + 1]);
        }
        infl_bone.push_back(0u | (153u << 16));   // 153/255 = 0.6
        infl_bone.push_back(1u | (102u << 16));   // 102/255 = 0.4
    }
    // 2×2 图集：(0,0) 红 (1,0) 蓝 (0,1) 绿 (1,1) 白；texel 中心 UV
    std::vector<float> uvs;
    const auto push_uv4 = [&](float u, float v) {
        for (int i = 0; i < 4; ++i)
        {
            uvs.push_back(u);
            uvs.push_back(v);
        }
    };
    push_uv4(0.25f, 0.25f);   // [0..3]  红：item1
    push_uv4(0.75f, 0.25f);   // [4..7]  蓝：item2
    push_uv4(0.75f, 0.75f);   // [8..11] 白：item0
    // CPU 顶点（b8）：左下象限，绿
    const float cpu[16] = {-28, -28, 0.25f, 0.75f,  -4, -28, 0.25f, 0.75f,  -4, -4, 0.25f, 0.75f,  -28, -4, 0.25f, 0.75f};
    GpuDraw draws[4] {};
    // 本实例在 b6 里是第 1 个（第 0 个是别的实例：全 0 变换 ⇒ 所有顶点退化，画不出东西）
    constexpr uint32_t kInstIndex = 1u;
    draws[0] = {0u, 8u, 2u, kBoneBase, kInstIndex, 0xFFFFFFFFu, 1u, 8u, 0u, {0, 0, 0}};
    draws[1] = {0u, 0u, 0u, kBoneBase, kInstIndex, 0xFFFFFFFFu, 1u, 8u, 0u, {0, 0, 0}};
    draws[2] = {1u, 4u, 0u, kBoneBase, kInstIndex, 0xFFFFFFFFu, 1u | 16u, 8u, 0u, {0.5f, 0, 0}};
    draws[3] = {0u, 0u, 0u, kBoneBase, kInstIndex, 0x80FFFFFFu, 1u | 8u, 8u, 0u, {0, 0, 0}};
    GpuInst insts[2] {};
    GpuInst& inst = insts[kInstIndex];
    inst.clip_col[0] = 1.0f / 32.0f;
    inst.clip_col[5] = +1.0f / 32.0f;   // DX12：NDC y 向上 ⇒ 正（= 渲染器 ClipSpaceYSign）
    inst.clip_col[10] = 1.0f;
    inst.clip_col[15] = 1.0f;
    inst.root_abcd[0] = 1.0f;
    inst.root_abcd[3] = 1.0f;
    const float deform[4] = {0, 0, 0, 0};
    const uint32_t ids[4] = {0, 1, 2, 3};
    const uint16_t indices[6] = {0, 1, 2, 2, 3, 0};

    struct Src
    {
        const void* data;
        size_t bytes;
    };
    const Src srcs[9] = {{words.data(), words.size() * 4},          {infl_bind.data(), infl_bind.size() * 4},
                         {infl_bone.data(), infl_bone.size() * 4},  {uvs.data(), uvs.size() * 4},
                         {bones, sizeof(bones)},                    {draws, sizeof(draws)},
                         {insts, sizeof(insts)},                     {deform, sizeof(deform)},
                         {cpu, sizeof(cpu)}};
    ComPtr<ID3D12Resource> sb[9];
    for (int i = 0; i < 9; ++i)
    {
        sb[i] = MakeBuffer(device, srcs[i].bytes, D3D12_HEAP_TYPE_UPLOAD, srcs[i].data);
    }
    ComPtr<ID3D12Resource> vb = MakeBuffer(device, sizeof(ids), D3D12_HEAP_TYPE_UPLOAD, ids);
    ComPtr<ID3D12Resource> ib = MakeBuffer(device, sizeof(indices), D3D12_HEAP_TYPE_UPLOAD, indices);

    // ---- 贴图 / 渲染目标 / 回读 ----
    const uint8_t texels[2][256] = {{255, 0, 0, 255, 0, 0, 255, 255}, {0, 255, 0, 255, 255, 255, 255, 255}};
    ComPtr<ID3D12Resource> tex_upload = MakeBuffer(device, 512, D3D12_HEAP_TYPE_UPLOAD, texels);
    ComPtr<ID3D12Resource> tex =
        MakeTexture(device, 2, 2, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    D3D12_CLEAR_VALUE clear {};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> rt = MakeTexture(device, kSize, kSize, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear);
    ComPtr<ID3D12Resource> readback = MakeBuffer(device, 256ull * kSize, D3D12_HEAP_TYPE_READBACK, nullptr);
    if (!vb || !ib || !tex_upload || !tex || !rt || !readback)
    {
        std::printf("  [FAIL] resource creation\n");
        return 1;
    }
    for (auto& b : sb)
    {
        if (!b)
        {
            std::printf("  [FAIL] buffer creation\n");
            return 1;
        }
    }

    // ---- 描述符：一个 binding 一张表（= DX12RHI::CreatePipelineLayout 的映射）----
    ComPtr<ID3D12DescriptorHeap> srv_heap, smp_heap, rtv_heap;
    {
        D3D12_DESCRIPTOR_HEAP_DESC d {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 10, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&srv_heap));
        D3D12_DESCRIPTOR_HEAP_DESC s {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        device->CreateDescriptorHeap(&s, IID_PPV_ARGS(&smp_heap));
        D3D12_DESCRIPTOR_HEAP_DESC r {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        device->CreateDescriptorHeap(&r, IID_PPV_ARGS(&rtv_heap));
    }
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = 0; i < 9; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = static_cast<UINT>(srcs[i].bytes / kSet0Strides[i]);
        sd.Buffer.StructureByteStride = kSet0Strides[i];
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * inc;
        device->CreateShaderResourceView(sb[i].Get(), &sd, h);
    }
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += 9u * inc;
        device->CreateShaderResourceView(tex.Get(), &sd, h);
        D3D12_SAMPLER_DESC smp {};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        device->CreateSampler(&smp, smp_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(rt.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    // ---- 根签名：11 张表（set0 t0..t8 space0；set1 t0 / s0 space1）----
    ComPtr<ID3D12RootSignature> root_sig;
    {
        D3D12_DESCRIPTOR_RANGE ranges[11] {};
        D3D12_ROOT_PARAMETER params[11] {};
        for (UINT i = 0; i < 11; ++i)
        {
            ranges[i].NumDescriptors = 1;
            ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            if (i < 9)
            {
                ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                ranges[i].BaseShaderRegister = i;
                ranges[i].RegisterSpace = 0;
            }
            else
            {
                ranges[i].RangeType = i == 9 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                ranges[i].BaseShaderRegister = 0;
                ranges[i].RegisterSpace = 1;
            }
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable.NumDescriptorRanges = 1;
            params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[i].ShaderVisibility = i < 9 ? (i == 5 ? D3D12_SHADER_VISIBILITY_ALL : D3D12_SHADER_VISIBILITY_VERTEX)
                                               : D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rs {};
        rs.NumParameters = 11;
        rs.pParameters = params;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root_sig))))
        {
            std::printf("  [FAIL] root signature\n");
            return 1;
        }
    }

    // ---- PSO：per-instance u32（TEXCOORD0）+ 预乘 alpha 混合 ----
    ComPtr<ID3D12PipelineState> pso;
    {
        D3D12_INPUT_ELEMENT_DESC elem {"TEXCOORD", 0, DXGI_FORMAT_R32_UINT, 0, 0,
                                       D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root_sig.Get();
        pd.VS = {vs.dxil_code.data(), vs.dxil_code.size()};
        pd.PS = {ps.dxil_code.data(), ps.dxil_code.size()};
        pd.InputLayout = {&elem, 1};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        D3D12_RENDER_TARGET_BLEND_DESC& bl = pd.BlendState.RenderTarget[0];
        bl.BlendEnable = TRUE;
        bl.SrcBlend = D3D12_BLEND_ONE;
        bl.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = D3D12_BLEND_ONE;
        bl.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bl.LogicOp = D3D12_LOGIC_OP_NOOP;
        bl.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        {
            std::printf("  [FAIL] CreateGraphicsPipelineState (shader / root signature / input layout mismatch)\n");
            return 1;
        }
    }

    // ---- 录制 ----
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    {
        const D3D12_COMMAND_QUEUE_DESC qd {D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&cmd));
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = tex_upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256};
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd.Get(), tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const float zero[4] = {0, 0, 0, 0};
    cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp {0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
    const D3D12_RECT sc {0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    ID3D12DescriptorHeap* heaps[2] = {srv_heap.Get(), smp_heap.Get()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(root_sig.Get());
    for (UINT i = 0; i < 10; ++i)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += static_cast<UINT64>(i) * inc;
        cmd->SetGraphicsRootDescriptorTable(i, g);
    }
    cmd->SetGraphicsRootDescriptorTable(10, smp_heap->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW vbv {vb->GetGPUVirtualAddress(), sizeof(ids), 4};
    cmd->IASetVertexBuffers(0, 1, &vbv);
    const D3D12_INDEX_BUFFER_VIEW ibv {ib->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R16_UINT};
    cmd->IASetIndexBuffer(&ibv);
    // firstInstance = 1, 2, 3（**从不**用 0 —— item0 出现 ⇒ 下标取成了 SV_InstanceID）
    for (UINT item = 1; item <= 3; ++item)
    {
        cmd->DrawIndexedInstanced(6, 1, 0, 0, item);
    }
    Barrier(cmd.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 256};
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = rt.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (FAILED(cmd->Close()))
    {
        std::printf("  [FAIL] command list Close\n");
        return 1;
    }
    ID3D12CommandList* lists[1] = {cmd.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    if (FAILED(device->GetDeviceRemovedReason()))
    {
        std::printf("  [FAIL] device removed\n");
        return 1;
    }

    // ---- 比像素（行 0 = 图像顶部）----
    const uint8_t* px = nullptr;
    {
        const D3D12_RANGE all {0, 256ull * kSize};
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, &all, &mapped)))
        {
            std::printf("  [FAIL] readback Map\n");
            return 1;
        }
        px = static_cast<const uint8_t*>(mapped);
    }
    const auto at = [&](uint32_t x, uint32_t y, int c) { return static_cast<int>(px[y * 256u + x * 4u + c]); };
    const auto is = [&](uint32_t x, uint32_t y, int r, int g, int b, int a) {
        const bool ok = std::abs(at(x, y, 0) - r) <= 2 && std::abs(at(x, y, 1) - g) <= 2 &&
                        std::abs(at(x, y, 2) - b) <= 2 && std::abs(at(x, y, 3) - a) <= 2;
        if (!ok)
        {
            std::printf("    pixel (%u,%u) = (%d %d %d %d), want (%d %d %d %d)\n", x, y, at(x, y, 0), at(x, y, 1),
                        at(x, y, 2), at(x, y, 3), r, g, b, a);
        }
        return ok;
    };
    Check(is(16, 16, 255, 0, 0, 255),
          "item1 rigid：骨架左上象限 ⇒ 图像左上、红（y 方向 + rigid 蒙皮 + uv_offset + bone_base / inst_index 偏置）");
    Check(is(48, 16, 0, 0, 0, 0), "右上象限为空：item0 没画 ⇒ 绘制项下标含 StartInstanceLocation（不是 SV_InstanceID）");
    Check(is(48, 48, 128, 0, 255, 255),
          "item2 weighted：右下、蓝 + 暗色 (0.5,0,0) ⇒ (0.5,0,1)（tint black 公式 + b1/b2 stride）");
    Check(is(36, 48, 128, 0, 255, 255) && is(48, 36, 128, 0, 255, 255),
          "weighted 边界：第 36 列 / 行已覆盖（0.6·B0 + 0.4·B1 = +4；权重换了位就是 +6，会空）");
    Check(is(60, 48, 0, 0, 0, 0) && is(48, 60, 0, 0, 0, 0), "weighted 边界：第 60 列 / 行为空");
    Check(is(16, 48, 0, 128, 0, 128), "item3 CPU 顶点（b8，stride 16）：左下、绿，tint alpha 128 在**最高字节**");
    readback->Unmap(0, nullptr);

    std::printf("=== verdict: %s (%d/%d) ===\n", g_fail == 0 ? "PASS" : "FAIL", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
