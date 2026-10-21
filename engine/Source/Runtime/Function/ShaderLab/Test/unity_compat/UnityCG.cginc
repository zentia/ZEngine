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
//   * SurfaceOutput / SurfaceOutputStandard  (surface shaders)
//   * UNITY_INSTANCING_BUFFER_START / _END   (instancing)
//   * _LightColor0 / _WorldSpaceLightPos0 / _WorldSpaceCameraPos
//
// Anything DXC already knows (float4, mul, clip, SV_POSITION, TEXCOORDn) is
// intentionally NOT redefined here.
//
// NOTE: Unity spreads these over HLSLSupport.cginc, UnityShaderVariables.cginc,
// UnityCG.cginc and Lighting.cginc. This shim is one file standing in for all
// of them -- it is a compile-check artefact, not a port.

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
// Unity 的这几个宏展开后自带分号，所以调用处写的是
//     TRANSFER_SHADOW_CASTER(o)      <- 没有分号
//     SHADOW_CASTER_FRAGMENT(i)      <- 没有分号
// shim 必须保持一致，否则 DXC 报 "expected ';' after expression"。
#define V2F_SHADOW_CASTER         float4 shadowPos : SV_POSITION
#define TRANSFER_SHADOW_CASTER(o)  o.shadowPos = UnityObjectToClipPos(v.vertex);
#define SHADOW_CASTER_FRAGMENT(i)  return float4(0, 0, 0, 0);

// ---------------------------------------------------------------------------
// 6. Surface shader output structs.
//
// Unity defines these in Lighting.cginc, which a surface shader never
// includes by hand -- the surface compiler pulls it in. Field-for-field with
// Unity so an unmodified `void surf (Input IN, inout SurfaceOutputStandard o)`
// compiles and so the generated lighting code can read every field.
// ---------------------------------------------------------------------------
struct SurfaceOutputStandard
{
    float3 Albedo;      // base (diffuse or specular) color
    float3 Normal;      // tangent-space normal; (0,0,1) = unperturbed
    float3 Emission;
    float Metallic;     // 0 = non-metal, 1 = metal
    float Smoothness;   // 0 = rough, 1 = smooth
    float Occlusion;    // ambient occlusion, default 1
    float Alpha;        // alpha for transparencies
};

struct SurfaceOutput
{
    float3 Albedo;
    float3 Normal;
    float3 Emission;
    float Specular;
    float Gloss;
    float Alpha;
};

// ---------------------------------------------------------------------------
// 7. Instancing buffer macros.
//
// Unity's real expansion declares a cbuffer whose members are ARRAYS sized by
// UNITY_INSTANCED_ARRAY_SIZE (one slot per instance in the batch) and whose
// accessors index it with the instance ID. ZEngine has no instancing path
// wired to ShaderLab yet, so this is the degenerate one-instance form: a
// plain cbuffer with plain members.
//
// That keeps the ubiquitous
//     UNITY_INSTANCING_BUFFER_START(Props)
//         UNITY_DEFINE_INSTANCED_PROP(float4, _Color)
//     UNITY_INSTANCING_BUFFER_END(Props)
//     ... UNITY_ACCESS_INSTANCED_PROP(Props, _Color) ...
// syntactically valid and type-correct, which is all a compile check needs.
// ACCESS is included because a buffer you cannot read from is useless, and
// reading a plain member is the correct one-instance behaviour anyway.
//
// Still NOT provided -- these would compile into something that looks right
// and quietly drops instancing, which is worse than a build error:
//   UNITY_VERTEX_INPUT_INSTANCE_ID, UNITY_SETUP_INSTANCE_ID,
//   UNITY_TRANSFER_INSTANCE_ID, UNITY_INSTANCED_ARRAY_SIZE.
// ---------------------------------------------------------------------------
#define UNITY_INSTANCING_BUFFER_START(name)      cbuffer UnityInstancing_##name {
#define UNITY_INSTANCING_BUFFER_END(name)        };
#define UNITY_DEFINE_INSTANCED_PROP(type, name)  type name;
#define UNITY_ACCESS_INSTANCED_PROP(buffer, name) name

// ---------------------------------------------------------------------------
// 8. Built-in lighting globals.
//
// UnityShaderVariables.cginc / Lighting.cginc. The surface-shader desugaring
// (ShaderLabSurface.cpp) generates lighting code that reads these, exactly as
// Unity's generated passes do.
// ---------------------------------------------------------------------------
float4 _LightColor0;
float4 _WorldSpaceLightPos0;
float3 _WorldSpaceCameraPos;

// 历史记录：Spine 的 Caster pass 写 `o.uv = v.texcoord;`，uv 是 float2 而
// appdata_base.texcoord 是 float4（与 Unity 一致）。HLSL 允许这种隐式截断，
// FXC 只给警告。ZEngine 的 DX12 后端曾经在 _DEBUG 下给 DXC 传 -WX，把它升级成
// error（implicit truncation of vector type [-Werror,-Wconversion]），于是 Debug
// 成了唯一编不过的配置。-WX 现在由 r.Shaders.WarningsAsErrors 控制、默认关闭。
// 这里保持 float4 不迁就，把差异留在明面上。

#endif  // ZENGINE_UNITYCG_SHIM_INCLUDED
