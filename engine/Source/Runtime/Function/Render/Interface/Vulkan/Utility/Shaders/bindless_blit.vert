// =====================================================================
// PR-V3: Bindless texture-blit vertex shader (Vulkan / GLSL 4.50)
// ---------------------------------------------------------------------
// Fullscreen-triangle VS driven by gl_VertexIndex. No vertex buffer,
// no input layout. The classic "draw 3, no IA" trick: one over-sized
// triangle that covers the entire NDC quad after rasterisation, with
// UVs in [0..1] mapped to clip-space [-1..3] / [-3..1].
//
// Compiled at runtime by VulkanRHI::createShaderModuleFromSource via
// VulkanBindlessTextureBlitPipeline::compileShaders.
//
// MUST stay byte-aligned with bindless_blit.frag on the v_uv location
// shape, otherwise vkCreateGraphicsPipelines will reject the linkage
// at PSO build time.
// =====================================================================
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Output to fragment stage.
layout(location = 0) out vec2 v_uv;

void main()
{
    // Fullscreen oversized triangle (Sascha Willems / V. Wennersten idiom):
    //   vid=0 -> ( -1, -1 ) uv=( 0, 0 )
    //   vid=1 -> (  3, -1 ) uv=( 2, 0 )
    //   vid=2 -> ( -1,  3 ) uv=( 0, 2 )
    // After clipping to NDC [-1..1] the visible portion has uv in
    // [0..1]. Saves three vertices and one index buffer over the
    // textbook quad approach -- standard idiom in DX12 / Vulkan
    // tutorials.
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = uv;

    // NOTE: Vulkan's NDC has Y-down (relative to GL), so we do NOT
    // flip Y here -- we want the same UV-down mapping the DX12 sibling
    // uses for editor / smoke-test parity (top-left origin).
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
