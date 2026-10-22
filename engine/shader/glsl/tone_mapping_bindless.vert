// =====================================================================
// Bindless tone-mapping vertex shader (pairs with tone_mapping_bindless.frag)
// ---------------------------------------------------------------------
// One SV_VertexID-driven oversized fullscreen triangle: 3 vertices, no
// vertex buffer. VulkanBindlessTonemapPipeline::RecordTonemap draws it
// with vkCmdDraw(3, 1, 0, 0) and an empty vertex-input state.
//
// Compiled at build time by engine/shader/CMakeLists.txt (glslangValidator
// runs from CMake, not from the editor) into
// shader/generated/cpp/tone_mapping_bindless_vert.h.
// =====================================================================

#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = uv;
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
