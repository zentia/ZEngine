// =====================================================================
// PR-V4 part 1: bindless tone-mapping fragment shader
// ---------------------------------------------------------------------
// Sibling of `tone_mapping.frag` (the legacy `subpassInput` version).
// The legacy shader reads its HDR input as a Vulkan input attachment
// (subpassInput / subpassLoad), which forces the tonemap pass to live
// inside the main_camera render pass as a subpass with a tile-memory
// dependency on the preceding lighting subpass. That input-attachment
// shape is fundamentally incompatible with bindless descriptor-indexed
// texture loads -- a `subpassInput` is a per-fragment fixed-function
// binding, not a `sampler2D`.
//
// This shader is the "what tone-mapping looks like once the input is a
// regular sampled image" pivot. Once the prerequisite "lift tonemap
// out of the main_camera render pass into a standalone pass" PR lands
// (tracked as PR-V4 part 2 in BINDLESS_TEXTURE_PATH.md), this shader
// becomes the production fragment shader for that standalone pass.
//
// Bindless wiring (matches the `VulkanBindlessTextureBlitPipeline`
// template exactly so the consumer-side glue is byte-identical):
//   - set 0, binding 0 = `sampler2D u_bindless[]` -- the global bindless
//     descriptor table owned by `VulkanBindlessTextureManager`.
//   - 4-byte push constant at offset 0 = `BindlessIndex::pack(tex,
//     sampler)`. Low 16 bits = bindless slot index; high 16 bits are
//     ignored on Vulkan (the slot's sampler was bound at allocate()
//     time -- see PR-V3 semantic divergence note 2 in
//     BINDLESS_TEXTURE_PATH.md).
//
// Tone-mapping math is byte-identical to the legacy shader -- same
// Uncharted2 curve, same exposure (* 4.5f), same Gamma 2.2 correction
// path. The intent is that swapping shaders in the standalone pass is
// a pixel-equivalent change, isolating any visual delta to the
// pipeline-topology change rather than the tone-mapping algorithm.
//
// GLSL version note: the legacy shader uses `#version 310 es` because
// it ships on Adreno / Mali / Apple. The bindless variant requires
// `GL_EXT_nonuniform_qualifier`, which is a desktop / Vulkan 1.2-class
// extension; we therefore follow the bindless_blit.frag template and
// declare `#version 450`. If a future mobile bindless story is needed,
// the right answer is `GL_KHR_shader_subgroup_uniform_control_flow` +
// `nonuniformEXT()` on a 320 es profile -- a separate cross-cutting
// PR, not this one.
// =====================================================================

#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier     : require

layout(set = 0, binding = 0) uniform sampler2D u_bindless[];

layout(push_constant) uniform BindlessPush
{
    uint g_packed_indices;
} u_push;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 Uncharted2Tonemap(vec3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

void main()
{
    uint texture_index = u_push.g_packed_indices & 0xFFFFu;
    vec3 color = texture(u_bindless[nonuniformEXT(texture_index)], v_uv).rgb;

    // Same Uncharted2 curve as the legacy `tone_mapping.frag`.
    color = Uncharted2Tonemap(color * 4.5);
    color = color * (1.0 / Uncharted2Tonemap(vec3(11.2)));

    // Gamma 2.2 correction. See the legacy shader for the rationale on
    // doing this in-shader vs picking a `_SRGB` swapchain format -- not
    // changing it here keeps the pixel-equivalence guarantee.
    color = vec3(pow(color.x, 1.0 / 2.2),
                 pow(color.y, 1.0 / 2.2),
                 pow(color.z, 1.0 / 2.2));

    o_color = vec4(color, 1.0);
}
