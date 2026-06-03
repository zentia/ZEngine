// ZEngine Standard Shader 示例 (.shader)
// 这是一个基于 ShaderLab 语法的示例文件

Shader "ZEngine/Standard"
{
    Properties
    {
        // Base Color
        _BaseColor ("Base Color", Color) = (1, 1, 1, 1)
        
        // Base Map (Albedo)
        _BaseMap ("Base Map", 2D) = "white" {}
        
        // PBR 参数
        _Metallic ("Metallic", Range(0, 1)) = 0
        _Smoothness ("Smoothness", Range(0, 1)) = 0.5
        
        // Normal Map
        _NormalMap ("Normal Map", 2D) = "bump" {}
        _NormalScale ("Normal Scale", Range(0, 1)) = 1
        
        // Occlusion
        _OcclusionMap ("Occlusion Map", 2D) = "white" {}
        
        // Emission
        _EmissionColor ("Emission Color", Color) = (0, 0, 0, 1)
        _EmissionMap ("Emission Map", 2D) = "white" {}
        
        // Alpha Test
        [Toggle(_ALPHATEST_ON)] _AlphaTest ("Alpha Test", Float) = 0
        _Cutoff ("Cutoff", Range(0, 1)) = 0.5
    }
    
    SubShader
    {
        Tags 
        { 
            "RenderPipeline" = "Deferred"
            "Queue" = "Geometry"
            "RenderType" = "Opaque"
        }
        LOD 100
        
        // GBuffer Pass
        Pass
        {
            Name "GBuffer"
            Tags { "LightMode" = "GBuffer" }
            
            // 渲染状态
            ZWrite On
            ZTest LEqual
            Cull Back
            
            HLSLPROGRAM
            
            #pragma vertex StandardVert
            #pragma fragment StandardFrag
            
            // Shader 变体
            #pragma multi_compile _ LIGHTMAP_ON
            #pragma multi_compile _ DIRLIGHTMAP_COMBINED
            #pragma multi_compile _ DYNAMICLIGHTMAP_ON
            #pragma multi_compile _ SHADOWS_SHADOWMASK
            #pragma multi_compile_fragment _ SHADOWS_SOFT
            
            #pragma shader_feature _ _ALPHATEST_ON
            #pragma shader_feature _ _EMISSION
            
            #include "shader/include/common.h"
            #include "shader/include/gbuffer.h"
            
            CBUFFER_START(UnityPerMaterial)
                float4 _BaseColor;
                float4 _BaseMap_ST;
                float  _Metallic;
                float  _Smoothness;
                float  _NormalScale;
                float4 _EmissionColor;
                float  _Cutoff;
            CBUFFER_END
            
            struct appdata
            {
                float4 position : POSITION;
                float3 normal : NORMAL;
                float4 tangent : TANGENT;
                float2 uv : TEXCOORD0;
                float2 uv1 : TEXCOORD1;
                float2 uv2 : TEXCOORD2;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            
            struct v2f
            {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
                float3 normal : TEXCOORD1;
                float3 tangent : TEXCOORD2;
                float3 bitangent : TEXCOORD3;
                UNITY_VERTEX_INPUT_INSTANCE_ID
                UNITY_VERTEX_OUTPUT_STEREO
            };
            
            v2f StandardVert(appdata input)
            {
                v2f output;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_TRANSFER_INSTANCE_ID(input, output);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);
                
                output.position = TransformObjectToHClip(input.position.xyz);
                output.uv = TRANSFORM_TEX(input.uv, _BaseMap);
                output.normal = TransformObjectToWorldNormal(input.normal);
                output.tangent = TransformObjectToWorldDir(input.tangent.xyz);
                output.bitangent = cross(output.normal, output.tangent) * input.tangent.w;
                
                return output;
            }
            
            void StandardFrag(v2f input, out GBufferOutput output)
            {
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);
                
                // 采样贴图
                float4 albedo = SAMPLE_TEXTURE2D(_BaseMap, input.uv) * _BaseColor;
                
                #if defined(_ALPHATEST_ON)
                    clip(albedo.a - _Cutoff);
                #endif
                
                // Normal
                float3 normal = normalize(input.normal);
                
                // 输出 GBuffer
                output.GBuffer0 = float4(albedo.rgb, _Metallic);
                output.GBuffer1 = float4(_Smoothness, 0, 0, 0);
                output.GBuffer2 = float4(normal * 0.5 + 0.5, 1);
                
                #if defined(_EMISSION)
                    float4 emission = SAMPLE_TEXTURE2D(_EmissionMap, input.uv) * _EmissionColor;
                    output.GBuffer3 = emission.rgb;
                #else
                    output.GBuffer3 = float3(0, 0, 0);
                #endif
            }
            ENDHLSL
        }
        
        // Forward Lighting Pass (for transparent objects)
        Pass
        {
            Name "ForwardLit"
            Tags { "LightMode" = "ForwardLit" }
            
            // Transparent settings
            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite Off
            Cull Off
            
            HLSLPROGRAM
            
            #pragma vertex StandardVert
            #pragma fragment ForwardFrag
            
            #pragma multi_compile _ LIGHTMAP_ON
            #pragma multi_compile _ DIRECTIONAL
            
            #include "shader/include/common.h"
            
            CBUFFER_START(UnityPerMaterial)
                float4 _BaseColor;
                float4 _BaseMap_ST;
                float  _Metallic;
                float  _Smoothness;
                float  _NormalScale;
                float4 _EmissionColor;
            CBUFFER_END
            
            struct appdata
            {
                float4 position : POSITION;
                float3 normal : NORMAL;
                float2 uv : TEXCOORD0;
            };
            
            struct v2f
            {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
                float3 worldPos : TEXCOORD1;
                float3 normal : TEXCOORD2;
            };
            
            v2f StandardVert(appdata input)
            {
                v2f output;
                output.position = TransformObjectToHClip(input.position.xyz);
                output.uv = TRANSFORM_TEX(input.uv, _BaseMap);
                output.worldPos = TransformObjectToWorld(input.position.xyz);
                output.normal = TransformObjectToWorldNormal(input.normal);
                return output;
            }
            
            float4 ForwardFrag(v2f input) : SV_Target
            {
                float4 albedo = SAMPLE_TEXTURE2D(_BaseMap, input.uv) * _BaseColor;
                float3 viewDir = normalize(GetCameraPosition() - input.worldPos);
                float3 normal = normalize(input.normal);
                
                // Simple diffuse
                float3 lightDir = normalize(float3(1, 1, 1));
                float3 diffuse = max(dot(normal, lightDir), 0) * albedo.rgb;
                
                return float4(diffuse, albedo.a);
            }
            ENDHLSL
        }
    }
    
    FallBack "ZEngine/Diffuse"
}
