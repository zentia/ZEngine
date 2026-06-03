#pragma once

// Built-in directional shadow shaders for DX12 (Vulkan path uses precompiled SPIR-V blobs).

namespace ShadowPassDx12Shaders
{
    inline const char* kDirectionalShadowVert = R"(
struct MeshInstance
{
    float enable_vertex_blending;
    float3 _pad0;
    float4x4 model_matrix;
};

cbuffer PerFrame : register(b0)
{
    float4x4 light_proj_view;
};

StructuredBuffer<MeshInstance> mesh_instances : register(t1);

struct VsInput
{
    float3 position : POSITION;
};

struct VsOutput
{
    float4 position : SV_POSITION;
};

VsOutput main(VsInput input, uint instance_id : SV_InstanceID)
{
    float4 world = mul(mesh_instances[instance_id].model_matrix, float4(input.position, 1.0f));
    VsOutput output;
    output.position = mul(light_proj_view, world);
    return output;
}
)";

    inline const char* kDirectionalShadowFrag = R"(
float main(float4 position : SV_POSITION) : SV_Target0
{
    return position.z / position.w;
}
)";
}  // namespace ShadowPassDx12Shaders
