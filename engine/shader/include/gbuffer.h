// ---------------------------------------------------------------------------
// Shared by BOTH shader languages.
//
//   * GLSL (engine/shader/glsl/*.frag, compiled by glslangValidator) includes
//     this file directly and gets the branch below.
//   * HLSL (ShaderLab HLSLPROGRAM blocks, compiled by DXC) includes
//     common.h first, which defines ZENGINE_HLSL, so it gets the branch above.
//     => HLSL consumers MUST include common.h before this file.
//
// Both sides describe the same GBuffer layout: THREE render targets, in
// normal / material / albedo order (see EncodeGBufferData below and
// engine/shader/hlsl/rp1/mesh_gbuffer.frag.hlsl). There is no emissive
// target -- every channel of all three is already spoken for.
// ---------------------------------------------------------------------------

#ifdef ZENGINE_HLSL

// MRT output of the GBuffer pass -- EXACTLY THREE render targets.
//
// The RP1 render pass owns gbuffer_a/b/c and nothing else: RenderPass.h
// defines no fourth attachment and CreateRuntimeMeshPipeline(..., 3) binds
// three. A fragment shader that declares a fourth SV_Target cannot form a PSO
// against that render pass, so the material silently falls back to the
// built-in mesh_gbuffer shader and everything the author wrote is discarded.
//
//   GBuffer0 : SV_Target0 = EncodeNormal(worldNormal)   (.a unused)
//   GBuffer1 : SV_Target1 = metallic, specular, roughness, shading model id
//   GBuffer2 : SV_Target2 = baseColor                   (.a unused)
struct GBufferOutput
{
    float4 GBuffer0 : SV_Target0;
    float4 GBuffer1 : SV_Target1;
    float4 GBuffer2 : SV_Target2;
};

#define SHADINGMODELID_UNLIT 0u
#define SHADINGMODELID_DEFAULT_LIT 1u
#define SHADINGMODELID_SKY 2u

// Mirrors of the GLSL helpers below, so a ShaderLab pass can write the layout
// instead of hand-rolling the packing (and getting the target order wrong).
float3 EncodeNormal(float3 N) { return N * 0.5 + 0.5; }
float3 DecodeNormal(float3 N) { return N * 2.0 - 1.0; }
float  EncodeShadingModelId(uint id) { return float(id) / 255.0; }
uint   DecodeShadingModelId(float packed) { return (uint)round(packed * 255.0); }

#else  // ---- GLSL ----

struct PGBufferData
{
    highp vec3  worldNormal;
    highp vec3  baseColor;
    highp float metallic;
    highp float specular;
    highp float roughness;
    highp uint  shadingModelID;
};

#define SHADINGMODELID_UNLIT 0U
#define SHADINGMODELID_DEFAULT_LIT 1U
#define SHADINGMODELID_SKY 2U

highp vec3 EncodeNormal(highp vec3 N) { return N * 0.5 + 0.5; }

highp vec3 DecodeNormal(highp vec3 N) { return N * 2.0 - 1.0; }

highp vec3 EncodeBaseColor(highp vec3 baseColor)
{
    // we use sRGB on the render target to give more precision to the darks
    return baseColor;
}

highp vec3 DecodeBaseColor(highp vec3 baseColor)
{
    // we use sRGB on the render target to give more precision to the darks
    return baseColor;
}

highp float EncodeShadingModelId(highp uint ShadingModelId)
{
    highp uint Value = ShadingModelId;
    return (float(Value) / float(255));
}

highp uint DecodeShadingModelId(highp float InPackedChannel) { return uint(round(InPackedChannel * float(255))); }

void EncodeGBufferData(PGBufferData   InGBuffer,
                       out highp vec4 OutGBufferA,
                       out highp vec4 OutGBufferB,
                       out highp vec4 OutGBufferC)
{
    OutGBufferA.rgb = EncodeNormal(InGBuffer.worldNormal);

    OutGBufferB.r = InGBuffer.metallic;
    OutGBufferB.g = InGBuffer.specular;
    OutGBufferB.b = InGBuffer.roughness;
    OutGBufferB.a = EncodeShadingModelId(InGBuffer.shadingModelID);

    OutGBufferC.rgb = EncodeBaseColor(InGBuffer.baseColor);
}

void DecodeGBufferData(out PGBufferData OutGBuffer, highp vec4 InGBufferA, highp vec4 InGBufferB, highp vec4 InGBufferC)
{
    OutGBuffer.worldNormal = DecodeNormal(InGBufferA.xyz);

    OutGBuffer.metallic       = InGBufferB.r;
    OutGBuffer.specular       = InGBufferB.g;
    OutGBuffer.roughness      = InGBufferB.b;
    OutGBuffer.shadingModelID = DecodeShadingModelId(InGBufferB.a);

    OutGBuffer.baseColor = DecodeBaseColor(InGBufferC.rgb);
}

#endif  // ZENGINE_HLSL