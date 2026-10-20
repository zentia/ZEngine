// ---------------------------------------------------------------------------
// ZEngine ShaderLab -- HLSL common header (Unity-flavoured helper layer).
//
// Consumed by HLSLPROGRAM blocks, e.g. engine/shader/example_standard.shader.
//
// Language split -- IMPORTANT:
//   * The GLSL shaders under engine/shader/glsl/ do NOT include this file.
//     They include constants.h (+ gbuffer.h) instead, and are compiled by
//     glslangValidator.
//   * gbuffer.h is shared by BOTH languages. It branches on ZENGINE_HLSL,
//     which is defined right here -- so a shader that needs it must include
//     common.h BEFORE gbuffer.h. (GLSL consumers never define the macro and
//     therefore keep getting the GLSL branch.)
//
// This is a compatibility shim, not the RP1/RP2 bindless pipeline: the real
// pipeline headers live under engine/shader/hlsl/ and bind explicitly with
// register(b0, space2) etc. Here the Unity-style globals are declared without
// explicit slots so DXC assigns them, which keeps the shim self-consistent.
// ---------------------------------------------------------------------------

#ifndef ZENGINE_SHADER_INCLUDE_COMMON_H
#define ZENGINE_SHADER_INCLUDE_COMMON_H

// Dialect marker consumed by gbuffer.h. See the note above about include order.
#define ZENGINE_HLSL 1

// ---------------------------------------------------------------------------
// Constant buffers
// ---------------------------------------------------------------------------
// Unity's Core.hlsl spelling, minus the instancing branches.
#define CBUFFER_START(name) cbuffer name {
#define CBUFFER_END }

// ---------------------------------------------------------------------------
// Unity built-in transforms / camera
// ---------------------------------------------------------------------------
cbuffer UnityPerDraw
{
    float4x4 unity_ObjectToWorld;
    float4x4 unity_WorldToObject;
};

cbuffer UnityPerCamera
{
    float4x4 unity_MatrixVP;
    float4 _WorldSpaceCameraPos;
};

float4x4 GetObjectToWorldMatrix() { return unity_ObjectToWorld; }
float4x4 GetWorldToObjectMatrix() { return unity_WorldToObject; }
float4x4 GetViewProjectionMatrix() { return unity_MatrixVP; }

float3 GetCameraPosition() { return _WorldSpaceCameraPos.xyz; }

// HLSL requires declaration before use, so keep the dependency order:
// object->world first, then the clip-space helper built on top of it.
float3 TransformObjectToWorld(float3 positionOS)
{
    return mul(GetObjectToWorldMatrix(), float4(positionOS, 1.0)).xyz;
}

// Object space -> clip space.
float4 TransformObjectToHClip(float3 positionOS)
{
    return mul(GetViewProjectionMatrix(), float4(TransformObjectToWorld(positionOS), 1.0));
}

// Direction: no translation, no normalisation (caller decides).
float3 TransformObjectToWorldDir(float3 dirOS)
{
    return mul((float3x3)GetObjectToWorldMatrix(), dirOS);
}

// Normal: inverse-transpose, i.e. transpose of the world-to-object 3x3.
// HLSL mul(vector, matrix) treats the vector as a row, which is exactly the
// transpose we need, so no extra matrix has to be shipped per draw.
float3 TransformObjectToWorldNormal(float3 normalOS)
{
    return normalize(mul(normalOS, (float3x3)GetWorldToObjectMatrix()));
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
// Unity SRP spelling. A shader must declare its resources before sampling:
//
//     TEXTURE2D(_BaseMap);
//     SAMPLER(sampler_BaseMap);
//     ...
//     SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, uv);
//
// (The older Cg `tex2D(_BaseMap, uv)` two-argument form is intentionally NOT
// provided: HLSL cannot declare a texture inside an expression, so the
// sampler has to be named explicitly.)
#define TEXTURE2D(textureName) Texture2D textureName
#define SAMPLER(samplerName) SamplerState samplerName
#define SAMPLE_TEXTURE2D(textureName, samplerName, coord2) textureName.Sample(samplerName, coord2)

// Unity's standard tiling/offset transform: needs `<name>_ST` in the material
// cbuffer, which is what the `_BaseMap_ST` property declaration supplies.
#define TRANSFORM_TEX(tex, name) ((tex).xy * name##_ST.xy + name##_ST.zw)

// ---------------------------------------------------------------------------
// Instancing / stereo
// ---------------------------------------------------------------------------
// No-op unless the pass opts in. Unity expands these to real code only when
// UNITY_ANY_INSTANCING_ENABLED / UNITY_STEREO_INSTANCING_ENABLED are set
// (driven by #pragma multi_compile_instancing), and example_standard.shader
// does not use those pragmas -- so a GPU-instancing-free build is correct here
// rather than a silent trap.
#define UNITY_VERTEX_INPUT_INSTANCE_ID
#define UNITY_VERTEX_OUTPUT_STEREO
#define UNITY_SETUP_INSTANCE_ID(input)
#define UNITY_TRANSFER_INSTANCE_ID(input, output)
#define UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output)
#define UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input)

#endif  // ZENGINE_SHADER_INCLUDE_COMMON_H
