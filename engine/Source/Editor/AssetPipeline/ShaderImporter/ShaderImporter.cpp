#include "ShaderImporter.h"

#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/ShaderLab/ShaderCbufferReflection.h"
#include "Runtime/Function/ShaderLab/ShaderLabDx12Compiler.h"
#include "Runtime/Function/ShaderLab/ShaderLabParser.h"
#include "Runtime/Function/ShaderLab/ShaderLabVariant.h"
#include "Runtime/Function/ShaderLab/ShaderTextureSlot.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/Shader.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace Runtime
{
    // -------------------------------------------------------------------------
    // PR-SE3b: .shader → ShaderRes .zasset conversion helpers
    // -------------------------------------------------------------------------

    namespace
    {
        std::string NormaliseShaderLookupKey(const eastl::string& shader_name)
        {
            std::string key(shader_name.c_str());
#if defined(_WIN32)
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
            return key;
        }

        ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader CollectMaterialVariantKeysByShader()
        {
            ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader result;

            ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
            AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
            if (project_info == nullptr || asset_mgr == nullptr)
            {
                return result;
            }

            const std::filesystem::path content_root = project_info->GetProjectContent();
            if (content_root.empty())
            {
                return result;
            }

            for (const std::filesystem::path& material_path : asset_mgr->GetAssetsByType("Material", content_root))
            {
                std::filesystem::path read_path = material_path;
                Material* material = asset_mgr->ReadObject<Material>(read_path);
                if (material == nullptr)
                {
                    continue;
                }

                const eastl::string shader_name =
                    material->m_Shader.empty() ? material->GetShaderName() : material->m_Shader;
                if (shader_name.empty() || shader_name == "StandardLit")
                {
                    continue;
                }

                std::vector<std::string> enabled_keywords;
                enabled_keywords.reserve(material->m_EnabledShaderKeywords.size());
                for (const eastl::string& keyword : material->m_EnabledShaderKeywords)
                {
                    if (!keyword.empty())
                    {
                        enabled_keywords.emplace_back(keyword.c_str());
                    }
                }

                const ZEngine::ShaderLab::ShaderVariantKey variant_key =
                    ZEngine::ShaderLab::ShaderVariantKeyFromEnabledKeywords(enabled_keywords);

                const std::string lookup_key = NormaliseShaderLookupKey(shader_name);
                std::vector<ZEngine::ShaderLab::ShaderVariantKey>& bucket = result[lookup_key];
                if (std::find(bucket.begin(), bucket.end(), variant_key) == bucket.end())
                {
                    bucket.push_back(variant_key);
                }
            }

            return result;
        }

        /// 一个 Pass 里 HLSLPROGRAM / GLSLPROGRAM 块的语言与入口点。
        struct ShaderProgramInfo
        {
            /// "HLSL" 或 "GLSL"。Cg 归入 HLSL（DX12 侧走的是同一条提取链路）。
            std::string source_language {"HLSL"};
            /// `#pragma vertex <name>`。ShaderLabParser::ExtractEntryPoints 已经
            /// 抓好了，没写 #pragma 时是 ShaderProgram 的默认值 "vert"/"frag"。
            std::string vertex_entry {"vert"};
            std::string fragment_entry {"frag"};
        };

        ShaderProgramInfo DescribeProgram(const ZEngine::ShaderLab::ShaderProgram& program)
        {
            ShaderProgramInfo info;
            info.source_language = program.language == ZEngine::ShaderLab::ShaderLanguage::GLSL ? "GLSL" : "HLSL";
            info.vertex_entry = program.vertex_entry.empty() ? std::string("main") : program.vertex_entry;
            info.fragment_entry = program.fragment_entry.empty() ? std::string("main") : program.fragment_entry;
            return info;
        }

        /// 属性名 -> 常量缓冲字节偏移。
        ///
        /// 这就是"脚本能对自己 shader 里写的任意属性 SetFloat"的那块元数据。
        /// 以前 ShaderRes 只有属性名 / 类型 / 默认值，**没有落点**，于是运行时
        /// 根本无从下手：RenderMaterialData 连字段都没有，拷过去的 m_Properties
        /// 谁也不消费。
        ///
        /// 反射的是 program 块源码（HLSLPROGRAM / GLSLPROGRAM 的正文），不是整份
        /// .shader —— CBUFFER_START(UnityPerMaterial) 就在正文里。
        ///
        /// 打包规则跟着源语言走：GLSL 是 std140，HLSL 是 cbuffer 规则。两条规则
        /// 对 `float _A; float2 _B;` 会算出不同的 offset，选错就是静默写错字节。
        void FillCbufferOffsets(const ZEngine::ShaderLab::ShaderLabAsset& lab_asset, ShaderRes& out)
        {
            // 找第一份带代码的 program（与语言 / 入口点取的是同一份）。
            const ZEngine::ShaderLab::ShaderProgram* program = nullptr;
            for (const auto& subshader : lab_asset.subshaders)
            {
                for (const auto& pass : subshader.passes)
                {
                    for (const auto& candidate : pass.programs)
                    {
                        if (!candidate.source_code.empty())
                        {
                            program = &candidate;
                            break;
                        }
                    }
                    if (program != nullptr)
                    {
                        break;
                    }
                }
                if (program != nullptr)
                {
                    break;
                }
            }

            if (program == nullptr || out.m_Properties.empty())
            {
                return;
            }

            const ZEngine::ShaderLab::ShaderCbufferPacking packing =
                program->language == ZEngine::ShaderLab::ShaderLanguage::GLSL
                    ? ZEngine::ShaderLab::ShaderCbufferPacking::Std140
                    : ZEngine::ShaderLab::ShaderCbufferPacking::HlslCbuffer;

            // Unity 的惯例：材质属性落在 UnityPerMaterial 里。
            const ZEngine::ShaderLab::ShaderCbufferLayout layout =
                ZEngine::ShaderLab::ReflectCbuffer(program->source_code, "UnityPerMaterial", packing);
            if (!layout.found)
            {
                LOG_WARNING(ZShader,
                            "ShaderImporter: no UnityPerMaterial cbuffer in '{}'; its properties have no UBO "
                            "offset and will not be writable by name",
                            out.m_ShaderName.c_str());
                return;
            }

            int matched = 0;
            for (ShaderPropertyDesc& property : out.m_Properties)
            {
                const ZEngine::ShaderLab::ShaderPropertyLayout* slot = layout.Find(property.m_Name.c_str());
                if (slot == nullptr)
                {
                    continue;
                }
                property.m_CbufferOffset = static_cast<int32_t>(slot->offset);
                property.m_CbufferSize = slot->size;
                property.m_CbufferComponents = slot->components;
                property.m_CbufferName = layout.buffer_name.c_str();
                ++matched;
            }

            if (matched == 0 && !out.m_Properties.empty())
            {
                LOG_WARNING(ZShader,
                            "ShaderImporter: shader '{}' declares {} Properties but none of them appears in "
                            "UnityPerMaterial ({} bytes, {} members)",
                            out.m_ShaderName.c_str(),
                            out.m_Properties.size(),
                            layout.size,
                            layout.properties.size());
            }
        }

        /// 2D 属性 -> 材质描述符集槽位（0..4）。
        ///
        /// 与 cbuffer 落点不同，这件事**不依赖 program 源码**：只数 Properties{}
        /// 里 2D 属性出现的顺序。所以放在这里单独跑，不受"第一份带代码的 program
        /// 找没找到"影响。
        ///
        /// 槽位复用内置那 5 个贴图 binding。超出 5 张的 2D 属性记 -1 并告警 ——
        /// 静默丢掉比这更糟：脚本 SetTexture 会拿到 false，但没人知道为什么。
        void FillTextureSlots(ShaderRes& out)
        {
            if (out.m_Properties.empty())
            {
                return;
            }

            std::vector<ZEngine::ShaderLab::ShaderTextureSlotInput> inputs;
            inputs.reserve(out.m_Properties.size());
            for (const ShaderPropertyDesc& property : out.m_Properties)
            {
                ZEngine::ShaderLab::ShaderTextureSlotInput input;
                input.name = property.m_Name.c_str();
                input.type = property.m_Type.c_str();
                inputs.push_back(std::move(input));
            }

            const ZEngine::ShaderLab::ShaderTextureSlotTable table =
                ZEngine::ShaderLab::AssignTextureSlots(inputs);
            for (size_t i = 0; i < out.m_Properties.size(); ++i)
            {
                out.m_Properties[i].m_TextureSlot = table.SlotAt(i);
            }

            if (table.overflow > 0)
            {
                LOG_WARNING(ZShader,
                            "ShaderImporter: shader '{}' declares {} 2D properties but a material descriptor set "
                            "only has {} texture slots; the last {} will never bind",
                            out.m_ShaderName.c_str(),
                            table.assigned + table.overflow,
                            ZEngine::ShaderLab::k_MaterialTextureSlotCount,
                            table.overflow);
            }
        }

        /// .shader 里 #include 的搜索根，存成**项目相对**路径（如 "Shaders"），
        /// 由 ResolveProjectAssetPath 在运行时还原成绝对路径。
        ///
        /// 以前写死 "Assets/Shaders"：Shaders/ 其实是 Assets/ 的**兄弟**目录
        /// （ProjectInfo::GetShadersRoot），拼出来是 <project>/Assets/Assets/Shaders，
        /// 于是所有项目相对路径的 #include 都 file not found。
        eastl::string ResolveShaderIncludeDirectory()
        {
            ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
            if (project_info == nullptr || project_info->project_path.empty())
            {
                return "Shaders";
            }

            const std::filesystem::path shaders_root = project_info->GetShadersRoot();
            if (shaders_root.empty())
            {
                return "Shaders";
            }

            std::error_code ec;
            const std::filesystem::path relative =
                std::filesystem::relative(shaders_root, std::filesystem::path(project_info->project_path.c_str()), ec);
            if (ec || relative.empty() || relative.generic_string().rfind("..", 0) == 0)
            {
                // 退化成绝对路径也比指到不存在的目录好。
                return shaders_root.generic_string().c_str();
            }
            return relative.generic_string().c_str();
        }

        // Convert a parsed ShaderLabAsset into a serialisable ShaderRes.
        // Mirrors the conversion logic that was previously inlined inside
        // inspector_window's LoadProjectShaderAssetByName.
        void ConvertShaderLabAssetToShaderRes(const ZEngine::ShaderLab::ShaderLabAsset& lab_asset,
                                              const std::filesystem::path& source_path,
                                              const eastl::string& include_directory,
                                              ShaderRes& out)
        {
            // Name: prefer the parsed shader_name; fall back to filename stem.
            out.m_ShaderName = lab_asset.shader_name.empty()
                                   ? source_path.stem().generic_string().c_str()
                                   : lab_asset.shader_name.c_str();

            // Properties
            out.m_Properties.clear();
            out.m_Properties.reserve(lab_asset.properties.size());
            for (const ZEngine::ShaderLab::ShaderProperty& prop : lab_asset.properties)
            {
                ShaderPropertyDesc desc;
                desc.m_Name = prop.name.c_str();
                desc.m_DisplayName = prop.display_name.c_str();
                desc.m_Type = ZEngine::ShaderLab::PropertyTypeToString(prop.type);
                desc.m_DefaultFloat = prop.default_value.float_value;
                desc.m_RangeMin = prop.default_value.range_min;
                desc.m_RangeMax = prop.default_value.range_max;
                desc.m_DefaultColor = Vector3(prop.default_value.color[0],
                                              prop.default_value.color[1],
                                              prop.default_value.color[2]);
                desc.m_DefaultAlpha = prop.default_value.color[3];
                desc.m_DefaultTexture = prop.default_value.texture_path.c_str();
                desc.m_DefaultToggle = prop.default_value.float_value > 0.5f || prop.default_value.int_value != 0;
                out.m_Properties.push_back(desc);
            }

            // Passes: extract from SubShader → Pass → Programs.
            out.m_Passes.clear();

            // 整份资产的语言 / 入口取**第一个带 program 的 pass**。同一份 .shader
            // 混写 HLSLPROGRAM 与 GLSLPROGRAM 是没有定义的行为，取第一个至少是
            // 确定的（而且会打警告）。
            ShaderProgramInfo primary_info;
            bool primary_info_set = false;

            for (const auto& subshader : lab_asset.subshaders)
            {
                // Unity 的 SubShader Tags 里 "RenderPipeline" 决定这个 SubShader
                // 属于哪条管线；没有就沿用 ShaderRes 的默认 "StandardLit"。
                const std::string subshader_pipeline = [&subshader]() -> std::string
                {
                    const auto pipeline_it = subshader.tags.find("RenderPipeline");
                    return pipeline_it != subshader.tags.end() ? pipeline_it->second : std::string();
                }();

                for (const auto& pass : subshader.passes)
                {
                    ShaderPassDesc pass_desc;
                    pass_desc.m_Name = pass.name.empty() ? "GBuffer" : pass.name.c_str();

                    // Light mode: try Tags["LightMode"], fall back to "GBuffer"
                    auto it = pass.tags.find("LightMode");
                    pass_desc.m_LightMode = (it != pass.tags.end()) ? it->second.c_str() : "GBuffer";

                    if (!subshader_pipeline.empty())
                    {
                        pass_desc.m_RenderPipeline = subshader_pipeline.c_str();
                    }
                    else if (const auto pipeline_it = pass.tags.find("RenderPipeline"); pipeline_it != pass.tags.end())
                    {
                        pass_desc.m_RenderPipeline = pipeline_it->second.c_str();
                    }

                    // Programs: the ShaderLab parser produces ShaderProgram
                    // entries with source_code / hlsl_code and entry points.
                    // ShaderRes stores FILE paths (not inline code). For
                    // ShaderLab assets we store the source .shader path so the
                    // runtime can locate it for compilation.
                    if (!pass.programs.empty())
                    {
                        pass_desc.m_VertexShaderFile = source_path.generic_string().c_str();
                        pass_desc.m_FragmentShaderFile = source_path.generic_string().c_str();

                        const ShaderProgramInfo info = DescribeProgram(pass.programs.front());
                        if (pass.programs.size() > 1)
                        {
                            LOG_WARNING(ZShader,
                                        "ShaderImporter: pass '{}' of {} declares {} program blocks; using the first ({})",
                                        pass_desc.m_Name.c_str(),
                                        source_path.generic_string(),
                                        pass.programs.size(),
                                        info.source_language);
                        }

                        // 入口名来自 #pragma vertex / #pragma fragment。以前写死
                        // "main"，凡是入口不叫 main 的 shader 编译期就报
                        // "missing entry point definition"。
                        pass_desc.m_VertexEntry = info.vertex_entry.c_str();
                        pass_desc.m_FragmentEntry = info.fragment_entry.c_str();

                        if (!primary_info_set)
                        {
                            primary_info = info;
                            primary_info_set = true;
                        }
                    }

                    // Render state from pass.render_states
                    const auto& rs = pass.render_states;
                    pass_desc.m_Zwrite = rs.zwrite;
                    pass_desc.m_Blend = rs.blend_enable ? "On" : "Off";
                    // Cull mode: enum → string
                    switch (rs.cull)
                    {
                        case ZEngine::ShaderLab::CullMode::Off:
                            pass_desc.m_Cull = "Off";
                            break;
                        case ZEngine::ShaderLab::CullMode::Front:
                            pass_desc.m_Cull = "Front";
                            break;
                        default:
                            pass_desc.m_Cull = "Back";
                            break;
                    }
                    // Z-test: enum → string
                    switch (rs.ztest)
                    {
                        case ZEngine::ShaderLab::CompareFunc::Less:
                            pass_desc.m_Ztest = "Less";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::Equal:
                            pass_desc.m_Ztest = "Equal";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::LEqual:
                            pass_desc.m_Ztest = "LEqual";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::Greater:
                            pass_desc.m_Ztest = "Greater";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::NotEqual:
                            pass_desc.m_Ztest = "NotEqual";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::GEqual:
                            pass_desc.m_Ztest = "GEqual";
                            break;
                        case ZEngine::ShaderLab::CompareFunc::Always:
                            pass_desc.m_Ztest = "Always";
                            break;
                        default:
                            pass_desc.m_Ztest = "LEqual";
                            break;
                    }

                    out.m_Passes.push_back(pass_desc);
                }
            }

            // Legacy flat fields (for ShaderRes objects that don't use the
            // multi-pass layout). When there is at least one pass, the
            // runtime resolves shader files per-pass; these flat fields are
            // only used by the legacy single-pass code path.
            if (!out.m_Passes.empty())
            {
                out.m_VertexShaderFile = out.m_Passes[0].m_VertexShaderFile;
                out.m_FragmentShaderFile = out.m_Passes[0].m_FragmentShaderFile;
                out.m_RenderPipeline = out.m_Passes[0].m_RenderPipeline;
            }

            // 源语言与入口点以前一律写死 "HLSL" / "main"。
            //
            // 写死 HLSL 的后果不是"少个优化"，是**整条 Vulkan 链路直接判死**：
            // MainCameraPassShaderCommon::CanUseRuntimeShaderPass 见到
            // source_language 含 "HLSL" 就 return false（Vulkan 没有 HLSL 前端，
            // 树内也没有 HLSL→SPIR-V 的交叉编译器）。所以 GLSLPROGRAM 写的
            // shader 会被当成 HLSL 挡掉，ShaderLab 在 Vulkan 上永远是死路。
            out.m_SourceLanguage = primary_info.source_language.c_str();
            out.m_VertexEntry = primary_info.vertex_entry.c_str();
            out.m_FragmentEntry = primary_info.fragment_entry.c_str();
            out.m_IncludeDirectory = include_directory;

            // 后端开关跟着源语言走，别再给一个注定失败的后端发通行证：
            //   HLSL -> DX12（DX12ShaderCompiler 走 ExtractHlslFromShaderLabFile）
            //   GLSL -> Vulkan / Metal（Metal 靠 spirv-cross 把 SPIR-V 转 MSL，
            //           而 spirv-cross 只在 Apple 链接，见 3rdparty/CMakeLists）
            // 以前三项全开，等于每个后端都先去撞一次编译失败再回退内置 shader。
            const bool is_glsl = primary_info.source_language == "GLSL";
            out.m_EnableDx12 = !is_glsl;
            out.m_EnableVulkan = is_glsl;
            out.m_EnableMetal = is_glsl;

            // 属性名 -> UBO 偏移。必须在语言 / 入口点定下来之后跑，因为打包规则
            // 要跟着源语言走。
            FillCbufferOffsets(lab_asset, out);

            // 2D 属性 -> 贴图槽。只数 Properties{} 的声明顺序，与 program 源码
            // 无关，所以无条件跑（FillCbufferOffsets 找不到 program 会提前返回）。
            FillTextureSlots(out);
        }

        bool doImportShaderLab(const std::filesystem::path& source_path,
                               const std::filesystem::path& output_path)
        {
            const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> lab_asset =
                ZEngine::ShaderLab::ParseShaderLabFromFile(source_path.generic_string());
            if (lab_asset == nullptr)
            {
                LOG_WARNING(ZShader, "ShaderImporter: failed to parse {}", source_path.generic_string());
                return false;
            }

            ShaderRes* shader_res = MemoryManager::CreateObject<ShaderRes>();
            if (shader_res == nullptr)
            {
                return false;
            }

            ConvertShaderLabAssetToShaderRes(*lab_asset, source_path, ResolveShaderIncludeDirectory(), *shader_res);

            AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
            const bool ok = asset_mgr && asset_mgr->WriteObjectToDiskThreadSafe(output_path, *shader_res);
            MemoryManager::DestroyObject(shader_res);
            if (ok)
            {
                ShaderImporter::PrecompileShaderVariants(source_path);
            }
            return ok;
        }
    }  // anonymous namespace

    // -------------------------------------------------------------------------
    // ShaderImporter public interface
    // -------------------------------------------------------------------------

    std::vector<std::string> ShaderImporter::GetSupportedExtensions() const
    {
        return {".shader", ".vert", ".frag"};
    }

    bool ShaderImporter::Import(const std::filesystem::path& source_path,
                                const std::filesystem::path& output_path,
                                const AssetImporterSettings& import_settings,
                                AssetMetadata& out_metadata)
    {
        const std::string ext = [&] {
            std::string e = source_path.extension().string();
            std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return e;
        }();

        if (ext == ".shader")
        {
            return doImportShaderLab(source_path, output_path);
        }

        // .vert / .frag — not yet implemented; the pre-existing stub
        // just returned true. Keep that behaviour for now.
        return true;
    }

    bool ShaderImporter::Reimport(const std::filesystem::path& zasset_path, const AssetImporterSettings& import_settings)
    {
        return true;
    }

    std::unique_ptr<AssetImporterSettings> ShaderImporter::GetDefaultSettings() const
    {
        return std::make_unique<ShaderImporterSettings>();
    }

    int ShaderImporter::ImportProjectShaders()
    {
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr || project_info->project_path.empty())
        {
            return 0;
        }

        const std::filesystem::path shaders_root = project_info->GetShadersRoot();
        const std::filesystem::path generated_root = project_info->GetGeneratedShadersRoot();
        if (shaders_root.empty() || generated_root.empty())
        {
            return 0;
        }

        std::error_code ec;
        if (!std::filesystem::exists(shaders_root, ec) || ec)
        {
            return 0;
        }

        // Ensure output directory exists
        std::filesystem::create_directories(generated_root, ec);
        if (ec)
        {
            LOG_WARNING(ZShader, "ShaderImporter: cannot create output dir {}: {}", generated_root.generic_string(), ec.message());
            return 0;
        }

        int imported = 0;

        // include 目录对所有 shader 都一样，循环外算一次。
        const eastl::string shader_include_directory = ResolveShaderIncludeDirectory();

        for (const auto& entry : std::filesystem::recursive_directory_iterator(shaders_root, ec))
        {
            if (ec)
            {
                break;
            }
            if (!entry.is_regular_file())
            {
                continue;
            }

            const std::string ext = [&] {
                std::string e = entry.path().extension().string();
                std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return e;
            }();
            if (ext != ".shader")
            {
                continue;
            }

            // Compute output path: mirror the relative path from Shaders/
            // into Assets/_Generated/Shaders/, changing extension to .shader.zasset
            const std::filesystem::path rel_path = std::filesystem::relative(entry.path(), shaders_root, ec);
            const std::filesystem::path output_path = generated_root / (rel_path.generic_string() + ".zasset");

            // A2: first-time seeding only. Skip if .zasset already exists.
            if (std::filesystem::exists(output_path, ec))
            {
                continue;
            }

            // Parse the .shader source
            const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> lab_asset =
                ZEngine::ShaderLab::ParseShaderLabFromFile(entry.path().generic_string());
            if (lab_asset == nullptr)
            {
                LOG_WARNING(ZShader, "ShaderImporter: failed to parse {}", entry.path().generic_string());
                continue;
            }

            // Convert to ShaderRes
            ShaderRes* shader_res = MemoryManager::CreateObject<ShaderRes>();
            if (shader_res == nullptr)
            {
                continue;
            }

            ConvertShaderLabAssetToShaderRes(*lab_asset, entry.path(), shader_include_directory, *shader_res);

            // Ensure parent directory exists for nested paths
            const std::filesystem::path output_dir = output_path.parent_path();
            if (!output_dir.empty())
            {
                std::filesystem::create_directories(output_dir, ec);
            }

            // Write .zasset
            AssetManager* asset_mgr = GET_SYSTEM(AssetManager);
            if (asset_mgr == nullptr || !asset_mgr->WriteObjectToDiskThreadSafe(output_path, *shader_res))
            {
                LOG_WARNING(ZShader, "ShaderImporter: failed to write {}", output_path.generic_string());
                MemoryManager::DestroyObject(shader_res);
                continue;
            }

            MemoryManager::DestroyObject(shader_res);
            ++imported;
        }

        if (imported > 0)
        {
            LOG_INFO(ZShader, "ShaderImporter: imported {} shader(s) to {}", imported, generated_root.generic_string());
        }

        return imported;
    }

    void ShaderImporter::PrecompileShaderVariants(const std::filesystem::path& source_shader_path)
    {
#if defined(_WIN32)
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return;
        }

        const std::filesystem::path cache_dir = project_info->GetIntermediateShadersRoot();
        if (cache_dir.empty())
        {
            return;
        }

        ZEngine::ShaderLab::ShaderLabDx12Compiler compiler;
        compiler.SetCacheDirectory(cache_dir);
        // 项目 shaders 根即 include 根（Unity 的 Assets/Shaders）。以前从不设置，
        // 于是所有写项目相对路径的 #include 都 file not found。
        if (const std::filesystem::path shaders_root = project_info->GetShadersRoot();
            !shaders_root.empty())
        {
            compiler.SetIncludeDirectory(shaders_root);
        }
        if (!compiler.LoadFromFile(source_shader_path.generic_string()))
        {
            LOG_WARNING(ZShader,
                        "ShaderImporter: variant precompile skipped for {} ({})",
                        source_shader_path.generic_string(),
                        compiler.GetParseError());
            return;
        }

        const ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader material_keys =
            CollectMaterialVariantKeysByShader();

        std::vector<ZEngine::ShaderLab::ShaderVariantKey> keys_for_shader;
        if (const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> asset = compiler.GetAsset())
        {
            eastl::string shader_name = asset->shader_name.empty()
                                            ? source_shader_path.stem().generic_string().c_str()
                                            : asset->shader_name.c_str();
            if (const auto keys_it = material_keys.find(NormaliseShaderLookupKey(shader_name));
                keys_it != material_keys.end())
            {
                keys_for_shader = keys_it->second;
            }
        }

        compiler.PrecompileAll({}, keys_for_shader.empty() ? nullptr : &keys_for_shader, true);
#else
        (void)source_shader_path;
#endif
    }

    int ShaderImporter::PrecompileProjectShaderVariants()
    {
#if defined(_WIN32)
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return 0;
        }

        const std::filesystem::path shaders_root = project_info->GetShadersRoot();
        const std::filesystem::path cache_dir = project_info->GetIntermediateShadersRoot();
        const ZEngine::ShaderLab::ShaderLabDx12Compiler::MaterialVariantKeysByShader material_keys =
            CollectMaterialVariantKeysByShader();
        return ZEngine::ShaderLab::ShaderLabDx12Compiler::PrecompileProjectShaders(shaders_root,
                                                                                   cache_dir,
                                                                                   64,
                                                                                   &material_keys);
#else
        return 0;
#endif
    }

}  // namespace Runtime
