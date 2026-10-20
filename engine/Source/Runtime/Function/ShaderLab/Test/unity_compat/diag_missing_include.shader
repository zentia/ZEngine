// Negative control for the include handler: the referenced file does not exist.
// A correct implementation must report a real DXC error, not silently
// comment the #include out and compile the rest.
Shader "Diag/MissingInclude" {
	SubShader {
		Pass {
			CGPROGRAM
			#pragma vertex vert
			#pragma fragment frag
			#include "ThisFileDoesNotExist.cginc"
			float4 vert (float4 v : POSITION) : SV_POSITION { return v; }
			float4 frag (float4 i : SV_POSITION) : SV_Target { return float4(1,0,0,1); }
			ENDCG
		}
	}
}
