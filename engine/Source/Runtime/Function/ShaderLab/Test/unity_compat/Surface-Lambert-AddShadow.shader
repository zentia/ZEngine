// Lambert 光照 + addshadow，并让 Input 同时使用 uv_/worldPos/viewDir/screenPos
// 四类 Unity 自动生成成员。用来覆盖 Standard 之外的另一条分支：
//   * SurfaceOutput（旧式输出结构，字段与 SurfaceOutputStandard 不同）
//   * addshadow -> 额外展开出一个 ShadowCaster pass
Shader "Custom/SurfaceLambertAddShadow"
{
    Properties
    {
        _Color ("Color", Color) = (1,1,1,1)
        _MainTex ("Albedo (RGB)", 2D) = "white" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        LOD 200

        CGPROGRAM
        #pragma surface surf Lambert addshadow

        sampler2D _MainTex;
        fixed4 _Color;

        struct Input
        {
            float2 uv_MainTex;
            float3 worldPos;
            float3 viewDir;
            float4 screenPos;
        };

        void surf (Input IN, inout SurfaceOutput o)
        {
            fixed4 c = tex2D (_MainTex, IN.uv_MainTex) * _Color;
            o.Albedo = c.rgb;
            o.Alpha = c.a;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
