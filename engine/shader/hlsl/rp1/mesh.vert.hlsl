// DX-B3 + PR-SK1: mesh vertex shader. Handles both rigid and skinned meshes
// (MeshVertex 3-stream layout + a per-vertex joint-binding SRV).
//
// Skinned path mirrors engine/shader/glsl/mesh.vert so the Vulkan and DX12
// backends agree: the joint palette is indexed as
// [kMeshVertexBlendingMaxJointCount * instance_id + joint_index] and slot 0 is
// the identity root, so a joint index of 0 means "no influence" (see
// Runtime/Function/Animation/Skinning.h).

// render_storage_structs.hlsli already includes scene_lighting_structs.hlsli
#include "../common/render_storage_structs.hlsli"
#include "../common/main_camera_per_frame_access.hlsli"

cbuffer PerFrame : register(b0)
{
    MainCameraPerFrame per_frame;
};

StructuredBuffer<MeshInstance> mesh_instances : register(t1);
// Per-drawcall joint palette (stride 64 == sizeof(float4x4); see
// MainCameraRp1Pass.cpp, which sets RHIDescriptorBufferInfo::stride).
StructuredBuffer<float4x4> joint_matrices : register(t2);
// Per-mesh joint binding buffer (set 1, binding 0; stride 32).
StructuredBuffer<MeshVertexJointBinding> joint_binding : register(t0, space1);

struct VsInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 texcoord : TEXCOORD0;
};

struct VsOutput
{
    float4 position : SV_POSITION;
    float3 world_position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float2 texcoord : TEXCOORD3;
};

VsOutput main(VsInput input, uint instance_id : SV_InstanceID, uint vertex_id : SV_VertexID)
{
    MeshInstance instance = mesh_instances[instance_id];

    float3 model_position = input.position;
    float3 model_normal = input.normal;
    float3 model_tangent = input.tangent;

    if (instance.enable_vertex_blending > 0.0)
    {
        MeshVertexJointBinding binding = joint_binding[vertex_id];

        float4x4 blend_matrix =
            float4x4(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        float total_weight = 0.0;

        [unroll]
        for (uint influence = 0; influence < 4; ++influence)
        {
            const int joint_index = binding.indices[influence];
            const float weight = binding.weights[influence];
            if (weight > 0.0 && joint_index > 0)
            {
                blend_matrix += joint_matrices[kMeshVertexBlendingMaxJointCount * instance_id + joint_index] * weight;
                total_weight += weight;
            }
        }

        // total_weight == 0 means the vertex carries no usable influence: keep the
        // bind pose instead of collapsing it into the origin.
        if (total_weight > 0.0)
        {
            model_position = mul(blend_matrix, float4(input.position, 1.0)).xyz;

            float3x3 blend_tangent_matrix =
                float3x3(blend_matrix[0].xyz, blend_matrix[1].xyz, blend_matrix[2].xyz);
            model_normal = normalize(mul(blend_tangent_matrix, input.normal));
            model_tangent = normalize(mul(blend_tangent_matrix, input.tangent));
        }
    }

    float4 world = mul(instance.model_matrix, float4(model_position, 1.0f));
    float3 world_normal = normalize(mul((float3x3)instance.model_matrix, model_normal));
    float3 world_tangent = normalize(mul((float3x3)instance.model_matrix, model_tangent));

    VsOutput output;
    output.position = mul(proj_view_matrix, world);
    output.world_position = world.xyz;
    output.normal = world_normal;
    output.tangent = world_tangent;
    output.texcoord = input.texcoord;
    return output;
}
