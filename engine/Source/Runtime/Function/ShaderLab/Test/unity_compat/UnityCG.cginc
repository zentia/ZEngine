// UnityCG.cginc -- MINIMAL ZEngine compatibility shim.
//
// Purpose: give ZEngine's ShaderLab -> DXC (DXIL) path just enough of Unity's
// built-in shader include to compile the CGPROGRAM body of a real Unity shader
// (Spine/Skeleton Tint) without editing the shader itself.
//
// This is a VERIFICATION ARTEFACT, not a production include. It deliberately
// covers only the built-ins that DXC does not natively provide:
//
//   * fixed / half precision aliases  -- Cg-only types, unknown to DXC
//   * sampler2D + tex2D               -- D3D9-style texture/sampler pair,
//                                        replaced by Texture2D + SamplerState
//   * UNITY_MATRIX_MVP / unity_*
//   * UnityObjectToClipPos
//   * appdata_base
//   * V2F_SHADOW_CASTER / TRANSFER_SHADOW_CASTER / SHADOW_CASTER_FRAGMENT
//
// Anything DXC already knows (float4, mul, clip, SV_POSITION, TEXCOORDn) is
// intentionally NOT redefined here.

#ifndef ZENGINE_UNITYCG_SHIM_INCLUDED
#define ZENGINE_UNITYCG_SHIM_INCLUDED

// ---------------------------------------------------------------------------
// 1. Precision aliases.
//
// Unity's `fixed`/`half` are real Cg types. DXC has no such type, so they
// collapse to `float`. This matches what Unity itself emits on most modern
// platforms (see HLSLSupport.cginc: fixed is float, half is float on desktop
// SM5+ targets).
// ---------------------------------------------------------------------------
#define fixed  float
#define fixed2 float2
#define fixed3 float3
#define fixed4 float4
#define half   float
#define half2  float2
#define half3  float3
#define half4  float4

// ---------------------------------------------------------------------------
// 2. Legacy D3D9 texture / sampler pair.
//
// Unity declares `sampler2D _MainTex;` and samples with `tex2D(_MainTex, uv)`.
// HLSL 2021 / DXC have neither keyword. We map the declaration to Texture2D
// and the sampling to Texture2D::Sample against one shared linear sampler.
//
// NOTE: this is the one place where the shim is a *semantic* choice, not a
// purely mechanical alias -- a real port would bind a per-texture sampler from
// the material's sampler state (Unity does exactly this: SamplerState
// sampler_MainTex). For a compile-only check the shared sampler is enough.
// ---------------------------------------------------------------------------
#define sampler2D Texture2D
SamplerState unity_SharedLinearSampler;
#define tex2D(tex, uv) tex.Sample(unity_SharedLinearSampler, (uv))

// ---------------------------------------------------------------------------
// 3. Transform matrices.
//
// Unity exposes these through UnityPerDraw / UnityPerFrame constant buffers.
// ZEngine has no equivalent yet, so the shim declares them in a single cbuffer
// that the DX12 backend would have to fill at bind time.
// ---------------------------------------------------------------------------
cbuffer UnityPerDraw
{
    float4x4 unity_ObjectToWorld;
    float4x4 unity_WorldToObject;
    float4x4 unity_MatrixVP;
    float4x4 unity_MatrixV;
    float4x4 unity_MatrixP;
    float4x4 unity_MatrixInvV;
};

// Unity's own definition: UNITY_MATRIX_MVP == mul(unity_MatrixVP, unity_ObjectToWorld)
#define UNITY_MATRIX_MVP mul(unity_MatrixVP, unity_ObjectToWorld)
#define UNITY_MATRIX_MV  mul(unity_MatrixV, unity_ObjectToWorld)

float4 UnityObjectToClipPos(float4 vertex)
{
    return mul(UNITY_MATRIX_MVP, float4(vertex.xyz, 1.0));
}

float4 UnityObjectToViewPos(float4 vertex)
{
    return mul(UNITY_MATRIX_MV, vertex);
}

float3 UnityObjectToWorldDir(float3 dir)
{
    return mul((float3x3)unity_ObjectToWorld, dir);
}

float4 UnityWorldToClipPos(float3 worldPos)
{
    return mul(unity_MatrixVP, float4(worldPos, 1.0));
}

// ---------------------------------------------------------------------------
// 4. Built-in appdata structs.
// ---------------------------------------------------------------------------
struct appdata_base
{
    float4 vertex   : POSITION;
    float3 normal   : NORMAL;
    float4 texcoord : TEXCOORD0;
};

struct appdata_tan
{
    float4 vertex   : POSITION;
    float4 tangent  : TANGENT;
    float3 normal   : NORMAL;
    float4 texcoord : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// 5. Shadow-caster macros (stubs).
//
// Spine's "Caster" pass uses Unity's shadow macros. ZEngine has no shadow
// pipeline wired to ShaderLab yet, so these are intentionally inert: they keep
// the pass syntactically valid so we can measure whether the *rest* of the
// Unity syntax parses and compiles.
// ---------------------------------------------------------------------------
#define V2F_SHADOW_CASTER        float4 shadowPos : SV_POSITION
#define TRANSFER_SHADOW_CASTER(o) o.shadowPos = UnityObjectToClipPos(v.vertex)
#define SHADOW_CASTER_FRAGMENT(i) return float4(0, 0, 0, 0)

#endif  // ZENGINE_UNITYCG_SHIM_INCLUDED
