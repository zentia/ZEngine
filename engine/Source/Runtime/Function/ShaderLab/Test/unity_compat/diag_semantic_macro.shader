// Diagnostic: can a #define in the shim rewrite a legacy output semantic?
// DXC rejected `: COLOR` for ps 6.0 on the real Spine shader. If the HLSL
// preprocessor expands macros in semantic position, a shim-side
// `#define COLOR SV_Target` would fix Unity shaders without editing them.
// If it does not expand (or DXC still rejects it), the fix has to be a
// post-extraction source rewrite inside ZEngine instead.
Shader "Diag/SemanticMacro" {
	SubShader {
		Pass {
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			#define COLOR SV_Target
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : COLOR { return float4(1,0,0,1); }
			ENDCG
		}
	}
}
