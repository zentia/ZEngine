#include "Editor/EditorUI/ProjectWindow/ProjectAssetActions.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Editor/EditorSceneManager/EditorSceneManager.h"
#include "Editor/EditorUI/ProjectWindow/ProjectWindowHelpers.h"
#include "Editor/EditorWindow/PreviewWindow/MeshDataPreview.h"
#include "Editor/Menu/AssetsMenu.h"
#include "Editor/Platform/Interface/EditorUtility.h"
#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/Function/Render/WindowSystem.h"
#include "Runtime/Function/ShaderLab/ShaderLabParser.h"
#include "Runtime/Platform/Path/Path.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Prefab/PrefabUtility.h"
#include "Runtime/Resource/ResType/Common/Level.h"
#include "Runtime/Resource/ResType/Data/Material.h"
#include "Runtime/Resource/ResType/Data/Shader.h"
#include "core/Log/LogSystem.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#ifdef _WIN32
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
    #include <commdlg.h>
    #include <objbase.h>
    #include <shlobj.h>
    #include <shobjidl.h>
    #include <windows.h>
#endif

namespace
{
    using namespace ProjectWindowHelpers;

    std::string normalizeShaderPropertyKey(const char* property_name)
    {
        std::string normalized;
        if (property_name == nullptr)
        {
            return normalized;
        }

        for (const char c : std::string(property_name))
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        return normalized;
    }

    std::string normalizeShaderPropertyType(const eastl::string& property_type)
    {
        return normalizeShaderPropertyKey(property_type.c_str());
    }

    bool matchesShaderPropertyKey(const std::string& key, std::initializer_list<const char*> aliases)
    {
        for (const char* alias : aliases)
        {
            if (key == alias)
            {
                return true;
            }
        }
        return false;
    }

    ShaderPropertyDesc convertShaderLabPropertyToShaderDesc(const ZEngine::ShaderLab::ShaderProperty& property)
    {
        using namespace ZEngine::ShaderLab;

        ShaderPropertyDesc property_desc;
        property_desc.m_Name = property.name.c_str();
        property_desc.m_DisplayName = property.display_name.c_str();
        property_desc.m_DefaultFloat = property.default_value.float_value;
        property_desc.m_RangeMin = property.default_value.range_min;
        property_desc.m_RangeMax = property.default_value.range_max;
        property_desc.m_DefaultColor = Vector3(property.default_value.color[0], property.default_value.color[1], property.default_value.color[2]);
        property_desc.m_DefaultAlpha = property.default_value.color[3];
        property_desc.m_DefaultTexture = property.default_value.texture_path.c_str();
        property_desc.m_DefaultToggle = property.default_value.float_value > 0.5f || property.default_value.int_value != 0;

        switch (property.type)
        {
            case PropertyType::Float:
                property_desc.m_Type = "Float";
                break;
            case PropertyType::Int:
                property_desc.m_Type = "Int";
                property_desc.m_DefaultFloat = static_cast<float>(property.default_value.int_value);
                break;
            case PropertyType::Range:
                property_desc.m_Type = "Range";
                break;
            case PropertyType::Color:
                property_desc.m_Type = "Color";
                break;
            case PropertyType::Vector:
                property_desc.m_Type = "Vector";
                property_desc.m_DefaultColor =
                    Vector3(property.default_value.vector[0], property.default_value.vector[1], property.default_value.vector[2]);
                property_desc.m_DefaultAlpha = property.default_value.vector[3];
                break;
            case PropertyType::Texture2D:
                property_desc.m_Type = "Texture2D";
                if (property_desc.m_DefaultTexture.empty())
                {
                    property_desc.m_DefaultTexture = property.default_value.texture_default.c_str();
                }
                break;
            case PropertyType::Cube:
                property_desc.m_Type = "Cube";
                if (property_desc.m_DefaultTexture.empty())
                {
                    property_desc.m_DefaultTexture = property.default_value.texture_default.c_str();
                }
                break;
            case PropertyType::Texture3D:
                property_desc.m_Type = "Texture3D";
                if (property_desc.m_DefaultTexture.empty())
                {
                    property_desc.m_DefaultTexture = property.default_value.texture_default.c_str();
                }
                break;
            case PropertyType::FloatArray:
                property_desc.m_Type = "FloatArray";
                break;
            default:
                property_desc.m_Type = "Float";
                break;
        }

        return property_desc;
    }

