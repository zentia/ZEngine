// Pick-pass SSBO payloads (C++ mirror: PickPassPerFrame / PickPassPerDrawcall in RenderCommon.h).

struct PickPassPerFrame
{
    float4x4 proj_view_matrix;
    uint rt_width;
    uint rt_height;
};

struct PickPassPerDrawcall
{
    float4x4 model_matrices[64];
    uint node_ids[64];
    float enable_vertex_blendings[64];
};
