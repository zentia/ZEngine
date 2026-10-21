// Standard 光照 + 法线贴图 + alphatest + addshadow。
//
// 覆盖前两轮 surface shader 支持里明确留空的三个点：
//   * o.Normal 参与光照 —— 它是切线空间法线，必须经 TBN 转到世界空间，
//     否则法线贴图完全不起作用
//   * alphatest:_Cutoff —— Unity 在 surf 之后插入 clip(o.Alpha - _Cutoff)，
//     并且 ShadowCaster 也要 clip，否则镂空物体的影子是实心的
//   * _Cutoff 只出现在 Properties 里，CGPROGRAM 体内没有声明
Shader "Custom/SurfaceNormalMapAlphaTest"
{
    Properties
    {
        _Color ("Color", Color) = (1,1,1,1)
        _MainTex ("Albedo (RGB)", 2D) = "white" {}
        _BumpMap ("Normal Map", 2D) = "bump" {}
        _Cutoff ("Alpha Cutoff", Range(0,1)) = 0.5
    }
    SubShader
    {
        Tags { "RenderType"="TransparentCutout" "Queue"="AlphaTest" }
        LOD 200

        CGPROGRAM
        #pragma surface surf Standard alphatest:_Cutoff addshadow

        sampler2D _MainTex;
        sampler2D _BumpMap;
        fixed4 _Color;

        struct Input
        {
            float2 uv_MainTex;
            float2 uv_BumpMap;
        };

        void surf (Input IN, inout SurfaceOutputStandard o)
        {
            fixed4 c = tex2D (_MainTex, IN.uv_MainTex) * _Color;
            o.Albedo = c.rgb;
            o.Normal = UnpackNormal (tex2D (_BumpMap, IN.uv_BumpMap));
            o.Alpha = c.a;
        }
        ENDCG
    }
    FallBack "Transparent/Cutout/Diffuse"
}
