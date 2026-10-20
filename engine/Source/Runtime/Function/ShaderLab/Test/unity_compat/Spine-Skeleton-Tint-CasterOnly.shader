// Spine/Skeleton Tint -- "Caster" pass only.
//
// This fixture exists because ShaderLabDx12Compiler::Compile() currently
// hands the WHOLE .shader file to ExtractHlslFromShaderLabFile(), which
// returns the FIRST program block it finds. Compiling pass 1 of the real
// two-pass shader therefore re-compiles pass 0. Isolating the Caster pass
// into its own file is the only way to actually exercise the shadow macros
// in the UnityCG shim.

Shader "Spine/Skeleton Tint CasterOnly" {
	Properties {
		[NoScaleOffset] _MainTex ("MainTex", 2D) = "black" {}
		_Cutoff ("Shadow alpha cutoff", Range(0,1)) = 0.1
	}

	SubShader {
		Tags { "Queue"="Transparent" "IgnoreProjector"="True" "RenderType"="Transparent" }

		Fog { Mode Off }
		Cull Off
		ZWrite On
		ZTest LEqual
		Lighting Off

		Pass {
			Name "Caster"
			Tags { "LightMode"="ShadowCaster" }
			Offset 1, 1

			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			#pragma multi_compile_shadowcaster
			#pragma fragmentoption ARB_precision_hint_fastest
			#include "UnityCG.cginc"
			sampler2D _MainTex;
			fixed _Cutoff;

			struct VertexOutput {
				V2F_SHADOW_CASTER;
				float2 uv : TEXCOORD1;
			};

			VertexOutput vert (appdata_base v) {
				VertexOutput o;
				o.uv = v.texcoord;
				TRANSFER_SHADOW_CASTER(o)
				return o;
			}

			float4 frag (VertexOutput i) : COLOR {
				fixed4 texcol = tex2D(_MainTex, i.uv);
				clip(texcol.a - _Cutoff);
				SHADOW_CASTER_FRAGMENT(i)
			}
			ENDCG
		}
	}
}
