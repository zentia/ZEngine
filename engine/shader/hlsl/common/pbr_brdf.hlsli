#ifndef PBR_BRDF_HLSLI
#define PBR_BRDF_HLSLI

static const float PI = 3.14159265f;
static const float MAX_REFLECTION_LOD = 8.0f;

float D_GGX(float dotNH, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = dotNH * dotNH * (alpha2 - 1.0f) + 1.0f;
    return alpha2 / (PI * denom * denom);
}

float G_SchlicksmithGGX(float dotNL, float dotNV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    float gl = dotNL / (dotNL * (1.0f - k) + k);
    float gv = dotNV / (dotNV * (1.0f - k) + k);
    return gl * gv;
}

float3 F_Schlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
}

float3 F_SchlickR(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), f0) - f0) *
                     pow(1.0f - cosTheta, 5.0f);
}

float3 BRDF(float3 L, float3 V, float3 N, float3 f0, float3 basecolor, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float dotNV = saturate(dot(N, V));
    float dotNL = saturate(dot(N, L));
    float dotNH = saturate(dot(N, H));

    float rroughness = max(0.05f, roughness);
    float D = D_GGX(dotNH, rroughness);
    float G = G_SchlicksmithGGX(dotNL, dotNV, rroughness);
    float3 F = F_Schlick(dotNV, f0);

    float3 spec = D * F * G / (4.0f * dotNL * dotNV + 0.001f);
    float3 kD = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);
    return kD * basecolor / PI + spec;
}

float2 NdcxyToUv(float2 ndcxy) { return ndcxy * float2(0.5f, 0.5f) + float2(0.5f, 0.5f); }
float2 UvToNdcxy(float2 uv) { return uv * 2.0f - 1.0f; }

#endif
