// Diagnostic: does Compile(subshader, pass, stage) actually honour `pass`?
//
// ShaderLabDx12Compiler::Compile() used to hand the whole .shader file to
// ExtractHlslFromShaderLabFile(), which returns the FIRST CGPROGRAM block it
// finds. Compiling pass 1 therefore silently produced pass 0's DXIL.
//
// The two passes below differ in entry-point name and in output colour, so:
//   * the reported entry point must differ between pass 0 and pass 1
//   * the produced DXIL must differ
// Before the fix both compiles returned vertA / identical DXIL.

Shader "Diag/TwoPasses" {
	SubShader {
		Pass {
			Name "First"
			CGPROGRAM
			#pragma vertex vertA
			#pragma fragment fragA
			float4 vertA (float4 v : POSITION) : SV_POSITION { return v; }
			float4 fragA (float4 i : SV_POSITION) : SV_Target { return float4(1, 0, 0, 1); }
			// vertA/vertB 故意取不同名字：入口名必须来自 #pragma，不能靠默认值。
			ENDCG
		}
		Pass {
			Name "Second"
			CGPROGRAM
			#pragma vertex vertB
			#pragma fragment fragB
			float4 vertB (float4 v : POSITION) : SV_POSITION { return v * float4(2, 2, 2, 2); }
			float4 fragB (float4 i : SV_POSITION) : SV_Target { return float4(0, 1, 0, 1); }
			ENDCG
		}
	}
}
