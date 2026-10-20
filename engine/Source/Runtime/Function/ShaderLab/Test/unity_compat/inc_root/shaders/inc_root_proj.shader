// Regression fixture: a PROJECT-RELATIVE #include path.
//
// `#include "unity_compat/inc_root/lib/shared.h"` only resolves when the
// include root is set to the fixture directory (the test uses
// ZENGINE_SHADERLAB_UNITY_COMPAT_TEST_DIR). Searching just the .shader's own
// directory -- which was the only thing Compile() ever did -- must fail.
Shader "ZEngineTest/IncludeRoot"
{
    Properties
    {
        _MainTex ("Texture", 2D) = "white" {}
    }

    SubShader
    {
        Tags { "RenderType"="Opaque" }

        Pass
        {
            Name "Default"

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag

            #include "unity_compat/inc_root/lib/shared.h"

            struct appdata
            {
                float4 vertex : POSITION;
            };

            struct v2f
            {
                float4 vertex : SV_POSITION;
            };

            v2f vert (appdata v)
            {
                v2f o;
                o.vertex = DoubleIt(v.vertex);
                return o;
            }

            float4 frag (v2f i) : SV_Target
            {
                return float4(1.0, 0.0, 0.0, 1.0);
            }
            ENDCG
        }
    }
}