    bool applyMaterialCompatibilityDefault(MaterialRes& material, const ShaderPropertyDesc& property_desc)
    {
        const std::string property_key = normalizeShaderPropertyKey(property_desc.m_Name.c_str());
        const std::string property_type = normalizeShaderPropertyType(property_desc.m_Type);

        if (matchesShaderPropertyKey(property_key, {"basecolor", "color", "albedo", "maincolor"}) && property_type == "color")
        {
            material.m_BaseColorFactor = property_desc.m_DefaultColor;
            material.m_AlphaFactor = property_desc.m_DefaultAlpha;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"metallic", "metalness"}))
        {
            material.m_MetallicFactor = property_desc.m_DefaultFloat;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"roughness", "smoothness"}))
        {
            material.m_RoughnessFactor = property_desc.m_DefaultFloat;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"normalscale"}))
        {
            material.m_NormalScale = property_desc.m_DefaultFloat;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"occlusion", "occlusionstrength"}))
        {
            material.m_OcclusionStrength = property_desc.m_DefaultFloat;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"emissioncolor", "emissivecolor"}) && property_type == "color")
        {
            material.m_EmissiveFactor = property_desc.m_DefaultColor;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"transparent", "isblend", "surfaceblend"}))
        {
            material.m_IsBlend = property_desc.m_DefaultToggle || property_desc.m_DefaultFloat > 0.5f;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"doublesided", "isdoublesided"}))
        {
            material.m_IsDoubleSided = property_desc.m_DefaultToggle || property_desc.m_DefaultFloat > 0.5f;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"basemap", "maintex", "albedomap"}))
        {
            material.m_BaseColourTextureFile = property_desc.m_DefaultTexture;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"metallicroughnessmap", "metallicmap"}))
        {
            material.m_MetallicRoughnessTextureFile = property_desc.m_DefaultTexture;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"normalmap"}))
        {
            material.m_NormalTextureFile = property_desc.m_DefaultTexture;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"occlusionmap", "aomap"}))
        {
            material.m_OcclusionTextureFile = property_desc.m_DefaultTexture;
            return true;
        }
        if (matchesShaderPropertyKey(property_key, {"emissionmap", "emissivemap"}))
        {
            material.m_EmissiveTextureFile = property_desc.m_DefaultTexture;
            return true;
        }

        return false;
    }

    void appendMaterialPropertyDefault(MaterialRes& material, const ShaderPropertyDesc& property_desc)
    {
        if (property_desc.m_Name.empty())
        {
            return;
        }

        if (applyMaterialCompatibilityDefault(material, property_desc))
        {
            return;
        }

        const std::string property_type = normalizeShaderPropertyType(property_desc.m_Type);
        if (property_type == "color")
        {
            MaterialColorProperty property;
            property.m_Name = property_desc.m_Name;
            property.m_Color = property_desc.m_DefaultColor;
            property.m_Alpha = property_desc.m_DefaultAlpha;
            material.m_ColorProperties.push_back(property);
            return;
        }

        if (property_type == "texture2d" || property_type == "texture" || property_type == "cube" || property_type == "texture3d")
        {
            MaterialTextureProperty property;
            property.m_Name = property_desc.m_Name;
            property.m_TextureFile = property_desc.m_DefaultTexture;
            material.m_TextureProperties.push_back(property);
            return;
        }

        if (property_type == "toggle" || property_type == "bool")
        {
            MaterialToggleProperty property;
            property.m_Name = property_desc.m_Name;
            property.m_Value = property_desc.m_DefaultToggle;
            material.m_ToggleProperties.push_back(property);
            return;
        }

        MaterialFloatProperty property;
        property.m_Name = property_desc.m_Name;
        property.m_Value = property_desc.m_DefaultFloat;
        material.m_FloatProperties.push_back(property);
    }

    void initializeMaterialDefaultsFromShader(MaterialRes& material, const std::vector<ShaderPropertyDesc>& properties)
    {
        material.m_FloatProperties.clear();
        material.m_ColorProperties.clear();
        material.m_TextureProperties.clear();
        material.m_ToggleProperties.clear();

        for (const ShaderPropertyDesc& property_desc : properties)
        {
            appendMaterialPropertyDefault(material, property_desc);
        }
    }

    bool loadShaderDefinitionForMaterialCreation(const std::filesystem::path& shader_path,
                                                 eastl::string& shader_name,
                                                 std::vector<ShaderPropertyDesc>& properties)
    {
        if (shader_path.empty() || !std::filesystem::exists(shader_path))
        {
            return false;
        }

        std::string extension = shader_path.extension().generic_string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".shader")
        {
            return false;
        }

        const std::shared_ptr<ZEngine::ShaderLab::ShaderLabAsset> shader_lab_asset =
            ZEngine::ShaderLab::ParseShaderLabFromFile(shader_path.generic_string());
        if (shader_lab_asset == nullptr)
        {
            return false;
        }

        if (shader_lab_asset->shader_name.empty())
        {
            const std::string stem_name = shader_path.stem().generic_string();
            shader_name = stem_name.c_str();
        }
        else
        {
            shader_name = shader_lab_asset->shader_name.c_str();
        }
        properties.clear();
        properties.reserve(shader_lab_asset->properties.size());
        for (const ZEngine::ShaderLab::ShaderProperty& property : shader_lab_asset->properties)
        {
            properties.push_back(convertShaderLabPropertyToShaderDesc(property));
        }
        return true;
    }

    std::filesystem::path buildUniqueMaterialAssetPath(const std::filesystem::path& folder_path)
    {
        const std::string material_base_name = "New Material";

        std::filesystem::path candidate_path = folder_path / (material_base_name + ".zasset");
        int counter = 1;
        while (std::filesystem::exists(candidate_path))
        {
            candidate_path = folder_path / (material_base_name + " " + std::to_string(counter) + ".zasset");
            ++counter;
        }
        return candidate_path;
    }

    std::filesystem::path buildMaterialPathFromShader(const std::filesystem::path& shader_path)
    {
        (void)shader_path;
        std::filesystem::path assets_folder = GetEditorSourceAssetFolder();
        std::error_code ec;
        std::filesystem::create_directories(assets_folder, ec);
        return buildUniqueMaterialAssetPath(assets_folder);
    }

    std::string buildShaderLabName(const std::filesystem::path& shader_file_path)
    {
        std::filesystem::path relative_path = Path::GetRelativePath(GetEditorSourceAssetFolder(), shader_file_path);
        relative_path.replace_extension();

        const std::string relative_name = relative_path.generic_string();
        if (relative_name.empty() || (relative_name.size() >= 2 && relative_name[0] == '.' && relative_name[1] == '.'))
        {
            return "Custom/NewShader";
        }

        return "Custom/" + relative_name;
    }

    std::string buildDefaultShaderLabSource(const std::filesystem::path& shader_file_path)
    {
        const std::string shader_name = buildShaderLabName(shader_file_path);
        return "Shader \"" + shader_name + "\"\n"
                                           "{\n"
                                           "    Properties\n"
                                           "    {\n"
                                           "        _BaseColor (\"Base Color\", Color) = (1, 1, 1, 1)\n"
                                           "    }\n\n"
                                           "    SubShader\n"
                                           "    {\n"
                                           "        Tags\n"
                                           "        {\n"
                                           "            \"RenderPipeline\" = \"Deferred\"\n"
                                           "            \"Queue\" = \"Geometry\"\n"
                                           "            \"RenderType\" = \"Opaque\"\n"
                                           "        }\n\n"
                                           "        Pass\n"
                                           "        {\n"
                                           "            Name \"ForwardLit\"\n"
                                           "            Tags { \"LightMode\" = \"ForwardLit\" }\n\n"
                                           "            ZWrite On\n"
                                           "            ZTest LEqual\n"
                                           "            Cull Back\n\n"
                                           "            HLSLPROGRAM\n"
                                           "            #pragma vertex vert\n"
                                           "            #pragma fragment frag\n\n"
                                           "            cbuffer UnityPerMaterial\n"
                                           "            {\n"
                                           "                float4 _BaseColor;\n"
                                           "            };\n\n"
                                           "            struct Attributes\n"
                                           "            {\n"
                                           "                float4 position : POSITION;\n"
                                           "            };\n\n"
                                           "            struct Varyings\n"
                                           "            {\n"
                                           "                float4 position : SV_POSITION;\n"
                                           "            };\n\n"
                                           "            Varyings vert(Attributes input)\n"
                                           "            {\n"
                                           "                Varyings output;\n"
                                           "                output.position = input.position;\n"
                                           "                return output;\n"
                                           "            }\n\n"
                                           "            float4 frag(Varyings input) : SV_Target\n"
                                           "            {\n"
                                           "                return _BaseColor;\n"
                                           "            }\n"
                                           "            ENDHLSL\n"
                                           "        }\n"
                                           "    }\n"
                                           "}\n";
    }

    bool ensureMaterialResTypeReady()
    {
        Type* material_type = const_cast<Type*>(TypeOf<MaterialRes>());
        if (material_type == nullptr)
        {
            return false;
        }

        if (material_type->GetFactory() == nullptr || std::strcmp(material_type->GetName(), "[UNREGISTERED]") == 0)
        {
            RegisterKlass<MaterialRes>();
        }

        if (material_type->GetFactory() == nullptr)
        {
            return false;
        }

        RuntimeTypeArray& runtime_types = TypeManager::GetInstance().GetRuntimeTypes();
        for (uint32_t type_index = 0; type_index < runtime_types.count; ++type_index)
        {
            if (runtime_types.types[type_index] == material_type)
            {
                material_type->derivedFromInfo.typeIndex = type_index;
                material_type->derivedFromInfo.descendantCount = 1;
                return true;
            }
        }

        if (runtime_types.count >= RuntimeTypeArray::MAX_RUNTIME_TYPES)
        {
            return false;
        }

        material_type->derivedFromInfo.typeIndex = runtime_types.count;
        material_type->derivedFromInfo.descendantCount = 1;
        runtime_types.types[runtime_types.count] = material_type;
        ++runtime_types.count;

        Type* object_type = const_cast<Type*>(TypeOf<Object>());
        if (object_type != nullptr && object_type->derivedFromInfo.typeIndex == 0 && object_type->derivedFromInfo.descendantCount < runtime_types.count)
        {
            object_type->derivedFromInfo.descendantCount = runtime_types.count;
        }

        return true;
    }

    void createMaterialFromShader(ProjectWindowContext& ctx, const std::filesystem::path& shader_path)
    {
        if (shader_path.empty())
        {
            return;
        }

        eastl::string shader_name;
        std::vector<ShaderPropertyDesc> shader_properties;
        if (!loadShaderDefinitionForMaterialCreation(shader_path, shader_name, shader_properties))
        {
            LOG_ERROR(ZEditor, "failed to load shader definition for material creation {}", shader_path.generic_string());
            return;
        }

        if (!ensureMaterialResTypeReady())
        {
            LOG_ERROR(ZEditor, "material type is not registered for shader material creation {}", shader_path.generic_string());
            return;
        }

        MaterialRes* material = MemoryManager::CreateObject<MaterialRes>();
        if (material == nullptr)
        {
            LOG_ERROR(ZEditor, "failed to allocate material asset for shader {}", shader_path.generic_string());
            return;
        }

        material->m_Shader = shader_name;
        material->m_ShaderGuid.clear();
        material->m_ShaderPptr = PPtr<ShaderRes>();
        initializeMaterialDefaultsFromShader(*material, shader_properties);

        const std::filesystem::path material_path = buildMaterialPathFromShader(shader_path);
        const bool material_created =
            GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(material_path, *material) &&
            std::filesystem::exists(material_path);

        MemoryManager::DestroyObject(material);

        if (!material_created)
        {
            LOG_ERROR(ZEditor, "failed to create material from shader {}", shader_path.generic_string());
            return;
        }

        MaterialRes* saved_material =
            GET_SYSTEM(AssetManager)->ReadObject<MaterialRes>(const_cast<std::filesystem::path&>(material_path));
        if (saved_material != nullptr)
        {
            saved_material->SetShaderByName(shader_name);
            GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(material_path, *saved_material);
        }

        ProjectAssetActions::RefreshProjectSelection(ctx, material_path);
    }
}  // namespace

