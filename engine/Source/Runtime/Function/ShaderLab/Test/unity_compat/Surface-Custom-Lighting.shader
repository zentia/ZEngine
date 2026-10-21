// 自定义光照模型 + vertex 修改器 + finalcolor 钩子 + alpha 混合。
//
// `#pragma surface surf WrapLambert` 里的 WrapLambert 不是 Unity 内置模型，
// 它指的是本文件里的 LightingWrapLambert()。Unity 支持两种签名（不带/带
// viewDir），这里用的是三参数形式。
//
// vertex:myVert   —— Unity 用 appdata_full 调用作者写的顶点修改器，之后
//                    生成的顶点着色器必须用被改过的顶点继续算裁剪坐标
// finalcolor:myFinal —— 光照算完之后再回调一次，可以改最终颜色
// alpha            —— 走透明混合，此时 o.Alpha 不再被强制成 1
Shader "Custom/SurfaceCustomLighting"
{
    Properties
    {
        _Color ("Color", Color) = (1,1,1,1)
        _MainTex ("Albedo (RGB)", 2D) = "white" {}
    }
    SubShader
    {
        Tags { "RenderType"="Transparent" "Queue"="Transparent" }
        LOD 200

        CGPROGRAM
        #pragma surface surf WrapLambert alpha vertex:myVert finalcolor:myFinal

        sampler2D _MainTex;
        fixed4 _Color;

        struct Input
        {
            float2 uv_MainTex;
            float3 viewDir;
        };

        void myVert (inout appdata_full v)
        {
            v.vertex.xyz += v.normal * 0.01;
        }

        void myFinal (Input IN, SurfaceOutput o, inout fixed4 color)
        {
            color.rgb *= 0.5;
        }

        void surf (Input IN, inout SurfaceOutput o)
        {
            fixed4 c = tex2D (_MainTex, IN.uv_MainTex) * _Color;
            o.Albedo = c.rgb;
            o.Alpha = c.a;
        }

        half4 LightingWrapLambert (SurfaceOutput s, half3 lightDir, half atten)
        {
            half wrapped = dot (s.Normal, lightDir) * 0.5 + 0.5;
            return half4 (s.Albedo * _LightColor0.rgb * wrapped * atten, s.Alpha);
        }
        ENDCG
    }
    FallBack "Transparent/Diffuse"
}
