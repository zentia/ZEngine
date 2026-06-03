// =====================================================================
// PR-V3: Bindless texture-blit fragment shader (Vulkan / GLSL 4.50)
// ---------------------------------------------------------------------
// First production-path GLSL consumer of the Vulkan bindless toolchain.
// Samples a single sampler2D from the engine's global bindless
// descriptor table (owned by VulkanBindlessTextureManager) using
// VK_EXT_descriptor_indexing's nonuniformEXT qualifier.
//
// Bindless table shape (mirrors VulkanBindlessTextureManager exactly):
//   set    = 0          (kBindlessDescriptorSet)
//   binding= 0          (kBindlessTextureBinding)
//   type   = COMBINED_IMAGE_SAMPLER     (sampler2D[])
//   flags  = UPDATE_AFTER_BIND
//          | PARTIALLY_BOUND
//          | VARIABLE_DESCRIPTOR_COUNT
//
// Index-pack contract (rhi.h BindlessIndex namespace, mirrored from
// PR6 verbatim):
//   bits  [ 0..15] : texture_index   -- slot in the bindless table
//   bits  [16..31] : sampler_index   -- IGNORED on Vulkan (the slot's
//                                       sampler is fixed at manager
//                                       allocate time; the half is
//                                       reserved for ABI symmetry
//                                       with DX12).
//
// Push-constant block:
//   stage  = ALL  (matches VulkanBindlessTextureManager::
//                  getBindlessPushConstantRange)
//   offset = 0
//   size   = 4
// VulkanRHI::cmdSetBindlessIndexPFN writes a single uint32 here every
// time recordBlit() runs.
//
// nonuniformEXT note:
//   The shader compiler (glslang / shaderc) requires
//   `nonuniformEXT(idx)` whenever the descriptor-array index isn't
//   provably uniform across a draw -- otherwise the validation layer
//   fires VUID-RuntimeSpirv-NonUniform-06274. Even though our index
//   IS uniform (we push the same value for the whole draw), wrapping
//   it costs nothing and matches the DX12 sibling's
//   NonUniformResourceIndex usage 1:1.
// =====================================================================
#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier     : require

// MUST match VulkanBindlessTextureManager's set / binding / capacity
// shape. We use an unsized array because the manager allocates the set
// with VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT; the
// driver permits referencing only slots actually written via
// PARTIALLY_BOUND, so unused slots are safe.
layout(set = 0, binding = 0) uniform sampler2D u_bindless[];

layout(push_constant) uniform BindlessPush
{
    uint g_packed_indices;
} u_push;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main()
{
    // Twin of BindlessIndex::unpackTexture in rhi.h. The mask MUST
    // stay 0xFFFF; PR-V2's static_assert on
    // BindlessIndex::kTextureIndexMask is the cross-backend pin.
    uint texture_index = u_push.g_packed_indices & 0xFFFFu;

    // nonuniformEXT keeps the validation layer happy on
    // VK_EXT_descriptor_indexing without DescriptorRequirements
    // metadata. The intrinsic is the GLSL spelling of
    // SPV_EXT_descriptor_indexing's NonUniform decoration.
    o_color = texture(u_bindless[nonuniformEXT(texture_index)], v_uv);
}