namespace ProjectAssetActions
{
    void RequestImportDialog(ProjectWindowContext& ctx)
    {
        ctx.pending_import_dialog = true;
    }

    void ExecutePendingImportDialog(ProjectWindowContext& ctx)
    {
        if (!ctx.pending_import_dialog)
        {
            return;
        }
        ctx.pending_import_dialog = false;

        if (ctx.selected_node == nullptr)
        {
            return;
        }

#ifdef _WIN32
        GLFWwindow* glfw_window = GET_SYSTEM(WindowSystem)->GetWindow();
        if (glfw_window == nullptr)
        {
            return;
        }

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool com_initialized = SUCCEEDED(hr);
        if (hr == RPC_E_CHANGED_MODE)
        {
            com_initialized = false;
        }

        IFileOpenDialog* pFileOpen = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pFileOpen));

        if (SUCCEEDED(hr))
        {
            DWORD dwFlags;
            hr = pFileOpen->GetOptions(&dwFlags);
            if (SUCCEEDED(hr))
            {
                hr = pFileOpen->SetOptions(dwFlags | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);
            }

            COMDLG_FILTERSPEC rgSpec[] = {{L"All Files", L"*.*"},
                                          {L"Image Files", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.dds"},
                                          {L"Model Files", L"*.fbx;*.obj;*.gltf;*.glb"},
                                          {L"Audio Files", L"*.wav;*.mp3;*.ogg"}};
            pFileOpen->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
            pFileOpen->SetFileTypeIndex(1);

            std::string initial_dir;
            if (!ctx.selected_node->m_FilePath.empty())
            {
                std::filesystem::path selected_path = GET_SYSTEM(AssetManager)->GetFullPath(ctx.selected_node->m_FilePath);
                if (std::filesystem::exists(selected_path))
                {
                    if (std::filesystem::is_directory(selected_path))
                    {
                        initial_dir = selected_path.string();
                    }
                    else
                    {
                        initial_dir = selected_path.parent_path().string();
                    }
                }
            }

            if (!initial_dir.empty())
            {
                std::wstring w_initial_dir(initial_dir.begin(), initial_dir.end());
                IShellItem* pShellItem = nullptr;
                hr = SHCreateItemFromParsingName(w_initial_dir.c_str(), nullptr, IID_PPV_ARGS(&pShellItem));
                if (SUCCEEDED(hr))
                {
                    pFileOpen->SetFolder(pShellItem);
                    pShellItem->Release();
                }
            }

            HWND hwnd = glfwGetWin32Window(glfw_window);
            hr = pFileOpen->Show(hwnd);

            if (SUCCEEDED(hr))
            {
                IShellItemArray* pItems = nullptr;
                hr = pFileOpen->GetResults(&pItems);
                if (SUCCEEDED(hr))
                {
                    DWORD count = 0;
                    pItems->GetCount(&count);
                    for (DWORD i = 0; i < count; i++)
                    {
                        IShellItem* pItem = nullptr;
                        hr = pItems->GetItemAt(i, &pItem);
                        if (SUCCEEDED(hr))
                        {
                            PWSTR pszFilePath = nullptr;
                            hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                            if (SUCCEEDED(hr))
                            {
                                int utf8_size =
                                    WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, nullptr, 0, nullptr, nullptr);
                                eastl::string file_path;
                                if (utf8_size > 0)
                                {
                                    file_path.resize(utf8_size - 1);
                                    WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, file_path.data(), utf8_size, nullptr, nullptr);
                                }

                                eastl::string target_dir;
                                if (!initial_dir.empty())
                                {
                                    target_dir = initial_dir.c_str();
                                }

                                AssetsMenu::ConvertAsset(file_path, target_dir);

                                CoTaskMemFree(pszFilePath);
                            }
                            pItem->Release();
                        }
                    }
                    pItems->Release();

                    ctx.file_service.BuildEngineFileTree();
                }
            }

            pFileOpen->Release();
        }

        if (com_initialized)
        {
            CoUninitialize();
        }
