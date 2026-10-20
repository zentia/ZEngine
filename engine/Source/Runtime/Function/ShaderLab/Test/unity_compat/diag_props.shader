// Diagnostic: which Unity property attribute forms does ShaderLabParser keep?
Shader "Diag/Props" {
	Properties {
		_TexPlain      ("TexPlain", 2D) = "black" {}
		[NoScaleOffset] _TexNoScale   ("TexNoScale", 2D) = "black" {}
		[HideInInspector] _FltHidden  ("FltHidden", Float) = 1.0
		[Toggle(_X)] _IntToggle       ("IntToggle", Int) = 0
		[Enum(UnityEngine.Rendering.CompareFunction)] _FltEnum ("FltEnum", Float) = 8
		_Col ("Col", Color) = (1,1,1,1)
		_Rng ("Rng", Range(0,1)) = 0.1
		_Vec ("Vec", Vector) = (0,0,0,0)
		_Flt ("Flt", Float) = 0.5
		_Int ("Int", Int) = 3
		_Cub ("Cub", Cube) = "" {}
		_Tex3 ("Tex3", 3D) = "" {}
	}

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
}
