// Diagnostic round 2: is it the TYPE or the DEFAULT that loses a texture prop?
// diag_props showed 2D and 3D props missing while Cube survived.
Shader "Diag/Props2" {
	Properties {
		_T2D_black   ("T2D_black", 2D) = "black" {}
		_T2D_empty   ("T2D_empty", 2D) = "" {}
		_T2D_white   ("T2D_white", 2D) = "white" {}
		_T2D_nospace ("T2D_nospace", 2D) = "black" { }
		_T3D_empty   ("T3D_empty", 3D) = "" {}
		_TCube_empty ("TCube_empty", Cube) = "" {}
		_T2DArray    ("T2DArray", 2DArray) = "" {}
		_TAny        ("TAny", Any) = "" {}
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
