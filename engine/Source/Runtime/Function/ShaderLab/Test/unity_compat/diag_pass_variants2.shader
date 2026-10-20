// Diagnostic round 2: bisect WHICH pass-level command loses the CGPROGRAM.
// SubShader 0 is the control (bare pass). Every later SubShader adds exactly
// ONE command. `programs=0` under a SubShader means that command broke it.
Shader "Diag/PassVariants2" {

	SubShader { // 0 control
		Pass {
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 1 Fog block
		Pass {
			Fog { Mode Off }
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 2 Cull
		Pass {
			Cull Off
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 3 Lighting
		Pass {
			Lighting Off
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 4 ZWrite
		Pass {
			ZWrite On
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 5 ZTest
		Pass {
			ZTest LEqual
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 6 Blend (premultiply form used by Spine)
		Pass {
			Blend One OneMinusSrcAlpha
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 7 Stencil block with bracketed property refs (Spine form)
		Pass {
			Stencil {
				Ref[_StencilRef]
				Comp[_StencilComp]
				Pass Keep
			}
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	// --- Same commands, but at SubShader level instead of Pass level ---
	SubShader { // 8 SubShader-level Fog
		Fog { Mode Off }
		Pass {
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	SubShader { // 9 SubShader-level ZWrite/ZTest/Lighting/Cull
		Cull Off
		ZWrite On
		ZTest LEqual
		Lighting Off
		Pass {
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}
}
