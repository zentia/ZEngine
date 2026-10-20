// Diagnostic: which pass-level constructs make ShaderLabParser drop a pass?
// Each SubShader holds one variant; a missing Pass under a SubShader means
// that variant's syntax broke the parser.
Shader "Diag/PassVariants" {

	// --- SubShader 0: bare pass (control group) ---
	SubShader {
		Pass {
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	// --- SubShader 1: + Name "Caster" ---
	SubShader {
		Pass {
			Name "Caster"
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	// --- SubShader 2: + Tags { "LightMode"="ShadowCaster" } ---
	SubShader {
		Pass {
			Name "Caster"
			Tags { "LightMode"="ShadowCaster" }
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	// --- SubShader 3: + Offset 1, 1 ---
	SubShader {
		Pass {
			Name "Caster"
			Tags { "LightMode"="ShadowCaster" }
			Offset 1, 1
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}

	// --- SubShader 4: + Fog { Mode Off } / Lighting Off / ZWrite On / ZTest LEqual ---
	SubShader {
		Pass {
			Name "Caster"
			Tags { "LightMode"="ShadowCaster" }
			Offset 1, 1
			Fog { Mode Off }
			Cull Off
			Lighting Off
			ZWrite On
			ZTest LEqual
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}
}