#endif
    }

    void OnMenuItemDelete(ProjectWindowContext& ctx, EditorFileNode* node)
    {
        if (node == nullptr || node->m_FilePath.empty())
        {
            return;
        }

        const std::filesystem::path asset_root_path = NormalizeProjectPath(GetEditorSourceAssetFolder());
        const std::filesystem::path target_path = NormalizeProjectPath(node->m_FilePath.c_str());
        if (target_path == asset_root_path)
        {
            return;
        }

        ctx.pending_delete_path = target_path;
        ctx.has_pending_delete = true;
    }

    void ExecutePendingDelete(ProjectWindowContext& ctx)
    {
        if (!ctx.has_pending_delete)
        {
            return;
        }

        const std::filesystem::path target_path = ctx.pending_delete_path;
        ctx.pending_delete_path.clear();
        ctx.has_pending_delete = false;

        const std::filesystem::path fallback_path = target_path.parent_path();

        std::error_code error_code;
        bool removed = false;
        if (std::filesystem::is_directory(target_path, error_code))
        {
            UnloadAssetStreamsUnderPath(target_path);
            error_code.clear();
            removed = std::filesystem::remove_all(target_path, error_code) > 0;
        }
        else if (target_path.extension() == ".zasset")
        {
            auto editor_assets = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager));
            if (editor_assets != nullptr)
            {
                removed = editor_assets->DeleteAsset(target_path.generic_string());
            }
            else
            {
                UnloadAssetStreamsUnderPath(target_path);
                error_code.clear();
                removed = std::filesystem::remove(target_path, error_code);
            }
        }
        else
        {
            UnloadAssetStreamsUnderPath(target_path);
            error_code.clear();
            removed = std::filesystem::remove(target_path, error_code);
        }

        if (!removed || error_code)
        {
            const char* detail = error_code ? error_code.message().c_str() : "operation failed";
            if (!error_code && target_path.extension() == ".zasset")
            {
                detail = "delete blocked or failed (check ZAsset log for referencers)";
            }
            LOG_ERROR(ZEditor, "failed to delete project asset {}: {}", target_path.generic_string(), detail);
            return;
        }

        ctx.file_service.BuildEngineFileTree();

        EditorFileNode* fallback_node = nullptr;
        if (EditorFileNode* editor_root_node = ctx.file_service.getEditorRootNode())
        {
            fallback_node = FindProjectNodeByPath(editor_root_node, fallback_path);
            if (fallback_node == nullptr)
            {
                fallback_node = editor_root_node;
            }
        }

        ctx.selected_node = fallback_node;
        GET_SYSTEM(EditorSceneManager)->OnAssetSelected(std::filesystem::path(), "");
    }

    void RequestPrefabCreate(ProjectWindowContext& ctx, GObjectID source_id, std::filesystem::path target_folder)
    {
        if (source_id == k_invalid_gobject_id)
        {
            return;
        }
        ctx.pending_prefab_source_gid = source_id;
        ctx.pending_prefab_target_folder = std::move(target_folder);
        ctx.has_pending_prefab_create = true;
    }

    void ExecutePendingPrefabCreate(ProjectWindowContext& ctx)
    {
        if (!ctx.has_pending_prefab_create)
        {
            return;
        }

        const GObjectID source_id = ctx.pending_prefab_source_gid;
        const std::filesystem::path target_folder = ctx.pending_prefab_target_folder;
        ctx.pending_prefab_source_gid = k_invalid_gobject_id;
        ctx.pending_prefab_target_folder.clear();
        ctx.has_pending_prefab_create = false;

        GameObject* dropped_go = ResolveLiveGameObject(source_id);
        if (dropped_go == nullptr)
        {
            return;
        }

        const std::filesystem::path prefab_path =
            BuildUniquePrefabAssetPath(target_folder, dropped_go->GetName());
        if (PrefabUtility::SaveAsPrefabAsset(dropped_go, prefab_path))
        {
            RefreshProjectSelection(ctx, prefab_path);
        }
    }

    void RefreshProjectSelection(ProjectWindowContext& ctx, const std::filesystem::path& asset_path)
    {
        ctx.file_service.BuildEngineFileTree();
        if (EditorFileNode* new_asset_node = FindProjectNodeByPath(ctx.file_service.getEditorRootNode(), asset_path))
        {
            ctx.selected_node = new_asset_node;
            GET_SYSTEM(EditorSceneManager)
                ->OnAssetSelected(new_asset_node->m_FilePath.c_str(), new_asset_node->displayTypeLabel());
        }
        else
        {
            ctx.selected_node = ctx.file_service.getEditorRootNode();
            GET_SYSTEM(EditorSceneManager)->OnAssetSelected(std::filesystem::path {}, std::string {});
        }
    }

    void RequestMaterialFromShader(ProjectWindowContext& ctx, EditorFileNode* node)
    {
        if (!IsShaderAssetNode(node) || node->m_FilePath.empty())
        {
            return;
        }

        ctx.pending_material_shader_path = node->m_FilePath.c_str();
        ctx.has_pending_material_from_shader = true;
    }

    void ExecutePendingMaterialFromShader(ProjectWindowContext& ctx)
    {
        if (!ctx.has_pending_material_from_shader)
        {
            return;
        }

        const std::filesystem::path shader_path = ctx.pending_material_shader_path;
        ctx.pending_material_shader_path.clear();
        ctx.has_pending_material_from_shader = false;

        createMaterialFromShader(ctx, shader_path);
    }

    bool CanRenameNode(const ProjectWindowContext& ctx, const EditorFileNode* node)
    {
        (void)ctx;
        if (node == nullptr || node->m_FilePath.empty() || node->m_NodeDepth < 0)
        {
            return false;
        }

        return !IsProtectedRenamePath(node->m_FilePath.c_str());
    }

    bool IsNodeRenaming(const ProjectWindowContext& ctx, const EditorFileNode* node)
    {
        if (!ctx.is_renaming || node == nullptr || node->m_FilePath.empty() || ctx.renaming_target_path.empty())
        {
            return false;
        }

        return NormalizeProjectPath(node->m_FilePath.c_str()) == NormalizeProjectPath(ctx.renaming_target_path);
    }

    void OnMenuItemRename(ProjectWindowContext& ctx, EditorFileNode* node)
    {
        if (!CanRenameNode(ctx, node))
        {
            return;
        }

        ctx.renaming_target_path = NormalizeProjectPath(node->m_FilePath.c_str());
        ctx.is_renaming = true;
        ctx.rename_focus_pending = true;

        std::string initial_name;
        if (IsFolderNode(node))
        {
            initial_name = node->m_FileName.c_str();
        }
        else
        {
            initial_name = std::filesystem::path(node->m_FileName.c_str()).stem().string();
        }

        std::memset(ctx.rename_buffer.data(), 0, ctx.rename_buffer.size());
        const size_t copy_len = std::min(initial_name.size(), ctx.rename_buffer.size() - 1);
        if (copy_len > 0)
        {
            std::memcpy(ctx.rename_buffer.data(), initial_name.data(), copy_len);
        }

        ctx.selected_node = node;
    }

    void CancelRename(ProjectWindowContext& ctx)
    {
        ctx.is_renaming = false;
        ctx.rename_focus_pending = false;
        ctx.renaming_target_path.clear();
        std::memset(ctx.rename_buffer.data(), 0, ctx.rename_buffer.size());
    }

    void CommitRename(ProjectWindowContext& ctx)
    {
        if (!ctx.is_renaming || ctx.renaming_target_path.empty())
        {
            CancelRename(ctx);
            return;
        }

        TrimRenameBufferInPlace(ctx.rename_buffer.data(), ctx.rename_buffer.size());
        const std::string new_token = ctx.rename_buffer.data();
        if (!IsValidRenameName(new_token))
        {
            LOG_WARNING(ZEditor, "Rename rejected: invalid name '{}'", new_token);
            CancelRename(ctx);
            return;
        }

        const std::filesystem::path old_path = ctx.renaming_target_path;
        if (IsProtectedRenamePath(old_path))
        {
            CancelRename(ctx);
            return;
        }

        std::error_code ec;
        const bool was_directory = std::filesystem::is_directory(old_path, ec);
        ec.clear();

        std::filesystem::path new_file_name;
        if (was_directory)
        {
            new_file_name = new_token;
        }
        else
        {
            const std::string extension = old_path.extension().generic_string();
            new_file_name = new_token + extension;
        }

        const std::filesystem::path new_path = old_path.parent_path() / new_file_name;
        if (NormalizeProjectPath(new_path) == NormalizeProjectPath(old_path))
        {
            CancelRename(ctx);
            return;
        }

        if (std::filesystem::exists(new_path, ec))
        {
            LOG_ERROR(ZEditor, "Rename failed: '{}' already exists", new_path.generic_string());
            CancelRename(ctx);
            return;
        }

        bool renamed = false;
        if (!was_directory && old_path.extension() == ".zasset")
        {
            auto editor_assets = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager));
            if (editor_assets != nullptr)
            {
                renamed = editor_assets->MoveAsset(old_path.generic_string(), new_path.generic_string());
                if (!renamed)
                {
                    CancelRename(ctx);
                    return;
                }
            }
        }

        if (!renamed)
        {
            ec.clear();
            std::filesystem::rename(old_path, new_path, ec);
            if (ec)
            {
                LOG_ERROR(ZEditor,
                          "Rename failed: {} -> {} ({})",
                          old_path.generic_string(),
                          new_path.generic_string(),
                          ec.message());
                CancelRename(ctx);
                return;
            }
        }

        LOG_INFO(ZEditor, "Renamed {} -> {}", old_path.generic_string(), new_path.generic_string());

        CancelRename(ctx);
        ctx.asset_tree_dirty = true;

        if (EditorFileNode* renamed_node = FindProjectNodeAcrossRoots(ctx.file_service, new_path))
        {
            ctx.selected_node = renamed_node;
            if (!IsFolderNode(renamed_node))
            {
                GET_SYSTEM(EditorSceneManager)
                    ->OnAssetSelected(renamed_node->m_FilePath.c_str(), renamed_node->displayTypeLabel());
            }
            else
            {
                GET_SYSTEM(EditorSceneManager)->OnAssetSelected(std::filesystem::path(), "");
            }
        }
        else
        {
            ctx.selected_node = ctx.file_service.getEditorRootNode();
            GET_SYSTEM(EditorSceneManager)->OnAssetSelected(std::filesystem::path(), "");
        }
    }

    void OnMenuItemShowInExplorer(EditorFileNode* node)
    {
        EditorUtility::RevealInFinder(node->m_FilePath);
    }

    void OnMenuItemCopyPath(EditorFileNode* node)
    {
        GLFWwindow* glfw_window = GET_SYSTEM(WindowSystem)->GetWindow();
        if (glfw_window == nullptr)
        {
            return;
        }
        const char* text = node->m_FilePath.empty() ? node->m_FileName.c_str() : node->m_FilePath.c_str();
        glfwSetClipboardString(glfw_window, text);
    }

    void OnMenuItemReimport(ProjectWindowContext& ctx, EditorFileNode* node)
    {
        if (node == nullptr || node->m_FilePath.empty())
        {
            return;
        }

        auto editor_asset_mgr = std::dynamic_pointer_cast<EditorAssetManager>(GET_SYSTEM(AssetManager));
        if (editor_asset_mgr == nullptr)
        {
            LOG_WARNING(ZEditor, "Reimport: EditorAssetManager unavailable");
            return;
        }

        const std::filesystem::path zasset_path = NormalizeProjectPath(node->m_FilePath.c_str());
        if (editor_asset_mgr->reimportAsset(zasset_path.generic_string(), nullptr))
        {
            MeshDataPreview::InvalidatePreview(zasset_path);
            ctx.file_service.BuildEngineFileTree();
            RefreshProjectSelection(ctx, zasset_path);
            LOG_INFO(ZEditor, "Reimport ok: {}", zasset_path.generic_string());
        }
        else
        {
            LOG_WARNING(ZEditor,
                        "Reimport failed for '{}' (see ZAsset log; need a registered or discoverable source file)",
                        zasset_path.generic_string());
        }
    }

    void CreateNewAsset(ProjectWindowContext& ctx, const std::string& asset_type)
    {
        if (ctx.selected_node == nullptr)
        {
            return;
        }

        std::filesystem::path folder_path = GetEditorSourceAssetFolder();

        if (!ctx.selected_node->m_FilePath.empty())
        {
            folder_path = ctx.selected_node->m_FilePath.c_str();
            if (!std::filesystem::is_directory(folder_path))
            {
                folder_path = folder_path.parent_path();
            }
        }

        if (!std::filesystem::exists(folder_path) || !std::filesystem::is_directory(folder_path))
        {
            folder_path = GetEditorSourceAssetFolder();
        }

        if (asset_type == "Shader")
        {
            const std::shared_ptr<ProjectInfo> project_info = GET_SYSTEM(ProjectInfo);
            if (project_info != nullptr)
            {
                std::error_code ec;
                std::filesystem::path shaders_root = project_info->GetShadersRoot();
                if (!shaders_root.empty())
                {
                    shaders_root = std::filesystem::absolute(shaders_root, ec).lexically_normal();
                    std::filesystem::path normalized_folder =
                        std::filesystem::absolute(folder_path, ec).lexically_normal();

                    const std::filesystem::path rel = normalized_folder.lexically_relative(shaders_root);
                    const std::string rel_str = rel.generic_string();
                    const bool is_inside_shaders =
                        !rel_str.empty() && rel_str != "." && rel_str.rfind("..", 0) != 0;

                    if (!is_inside_shaders)
                    {
                        std::filesystem::create_directories(shaders_root, ec);
                        folder_path = shaders_root;
                    }
                }
            }
        }

        std::string base_name = "New" + asset_type;
        std::string extension = ".json";

        if (asset_type == "Folder")
        {
            extension = "";
        }
        else if (asset_type == "Material")
        {
            extension = ".zasset";
            base_name = "New Material";
        }
        else if (asset_type == "Shader")
        {
            extension = ".shader";
        }
        else if (asset_type == "Blueprint")
        {
            extension = ".blueprint.json";
        }
        else if (asset_type == "Level")
        {
            extension = ".scene";
        }
        else if (asset_type == "Object")
        {
            extension = ".object.json";
        }

        std::filesystem::path new_file_path;
        if (asset_type == "Material")
        {
            new_file_path = buildUniqueMaterialAssetPath(folder_path);
        }
        else
        {
            new_file_path = folder_path / (base_name + extension);
            int counter = 1;
            while (std::filesystem::exists(new_file_path))
            {
                new_file_path = folder_path / (base_name + "_" + std::to_string(counter) + extension);
                counter++;
            }
        }

        if (asset_type == "Folder")
        {
            std::filesystem::create_directory(new_file_path);
        }
        else if (asset_type == "Material")
        {
            if (!ensureMaterialResTypeReady())
            {
                LOG_ERROR(ZEditor, "material type is not registered for asset creation {}", new_file_path.generic_string());
                return;
            }

            MaterialRes* material = MemoryManager::CreateObject<MaterialRes>();
            const bool material_created = material != nullptr &&
                                          GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(new_file_path, *material) &&
                                          std::filesystem::exists(new_file_path);
            MemoryManager::DestroyObject(material);
            if (!material_created)
            {
                LOG_ERROR(ZEditor, "failed to create material asset {}", new_file_path.generic_string());
                return;
            }
        }
        else if (asset_type == "Shader")
        {
            const bool shader_created = WriteTextFileIfMissing(new_file_path, buildDefaultShaderLabSource(new_file_path)) &&
                                          std::filesystem::exists(new_file_path);
            if (!shader_created)
            {
                LOG_ERROR(ZEditor, "failed to create shader source {}", new_file_path.generic_string());
                return;
            }
        }
        else if (asset_type == "Level")
        {
            // A new scene is an empty YAML object graph carrying a single LevelRes
            // header (fileID 1). GameObjects get appended on first save.
            LevelRes* header = static_cast<LevelRes*>(GET_SYSTEM(ObjectManager)->Produce(TypeOf<LevelRes>(), 0));
            bool level_created = false;
            if (header != nullptr)
            {
                Object* graph_objects[1] = {header};
                const int64_t graph_ids[1] = {1};
                level_created = GET_SYSTEM(AssetManager)->WriteObjectsToYaml(new_file_path, graph_objects, graph_ids, 1) &&
                                std::filesystem::exists(new_file_path);
                MemoryManager::DestroyObject(header);
            }
            if (!level_created)
            {
                LOG_ERROR(ZEditor, "failed to create scene asset {}", new_file_path.generic_string());
                return;
            }
        }
        else
        {
            std::ofstream file(new_file_path);
            if (file.is_open())
            {
                file << "{\n";
                file << "    \"asset_type\": \"" << asset_type << "\",\n";
                file << "    \"name\": \"" << new_file_path.stem().string() << "\"\n";
                file << "}\n";
                file.close();
            }
        }

        RefreshProjectSelection(ctx, new_file_path);
    }
}
