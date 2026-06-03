# Material shader keyword smoke test (Inspector -> macros -> PSO)

Prerequisites: Debug `ZEditor.exe` built; demo project `I:\ZEngineDemo\ZEngineDemo.zproject`;
Node on PATH if you use project-local tsc (not required for this test).

## 1. Add a test shader under Shaders/

Create `I:\ZEngineDemo\Shaders\KeywordSmoke.shader` (or edit an existing `.shader`):

```shaderlab
Shader "Custom/KeywordSmoke"
{
    Properties
    {
        _BaseColor ("Base Color", Color) = (1, 1, 1, 1)
    }

    SubShader
    {
        Tags { "RenderPipeline" = "StandardLit" "Queue" = "Geometry" }

        Pass
        {
            Name "GBuffer"
            Tags { "LightMode" = "GBuffer" }

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile _ USE_FOG
            #pragma shader_feature _ DEBUG_VIEW

            cbuffer UnityPerMaterial { float4 _BaseColor; };

            struct Attributes { float4 position : POSITION; };
            struct Varyings { float4 position : SV_POSITION; };

            Varyings vert(Attributes input)
            {
                Varyings o;
                o.position = input.position;
                return o;
            }

            float4 frag(Varyings input) : SV_Target
            {
            #if defined(USE_FOG)
                return float4(0.2, 0.5, 0.9, 1) * _BaseColor;
            #elif defined(DEBUG_VIEW)
                return float4(1, 0, 1, 1);
            #else
                return _BaseColor;
            #endif
            }
            ENDHLSL
        }
    }
}
```

On editor startup (or reimport), `ShaderImporter` should precompile variants under
`<Project>/Intermediate/Shaders/` (DX12 DXIL cache).

## 2. Material + Inspector

1. Launch: `bin\Debug\ZEditor.exe -p I:\ZEngineDemo\ZEngineDemo.zproject`
2. Project window: create or open a `.zasset` material under `Assets/`.
3. Inspector: set **Shader** to `Custom/KeywordSmoke` (or the shader stem name).
4. Open **Shader Variants**; you should see checkboxes `USE_FOG` and `DEBUG_VIEW`
   (underscore-only `_` slots are not listed).
5. Enable `USE_FOG`, save (Inspector auto-saves on change).
6. **Active variant** line should show something like `USE_FOG=1;`.

## 3. Pick up changes in the viewport

Material keyword changes update `MaterialSourceDesc` hash. Re-submit render state:

- Re-select the scene object using the material, or
- Toggle the MeshRenderer / reload the scene, or
- Stop and press Play again.

Until re-submit, the cached `MaterialAssetId` / PSO may still reflect the old variant.

## 4. Verify DXIL cache key (DX12, optional)

With `USE_FOG` enabled, the next mesh draw should miss the default-variant cache and
compile/load DXIL with define `USE_FOG=1`.

Check `Intermediate/Shaders/` for a new or updated `*_*.dxil` whose variant segment
includes `USE_FOG=1` in the filename hash (same FNV layout as `ShaderLabDx12Compiler`).

Or enable `ZShader` / render logs and confirm a cache miss + compile after toggling
the keyword and re-submitting the entity.

## 5. Regression checks

| Step | Expected |
|------|----------|
| Toggle keyword off, save | `Active variant` empty; macros `{}` at compile |
| Change Shader in Inspector | Keywords cleared (`SetShaderByName`) |
| Reopen material `.zasset` | `enabled_shader_keywords` round-trips |
| Old materials without field | Load OK (SafeBinaryRead default empty vector) |

## 6. Build strip (`shader_feature`)

Editor DXIL precompile (`ShaderImporter::PrecompileProjectShaderVariants`) uses
Unity-style strip:

| Pragma | Precompile set |
|--------|----------------|
| `#pragma multi_compile` | Full Cartesian product (all combos) |
| `#pragma shader_feature` | Only variant keys used by project `MaterialRes` (`enabled_shader_keywords`) plus the multi_compile-only combos (shader_feature off) |

Example: `KeywordSmoke` with `multi_compile _ USE_FOG` and `shader_feature _ DEBUG_VIEW`,
and no material enabling `DEBUG_VIEW`, precompiles **2** DXIL pairs (not 4). Console:
`build strip skipped N unused shader_feature variant(s)`.

Toggle a keyword in the Inspector and save the material, then reimport the shader (or
restart the editor) to refresh the warm cache. Runtime draws still compile on cache miss.

## 7. Known limits

- Keywords are discovered from the `.shader` **source** (ShaderRegistry path or
  resolved `.shader` from shader picker), not from generated `.zasset` alone.
- Built-in `StandardLit` has no variant UI.
