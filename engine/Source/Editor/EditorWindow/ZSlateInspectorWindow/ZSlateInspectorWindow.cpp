#include "ZSlateInspectorWindow.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"
#include "Editor/EditorUI/EditorEulerHint.h"
#include "Editor/EditorUI/EditorSelection.h"
#include "Editor/EditorUI/PropertyDrawer/EditorPropertyDrawer.h"  // MakeDisplayLabel / MakeTypeHeaderLabel
#include "Editor/Undo/EditorUndo.h"                              // Ctrl+Z for every field commit
#include "Editor/Undo/UndoSystem.h"
#include "Editor/Prefab/PrefabInstance.h"                         // prefab instance section
#include "Editor/Prefab/PrefabOverrideModel.h"                    // implicit overrides / Apply / Revert
#include "Editor/Prefab/PrefabUtilityEditor.h"
#include "Runtime/Function/Framework/World/WorldManager.h"
#include "Editor/EditorWindow/InspectorWindow/InspectorAssetCommon.h"       // ResolveInspectorAssetType / IsGenericInspectorZAssetType
#include "Editor/EditorWindow/InspectorWindow/InspectorDataTableInspector.h"// ResolveDataTableType
#include "Editor/EditorWindow/InspectorWindow/InspectorShaderInspector.h"   // material-row enumerators / BuildShaderInspectorWidget
#include "Editor/ZSlate/Backend/EditorSlateHost.h"                          // native input bus (P10)
#include "Editor/ZSlate/Backend/ZSlateEditorOverlay.h"                      // native RHI backend (M2)
#include "Editor/EditorAsset/EditorAssetManager.h"  // EditorAssetManager::reimportAsset
#include "Editor/EditorAsset/SourceAssetRegistry.h"                         // native Texture2D inspector source row
#include "Editor/AssetPipeline/DataTableImporter/DataTableImporter.h"       // native DataTable inspector
#include "Editor/AssetPipeline/DataTableImporter/XlsxImporter.h"            // native DataTable inspector (xlsx)
#include "Runtime/BaseClasses/ObjectManager.h"                              // native DataTable inspector
#include "Runtime/BaseClasses/PropertyMeta.h"                               // ZPROPERTY(meta=(...)) registry
#include "Runtime/BaseClasses/TypeManager.h"                                // Type / TypeOf (DataTable)
#include "Runtime/Core/Memory/MemoryManager.h"                              // MemoryManager::DestroyObject
#include "Runtime/Resource/Asset/Data/DataTable.h"                          // DataTableBase
#include "Runtime/Platform/Path/Path.h"                                     // Path::GetFilePureName
#include "Editor/AssetPipeline/TextureImporter/TextureImporterSettings.h"   // native Texture2D inspector
#include "Editor/AssetPipeline/TextureImporter/TextureImportSettingsRegistry.h"
#include "Runtime/Function/Render/Texture/Texture2D.h"  // native Texture2D inspector (imported dims)
#include "Runtime/Project/ProjectInfo.h"                // resolve content-relative asset paths
#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Layout/SBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/SColorPicker.h"   // native Material color editing
#include "ZSlate/Widgets/SDragFloat.h"     // native Material scalar editing
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/SExpandableArea.h"  // collapsible inspector sections
#include "ZSlate/Widgets/SMenu.h"     // combo dropdown rows
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/SDropTarget.h"  // Content Browser asset drop targets
#include "ZSlate/Widgets/SSlider.h"   // compression-quality slider
#include "ZSlate/Widgets/STooltip.h"  // hover tooltips for meta=(Tooltip="...")
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "Runtime/UI/Core/Font.h"  // native Font asset inspector

#include "Runtime/BaseClasses/GameObject.h"
#include "Editor/EditorDragDrop/EditorDragDrop.h"
#include "Runtime/Core/Base/Macro.h"  // LOG_INFO / LOG_WARNING (ZDataTable)
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializationMetaFlags.h"
#include "Runtime/Core/Serialize/TypeTree.h"
#include "Runtime/Core/Serialize/TypeTreeCache.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "Runtime/Function/Framework/Component/Spine/SpineRenderer.h"
#include "Runtime/Function/Render/Spine/SpineGpuAssetFile.h"
#include "Runtime/Function/Framework/Component/Transform/Transform.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Components/Mesh.h"  // SubMeshRes
#include "Runtime/Resource/ResType/Data/Material.h"    // Material
#include "Runtime/Resource/ResType/Data/Shader.h"      // ShaderRes (native Material inspector)

#include <EASTL/string.h>

#ifdef _WIN32
#include <shellapi.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "Runtime/BaseClasses/Cast.h"

using namespace ZSlate;
using ZSlateInspectorDetail::ComponentEnableBinding;
using ZSlateInspectorDetail::FieldBinding;
using ZSlateInspectorDetail::FieldEditGate;
using ZSlateInspectorDetail::FieldKind;

namespace
{
    const ZSlate::UIColor kLabelColor(0.74f, 0.78f, 0.84f, 1.0f);
    const ZSlate::UIColor kHeaderColor(0.92f, 0.93f, 0.97f, 1.0f);
    const ZSlate::UIColor kComponentHeaderColor(0.62f, 0.78f, 0.96f, 1.0f);
    const ZSlate::UIColor kValueColor(0.85f, 0.87f, 0.92f, 1.0f);
    const ZSlate::UIColor kDimColor(0.50f, 0.52f, 0.58f, 1.0f);

    std::shared_ptr<STextBlock> MakeText(const char* text, float font_size, const ZSlate::UIColor& color)
    {
        auto t = std::make_shared<STextBlock>();
        t->Text = text;
        t->FontSize = font_size;
        t->Color = color;
        t->Alignment = ZSlate::TextAnchor::MiddleLeft;
        return t;
    }

    std::string FormatFloat(float v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return buf;
    }

    float ParseFloat(const std::string& s)
    {
        return s.empty() ? 0.0f : static_cast<float>(std::strtod(s.c_str(), nullptr));
    }

    // --- native Texture2D import-settings inspector state ---------------------
    // The Inspector is effectively a singleton panel, so the editing session
    // state lives at file scope (mirrors the static locals the legacy ImGui
    // drawer used). It is reloaded when the selected asset changes and rebuilt
    // into the widget tree on every settings change (m_ForceRebuild).
    std::filesystem::path s_tex_cached_path;
    TextureImporterSettings s_tex_settings;
    TextureImporterSettings::BuildTarget s_tex_target = TextureImporterSettings::BuildTarget::Default;
    bool s_tex_dirty = false;
    std::string s_tex_status;

    const int kTexMaxSizeChoices[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    const char* kTexFormatLabels[] = {
        "RGBA8", "RGB8", "RGBA16F", "BC7", "BC1 (DXT1)", "BC3 (DXT5)", "ASTC4x4", "ASTC6x6", "ASTC8x8"};
    const char* kTexPlatformLabels[] = {
        "Default", "PC (Standalone)", "Android", "iOS", "HarmonyOS (OHOS)", "WebGL"};

    int TexMaxSizeToIndex(int max_size)
    {
        const int clamped = TextureImporterSettings::ClampMaxSize(max_size);
        for (int i = 0; i < static_cast<int>(std::size(kTexMaxSizeChoices)); ++i)
        {
            if (kTexMaxSizeChoices[i] == clamped)
                return i;
        }
        return static_cast<int>(std::size(kTexMaxSizeChoices)) - 1;
    }

    std::filesystem::path ResolveTexAbsolutePath(const std::filesystem::path& asset_path)
    {
        if (asset_path.empty() || asset_path.is_absolute())
            return asset_path;
        const auto project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
            return asset_path;
        const auto content_root = project_info->GetProjectContent();
        if (content_root.empty())
            return asset_path;
        std::error_code ec;
        return std::filesystem::absolute(content_root / asset_path, ec);
    }

    // --- native Material inspector cached editing state -----------------------
    // Reloaded on selection change; live edits mutate it in place and are flushed
    // to disk on gesture end (see OnGUI). Field pointers handed to widgets via the
    // MaterialInspectorRow enumerators point into these singletons, so they must
    // outlive the widget tree (they do -- reassigned only on reload -> rebuild).
    //
    // Material / ShaderRes are Object subclasses whose ctor runs
    // InitializeRuntimeTypeInfo() against TypeManager; a file-scope static of
    // either would risk a static-init-order fiasco (type registrars may not have
    // run yet). Wrap them in Meyers-singleton accessors so they construct lazily
    // on first OnGUI call -- well after engine init. (Matches the legacy ImGui
    // material drawer, which used function-local statics for the same reason.)
    Material& MatRef()
    {
        static Material s_instance;
        return s_instance;
    }
    ShaderRes& MatShaderRef()
    {
        static ShaderRes s_instance;
        return s_instance;
    }

    std::filesystem::path s_mat_cached_path;  // the path used for ReadObject/save
    bool s_mat_loaded = false;
    bool s_mat_shader_loaded = false;
    eastl::string s_mat_shader_name;
    std::filesystem::path s_mat_shader_path;
    bool s_mat_dirty = false;
    std::string s_mat_status;

    void MarkMaterialEdited()
    {
        s_mat_dirty = true;
        NotifyInspectorLiveMaterialPreviewChanged();
    }

    bool SaveCachedMaterialToDisk()
    {
        if (!s_mat_loaded || s_mat_cached_path.empty())
        {
            return false;
        }

        auto asset_mgr = GET_SYSTEM(AssetManager);
        if (asset_mgr == nullptr)
        {
            return false;
        }

        std::filesystem::path save_path = s_mat_cached_path;
        save_path.replace_extension(".mat");
        if (!asset_mgr->WriteObjectToDiskThreadSafe(save_path, MatRef()))
        {
            return false;
        }

        if (save_path != s_mat_cached_path)
        {
            s_mat_cached_path = save_path;
        }
        return true;
    }

    void ReloadCachedMaterialFromDisk()
    {
        s_mat_shader_loaded = false;
        s_mat_shader_name.clear();
        s_mat_shader_path.clear();
        s_mat_dirty = false;
        s_mat_status.clear();

        if (s_mat_cached_path.empty() || !std::filesystem::exists(s_mat_cached_path))
        {
            s_mat_loaded = false;
            return;
        }

        s_mat_loaded = LoadMaterialDefinitionForInspector(MatRef(), s_mat_cached_path);
        if (s_mat_loaded)
        {
            SanitizeMaterialShaderBindingForInspector(MatRef());
        }
    }

    // --- native DataTable inspector ------------------------------------------
    // Mirrors the legacy ImGui drawer's per-cell reflection (RowColumn /
    // CollectRowColumns / FormatCell) and CSV edit-session (PR #7 write-back).
    // The cell type-string whitelist must match SerializeTraits<T>::GetTypeString.
    struct DTRowColumn
    {
        std::string name;
        std::string type_string;
        uint32_t byte_offset = 0;
        int32_t byte_size = 0;
        bool is_array = false;
        bool is_supported = false;
    };

    bool DTIsSupportedCellType(const char* ts)
    {
        if (ts == nullptr)
            return false;
        return std::strcmp(ts, "string") == 0 || std::strcmp(ts, "int") == 0 || std::strcmp(ts, "SInt32") == 0 ||
               std::strcmp(ts, "uint32_t") == 0 || std::strcmp(ts, "UInt32") == 0 || std::strcmp(ts, "int64_t") == 0 ||
               std::strcmp(ts, "SInt64") == 0 || std::strcmp(ts, "uint64_t") == 0 || std::strcmp(ts, "UInt64") == 0 ||
               std::strcmp(ts, "float") == 0 || std::strcmp(ts, "bool") == 0 || std::strcmp(ts, "char") == 0;
    }

    std::vector<DTRowColumn> DTCollectRowColumns(const TypeTree& tree)
    {
        std::vector<DTRowColumn> out;
        TypeTreeIterator child = tree.Root().Children();
        for (; !child.IsNull(); child = child.Next())
        {
            DTRowColumn col;
            col.name = child.Name().c_str();
            col.type_string = child.Type().c_str();
            col.byte_offset = child.HasByteOffset() ? child.ByteOffset() : 0;
            col.byte_size = child.ByteSize();
            col.is_array = child->IsArray();
            if (col.type_string == "string")  // strings are scalar eastl::string for inspection
                col.is_array = false;
            col.is_supported = !col.is_array && DTIsSupportedCellType(col.type_string.c_str());
            out.emplace_back(std::move(col));
        }
        return out;
    }

    std::string DTFormatCell(const DTRowColumn& col, const void* cell_ptr)
    {
        if (cell_ptr == nullptr)
            return "(unfmt)";
        const char* ts = col.type_string.c_str();
        char buf[512];
        if (std::strcmp(ts, "string") == 0)
            return reinterpret_cast<const eastl::string*>(cell_ptr)->c_str();
        if (std::strcmp(ts, "int") == 0 || std::strcmp(ts, "SInt32") == 0)
            std::snprintf(buf, sizeof(buf), "%d", *reinterpret_cast<const int32_t*>(cell_ptr));
        else if (std::strcmp(ts, "uint32_t") == 0 || std::strcmp(ts, "UInt32") == 0)
            std::snprintf(buf, sizeof(buf), "%u", *reinterpret_cast<const uint32_t*>(cell_ptr));
        else if (std::strcmp(ts, "int64_t") == 0 || std::strcmp(ts, "SInt64") == 0)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(*reinterpret_cast<const int64_t*>(cell_ptr)));
        else if (std::strcmp(ts, "uint64_t") == 0 || std::strcmp(ts, "UInt64") == 0)
            std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(cell_ptr)));
        else if (std::strcmp(ts, "float") == 0)
            std::snprintf(buf, sizeof(buf), "%g", *reinterpret_cast<const float*>(cell_ptr));
        else if (std::strcmp(ts, "bool") == 0)
            std::snprintf(buf, sizeof(buf), "%s", *reinterpret_cast<const bool*>(cell_ptr) ? "true" : "false");
        else if (std::strcmp(ts, "char") == 0)
        {
            const char c = *reinterpret_cast<const char*>(cell_ptr);
            if (c == 0)
                return std::string();
            std::snprintf(buf, sizeof(buf), "%c", c);
        }
        else
            return "(unfmt)";
        return buf;
    }

    // CSV edit session (single in-flight session per inspector; switching asset
    // discards pending edits, matching the legacy ImGui drawer's behaviour).
    // eastl::vector default-ctor does not allocate, so a file-scope static is
    // safe here (unlike the Object-derived Material above).
    struct DTEditSession
    {
        std::string asset_key;
        std::filesystem::path csv_path;
        eastl::vector<eastl::string> headers;
        eastl::vector<eastl::vector<eastl::string>> rows;
        bool loaded = false;
        bool dirty = false;
        eastl::string last_error;
        std::string filter;
    };
    DTEditSession s_dt;

    // --- generic field helpers ------------------------------------------------

    bool ClassifyType(const char* t, FieldKind& out)
    {
        if (std::strcmp(t, "float") == 0)
        {
            out = FieldKind::Float;
            return true;
        }
        if (std::strcmp(t, "int") == 0 || std::strcmp(t, "int32_t") == 0 || std::strcmp(t, "SInt32") == 0)
        {
            out = FieldKind::Int;
            return true;
        }
        if (std::strcmp(t, "uint32_t") == 0 || std::strcmp(t, "UInt32") == 0)
        {
            out = FieldKind::UInt;
            return true;
        }
        if (std::strcmp(t, "uint8_t") == 0 || std::strcmp(t, "UInt8") == 0 || std::strcmp(t, "unsigned char") == 0)
        {
            out = FieldKind::UInt8;
            return true;
        }
        if (std::strcmp(t, "bool") == 0)
        {
            out = FieldKind::Bool;
            return true;
        }
        if (std::strcmp(t, "string") == 0 || std::strcmp(t, "std::string") == 0 ||
            std::strcmp(t, "eastl::string") == 0)
        {
            out = FieldKind::Str;
            return true;
        }
        if (std::strcmp(t, "Vector3") == 0)
        {
            out = FieldKind::Vec3;
            return true;
        }
        return false;
    }

    std::string FormatScalar(FieldKind kind, const void* a)
    {
        char buf[64];
        switch (kind)
        {
            case FieldKind::Float:
                std::snprintf(buf, sizeof(buf), "%.3f", *static_cast<const float*>(a));
                return buf;
            case FieldKind::Int:
                std::snprintf(buf, sizeof(buf), "%d", *static_cast<const int*>(a));
                return buf;
            case FieldKind::UInt:
                std::snprintf(buf, sizeof(buf), "%u", *static_cast<const uint32_t*>(a));
                return buf;
            case FieldKind::UInt8:
                std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(*static_cast<const uint8_t*>(a)));
                return buf;
            case FieldKind::Bool:
                return *static_cast<const bool*>(a) ? "true" : "false";
            case FieldKind::Str:
                return std::string(static_cast<const eastl::string*>(a)->c_str());
            case FieldKind::Vec3:
            {
                const Vector3* v = static_cast<const Vector3*>(a);
                std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", v->x, v->y, v->z);
                return buf;
            }
        }
        return "";
    }

    void WriteScalar(FieldKind kind, void* a, const std::string& s)
    {
        switch (kind)
        {
            case FieldKind::Float:
                *static_cast<float*>(a) = static_cast<float>(std::strtod(s.c_str(), nullptr));
                break;
            case FieldKind::Int:
                *static_cast<int*>(a) = static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
                break;
            case FieldKind::UInt:
                *static_cast<uint32_t*>(a) = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
                break;
            case FieldKind::UInt8:
            {
                unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
                if (v > 255u)
                    v = 255u;
                *static_cast<uint8_t*>(a) = static_cast<uint8_t>(v);
                break;
            }
            case FieldKind::Str:
                *static_cast<eastl::string*>(a) = s.c_str();
                break;
            default:
                break;
        }
    }

    Component* ResolveSelectedComponent(size_t index)
    {
        auto go = EditorSelection::GetActiveGameObject().lock();
        if (!go)
            return nullptr;
        auto comps = go->getComponents();
        if (index >= comps.size())
            return nullptr;
        return comps[index];
    }

    // Every Inspector commit is one undo step: open the transaction, snapshot the
    // component, then let the caller write the field. The ScopedTransaction lives in
    // the caller's scope so the step closes after the write and its side effects.
    std::string MakeComponentUndoTitle(const Component* component)
    {
        return "Modify " + EditorPropertyDrawer::MakeTypeHeaderLabel(component->GetTypeName());
    }

    bool IsGameObjectActive(const std::shared_ptr<GameObject>& go)
    {
        if (!go)
            return false;
        // 与 GameObject::GetActiveSelf 同一套判定，别在这里再抄一份。
        return go->GetActiveSelf();
    }

    void NotifyAllComponentsUpdated(GameObject* go)
    {
        if (go == nullptr)
            return;
        for (const auto& component_ptr : go->getComponents())
        {
            Component* component = static_cast<Component*>(component_ptr);
            if (component != nullptr)
                component->OnSerializedFieldsUpdated();
        }
        if (auto scene = GET_SYSTEM(EditorSceneManager))
            scene->DrawSelectedEntityAxis();
    }

    // ---- prefab instance overrides (doc/PrefabSystem_Design.md §9.4 item 3) ----------
    // Unity model: editing a field of an object inside a prefab instance records an
    // override; editing it back to the source value removes it. Every field commit
    // below calls RecordPrefabOverridesForUndo() before and SyncPrefabOverride() after
    // the write, so the override list is part of the same undo step as the value.
    const ZSlate::UIColor kOverrideColor(0.45f, 0.70f, 1.0f, 1.0f);

    Level* ActiveLevel()
    {
        WorldManager* world = GET_SYSTEM(WorldManager);
        return world != nullptr ? world->getCurrentActiveLevel() : nullptr;
    }

    PrefabInstance* SelectedPrefabInstance()
    {
        auto go = EditorSelection::GetActiveGameObject().lock();
        return go ? PrefabOverrideModel::FindInstance(ActiveLevel(), go.get()) : nullptr;
    }

    void RecordPrefabOverridesForUndo()
    {
        PrefabInstance* instance = SelectedPrefabInstance();
        EditorUndoSystem* undo = EditorUndo::GetSystem();
        if (instance == nullptr || undo == nullptr || !EditorUndo::IsRecording())
            return;
        UndoSystem& core = undo->GetCore();
        if (!core.HasPendingRecord(PrefabOverrideModel::OverrideListUndoKey(instance)))
            core.AddRecord(PrefabOverrideModel::MakeOverrideListUndoRecord(instance), "Edit Prefab Override");
    }

    /// Undo snapshot of the instance's component structure (see MakeStructureUndoRecord).
    /// Call before anything that adds / detaches / re-introduces components.
    void RecordPrefabStructureForUndo()
    {
        PrefabInstance* instance = SelectedPrefabInstance();
        EditorUndoSystem* undo = EditorUndo::GetSystem();
        if (instance == nullptr || undo == nullptr || !EditorUndo::IsRecording())
            return;
        UndoSystem& core = undo->GetCore();
        if (!core.HasPendingRecord(PrefabOverrideModel::StructureUndoKey(instance)))
            core.AddRecord(PrefabOverrideModel::MakeStructureUndoRecord(instance), "Edit Prefab Structure");
    }

    void SyncPrefabOverride(Object* target, const std::string& path)
    {
        if (target == nullptr || path.empty())
            return;
        if (PrefabInstance* instance = SelectedPrefabInstance())
            PrefabOverrideModel::SyncFieldOverride(instance, target, eastl::string(path.c_str()));
    }

    /// Undo snapshot of every object an override list touches (Revert writes them).
    /// Property rows only: a RemovedComponent row's target is the SOURCE component —
    /// a shared prefab template — and an undo must never write into that.
    void RecordOverrideTargetsForUndo(PrefabInstance* instance, const Object* only_target)
    {
        for (const PrefabOverrideModel::OverrideRow& row : PrefabOverrideModel::ListOverrides(instance))
        {
            if (row.kind != PrefabOverrideModel::OverrideRow::Kind::Property)
                continue;
            Object* obj = static_cast<Object*>(row.target);
            if (only_target != nullptr && obj != only_target)
                continue;
            if (Component* c = Cast<Component>(obj))
                EditorUndo::RecordComponent(c);
        }
    }

    bool HasMetaFlag(TransferMetaFlags flags, TransferMetaFlags flag)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
    }

    // --- ZPROPERTY(meta=(...)) support ---------------------------------------
    //
    // ZHeaderTool emits one static ZPropertyMeta table per annotated class, keyed
    // by the SERIALIZED field name -- the exact string a TypeTree node carries --
    // so a row can look its metadata up without any extra plumbing.

    const ZPropertyMeta* FindFieldMeta(const Type* type, const char* serialized_name)
    {
        return type == nullptr ? nullptr : PropertyMetaRegistry::Find(type, serialized_name);
    }

    /// Row label: meta=(DisplayName="..") wins, else the usual prettified name.
    std::string MakeFieldLabel(const ZPropertyMeta* meta, const char* serialized_name)
    {
        if (meta != nullptr && meta->displayName != nullptr && meta->displayName[0] != '\0')
            return meta->displayName;
        return EditorPropertyDrawer::MakeDisplayLabel(serialized_name);
    }

    /// Enforce meta=(ClampMin=.., ClampMax=..) right after a scalar write. The
    /// tool rejects clamps on non-numeric properties, so Vec3/Str/Bool never
    /// reach here with bounds set.
    void ApplyClamp(FieldKind kind, void* a, const ZPropertyMeta* meta)
    {
        if (meta == nullptr || (!meta->hasClampMin && !meta->hasClampMax))
            return;
        switch (kind)
        {
            case FieldKind::Float:
            {
                float& v = *static_cast<float*>(a);
                v = static_cast<float>(meta->Clamp(v));
                break;
            }
            case FieldKind::Int:
            {
                int& v = *static_cast<int*>(a);
                v = static_cast<int>(meta->Clamp(v));
                break;
            }
            case FieldKind::UInt:
            {
                uint32_t& v = *static_cast<uint32_t*>(a);
                v = static_cast<uint32_t>(meta->Clamp(v));
                break;
            }
            case FieldKind::UInt8:
            {
                uint8_t& v = *static_cast<uint8_t*>(a);
                v = static_cast<uint8_t>(meta->Clamp(v));
                break;
            }
            default:
                break;
        }
    }

    /// True when the row may be edited: no EditCondition, or the gating bool
    /// (optionally negated) currently reads true. `c` is the owning component.
    bool IsGateOpen(const FieldEditGate& gate, const Component* c)
    {
        if (!gate.active || c == nullptr)
            return true;
        const bool value = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(c) + gate.condition_offset);
        return gate.negate ? !value : value;
    }

    // Depth-first search for a `bool` serialized field by name; returns its byte
    // offset inside the component so the Inspector can re-read it every frame.
    bool FindBoolFieldOffset(const TypeTreeIterator& node, const char* name, uint32_t& out_offset)
    {
        for (TypeTreeIterator child = node.Children(); !child.IsNull(); child = child.Next())
        {
            if (child->IsArray())
                continue;
            if (child.HasByteOffset() && child.Name() == name && child.Type() == "bool")
            {
                out_offset = child.ByteOffset();
                return true;
            }
            if (FindBoolFieldOffset(child, name, out_offset))
                return true;
        }
        return false;
    }

    /// Resolve meta=(EditCondition="[!]m_Flag") to a byte offset. Unresolved
    /// conditions (field missing / not a bool) leave the gate inactive -- the
    /// tool already rejects those at generate time, so this is belt-and-braces.
    FieldEditGate MakeEditGate(const ZPropertyMeta* meta, const TypeTreeIterator& root)
    {
        FieldEditGate gate;
        if (meta == nullptr || meta->editCondition == nullptr || meta->editCondition[0] == '\0')
            return gate;
        const char* name = meta->editCondition;
        if (name[0] == '!')
        {
            gate.negate = true;
            ++name;
        }
        uint32_t offset = 0;
        if (!FindBoolFieldOffset(root, name, offset))
            return FieldEditGate {};
        gate.condition_offset = offset;
        gate.active = true;
        return gate;
    }

    // --- hover tooltips ------------------------------------------------------
    //
    // ZSlate has no parent links, and the hovered widget is the DEEPEST one under
    // the cursor (a label's STextBlock, an axis box, ...), so identity matching
    // against a row would miss. We therefore keep one entry per annotated row and
    // hit-test the row's cached geometry against the pointer -- which also makes
    // the whole row (padding included) a tooltip target.

    struct TooltipEntry
    {
        std::weak_ptr<SWidget> row;
        std::string text;
    };
    std::vector<TooltipEntry> s_tooltips;

    void RegisterRowTooltip(const std::shared_ptr<SWidget>& row, const char* text)
    {
        if (row == nullptr || text == nullptr || text[0] == '\0')
            return;
        s_tooltips.push_back(TooltipEntry {row, std::string(text)});
    }

    const std::string* FindTooltipAt(const ZSlate::Vector2& mouse)
    {
        for (const TooltipEntry& entry : s_tooltips)
        {
            const std::shared_ptr<SWidget> row = entry.row.lock();
            if (row == nullptr)
                continue;
            const FGeometry& geom = row->GetCachedGeometry();
            if (geom.LocalSize.x <= 0.0f || geom.LocalSize.y <= 0.0f)
                continue;
            if (geom.IsUnderLocation(mouse))
                return &entry.text;
        }
        return nullptr;
    }

    std::shared_ptr<SBox> MakeLabelBox(const std::string& text, float scale, float width)
    {
        auto box = std::make_shared<SBox>();
        box->WidthOverride = width;
        box->VAlign = EVerticalAlignment::Center;
        box->SetContent(MakeText(text.c_str(), 14.0f * scale, kLabelColor));
        return box;
    }

    /// Returns the row widget (so callers can attach a tooltip / dim the label).
    std::shared_ptr<SHorizontalBox> AddFieldRow(const std::shared_ptr<SVerticalBox>& column,
                                               const std::string& label,
                                               const std::shared_ptr<SWidget>& content,
                                               int depth,
                                               float scale,
                                               std::shared_ptr<STextBlock>* out_label = nullptr)
    {
        // Wide enough for "Compression Quality" / "Generate Mip Maps" at 14px.
        constexpr float kLabelColumnWidth = 168.0f;
        auto row = std::make_shared<SHorizontalBox>();
        std::shared_ptr<STextBlock> label_text = MakeText(label.c_str(), 14.0f * scale, kLabelColor);
        auto label_box = std::make_shared<SBox>();
        label_box->WidthOverride = kLabelColumnWidth * scale;
        label_box->VAlign = EVerticalAlignment::Center;
        label_box->SetContent(label_text);
        row->AddSlot(label_box).AutoSize().SetVAlign(EVerticalAlignment::Center);
        row->AddSlot(content).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
        column->AddSlot(row).AutoSize().SetPadding(ZSlate::FMargin(depth * 12.0f * scale, 0.0f, 0.0f, 4.0f * scale));
        if (out_label != nullptr)
            *out_label = label_text;
        return row;
    }

    std::shared_ptr<SEditableTextBox> MakeScalarBox(FieldKind kind,
                                                    size_t comp_index,
                                                    uint32_t offset,
                                                    float scale,
                                                    const ZPropertyMeta* meta = nullptr,
                                                    const FieldEditGate& gate = FieldEditGate {},
                                                    const std::string& field_path = {})
    {
        auto f = std::make_shared<SEditableTextBox>();
        f->FontSize = 14.0f * scale;
        f->MinWidth = 60.0f * scale;
        f->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
        f->OnTextCommitted = [kind, comp_index, offset, meta, gate, field_path](const std::string& s) {
            Component* c = ResolveSelectedComponent(comp_index);
            if (c == nullptr || !IsGateOpen(gate, c))
                return;
            EditorUndo::ScopedTransaction undo_scope(MakeComponentUndoTitle(c));
            EditorUndo::RecordComponent(c);
            RecordPrefabOverridesForUndo();
            void* a = reinterpret_cast<char*>(c) + offset;
            WriteScalar(kind, a, s);
            ApplyClamp(kind, a, meta);
            c->OnSerializedFieldsUpdated();
            SyncPrefabOverride(c, field_path);
        };
        return f;
    }

    // Per-component build context: the component's Type* (meta lookup key) and
    // the root of its TypeTree (EditCondition offset resolution).
    struct FieldBuildContext
    {
        const Type* type {nullptr};
        TypeTreeIterator root;
        std::string path_prefix;  ///< property path of the parent node ("" at depth 0)
    };

    // Forward decl for recursion.
    void BuildFieldChildren(const std::shared_ptr<SVerticalBox>& column,
                            const TypeTreeIterator& parent,
                            std::vector<FieldBinding>& bindings,
                            size_t comp_index,
                            float scale,
                            int depth,
                            const FieldBuildContext& ctx);

    void BuildFieldNode(const std::shared_ptr<SVerticalBox>& column,
                        const TypeTreeIterator& node,
                        std::vector<FieldBinding>& bindings,
                        size_t comp_index,
                        float scale,
                        int depth,
                        const FieldBuildContext& ctx)
    {
        if (HasMetaFlag(node.MetaFlags(), HideInEditorMask))
            return;

        // Component::m_Enabled is toggled from the section header checkbox (Unity style).
        if (node.HasByteOffset() && node.Name() == "enabled")
            return;

        const bool read_only = HasMetaFlag(node.MetaFlags(), DisallowSerializedPropertyModification);
        const ZPropertyMeta* meta = FindFieldMeta(ctx.type, node.Name().c_str());
        const std::string label = MakeFieldLabel(meta, node.Name().c_str());
        const FieldEditGate gate = MakeEditGate(meta, ctx.root);
        const std::string field_path =
            ctx.path_prefix.empty() ? std::string(node.Name().c_str()) : ctx.path_prefix + "." + node.Name().c_str();

        // Every row of an annotated field gets its tooltip registered; rows keep
        // a pointer to their label so a closed EditCondition can dim it.
        std::shared_ptr<STextBlock> row_label;
        auto finish_row = [&](const std::shared_ptr<SHorizontalBox>& row, FieldBinding& b) {
            RegisterRowTooltip(row, meta != nullptr ? meta->tooltip : nullptr);
            b.meta = meta;
            b.gate = gate;
            b.row_label = row_label;
            b.prefab_path = field_path;
            bindings.push_back(b);
        };

        if (node.HasByteOffset())
        {
            const char* type_name = node.Type().c_str();
            const uint32_t offset = node.ByteOffset();
            FieldKind kind;
            if (ClassifyType(type_name, kind))
            {
                if (kind == FieldKind::Vec3)
                {
                    if (read_only)
                    {
                        auto value_label = MakeText("", 14.0f * scale, kValueColor);
                        auto row = AddFieldRow(column, label, value_label, depth, scale, &row_label);
                        FieldBinding b;
                        b.kind = kind;
                        b.component_index = comp_index;
                        b.byte_offset = offset;
                        b.read_only = true;
                        b.label = value_label;
                        finish_row(row, b);
                        return;
                    }

                    std::array<std::shared_ptr<SEditableTextBox>, 3> boxes;
                    auto axis_row = std::make_shared<SHorizontalBox>();
                    const char* axis_names[3] = {"X", "Y", "Z"};
                    for (int a = 0; a < 3; ++a)
                    {
                        auto box = std::make_shared<SEditableTextBox>();
                        box->FontSize = 14.0f * scale;
                        box->MinWidth = 44.0f * scale;
                        box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
                        boxes[static_cast<size_t>(a)] = box;

                        axis_row->AddSlot(MakeText(axis_names[a], 13.0f * scale, kDimColor))
                            .AutoSize()
                            .SetVAlign(EVerticalAlignment::Center)
                            .SetPadding(ZSlate::FMargin(a == 0 ? 0.0f : 6.0f * scale, 0.0f, 2.0f * scale, 0.0f));
                        axis_row->AddSlot(box).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
                    }
                    // Raw pointers avoid a shared_ptr cycle (box -> lambda -> box).
                    SEditableTextBox* p0 = boxes[0].get();
                    SEditableTextBox* p1 = boxes[1].get();
                    SEditableTextBox* p2 = boxes[2].get();
                    auto commit = [comp_index, offset, p0, p1, p2, gate, field_path](const std::string&) {
                        Component* c = ResolveSelectedComponent(comp_index);
                        if (c == nullptr || !IsGateOpen(gate, c))
                            return;
                        EditorUndo::ScopedTransaction undo_scope(MakeComponentUndoTitle(c));
                        EditorUndo::RecordComponent(c);
                        RecordPrefabOverridesForUndo();
                        Vector3& v = *reinterpret_cast<Vector3*>(reinterpret_cast<char*>(c) + offset);
                        v.x = ParseFloat(p0->Text);
                        v.y = ParseFloat(p1->Text);
                        v.z = ParseFloat(p2->Text);
                        c->OnSerializedFieldsUpdated();
                        SyncPrefabOverride(c, field_path);
                    };
                    for (int a = 0; a < 3; ++a)
                        boxes[static_cast<size_t>(a)]->OnTextCommitted = commit;

                    auto row = AddFieldRow(column, label, axis_row, depth, scale, &row_label);
                    FieldBinding b;
                    b.kind = kind;
                    b.component_index = comp_index;
                    b.byte_offset = offset;
                    b.boxes = boxes;
                    finish_row(row, b);
                    return;
                }

                if (kind == FieldKind::Bool)
                {
                    if (read_only)
                    {
                        auto value_label = MakeText("", 14.0f * scale, kValueColor);
                        auto row = AddFieldRow(column, label, value_label, depth, scale, &row_label);
                        FieldBinding b;
                        b.kind = kind;
                        b.component_index = comp_index;
                        b.byte_offset = offset;
                        b.read_only = true;
                        b.label = value_label;
                        finish_row(row, b);
                        return;
                    }
                    auto check = std::make_shared<SCheckBox>();
                    check->BoxSize = 16.0f * scale;
                    check->OnCheckStateChanged = [comp_index, offset, gate, field_path](bool v) {
                        Component* c = ResolveSelectedComponent(comp_index);
                        if (c == nullptr || !IsGateOpen(gate, c))
                            return;
                        EditorUndo::ScopedTransaction undo_scope(MakeComponentUndoTitle(c));
                        EditorUndo::RecordComponent(c);
                        RecordPrefabOverridesForUndo();
                        *reinterpret_cast<bool*>(reinterpret_cast<char*>(c) + offset) = v;
                        c->OnSerializedFieldsUpdated();
                        SyncPrefabOverride(c, field_path);
                    };
                    auto row = AddFieldRow(column, label, check, depth, scale, &row_label);
                    FieldBinding b;
                    b.kind = kind;
                    b.component_index = comp_index;
                    b.byte_offset = offset;
                    b.check = check;
                    finish_row(row, b);
                    return;
                }

                // Float / Int / UInt / UInt8 / Str.
                if (read_only)
                {
                    auto value_label = MakeText("", 14.0f * scale, kValueColor);
                    auto row = AddFieldRow(column, label, value_label, depth, scale, &row_label);
                    FieldBinding b;
                    b.kind = kind;
                    b.component_index = comp_index;
                    b.byte_offset = offset;
                    b.read_only = true;
                    b.label = value_label;
                    finish_row(row, b);
                    return;
                }
                auto box = MakeScalarBox(kind, comp_index, offset, scale, meta, gate, field_path);
                auto row = AddFieldRow(column, label, box, depth, scale, &row_label);
                FieldBinding b;
                b.kind = kind;
                b.component_index = comp_index;
                b.byte_offset = offset;
                b.boxes[0] = box;
                finish_row(row, b);
                return;
            }

            // Unsupported leaf (PPtr<>, enums, nested-by-value structs without a
            // recognized scalar type, ...). Show the type read-only so the layout
            // stays meaningful and the gap is obvious.
            const std::string placeholder = std::string("<") + type_name + ">";
            auto row = AddFieldRow(column, label, MakeText(placeholder.c_str(), 13.0f * scale, kDimColor), depth, scale,
                                   &row_label);
            RegisterRowTooltip(row, meta != nullptr ? meta->tooltip : nullptr);
            return;
        }

        if (node->IsArray())
        {
            auto row = AddFieldRow(column, label, MakeText("<array> (not editable)", 13.0f * scale, kDimColor), depth, scale,
                                   &row_label);
            RegisterRowTooltip(row, meta != nullptr ? meta->tooltip : nullptr);
            return;
        }

        const TypeTreeIterator first_child = node.Children();
        if (!first_child.IsNull())
        {
            column->AddSlot(MakeText(label.c_str(), 14.0f * scale, kLabelColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(depth * 12.0f * scale, 4.0f * scale, 0.0f, 2.0f * scale));
            if (depth < 8)
            {
                FieldBuildContext child_ctx = ctx;
                child_ctx.path_prefix = field_path;
                BuildFieldChildren(column, node, bindings, comp_index, scale, depth + 1, child_ctx);
            }
            return;
        }

        auto row = AddFieldRow(column, label, MakeText((std::string("<") + node.Type().c_str() + ">").c_str(),
                                                       13.0f * scale, kDimColor),
                               depth, scale, &row_label);
        RegisterRowTooltip(row, meta != nullptr ? meta->tooltip : nullptr);
    }

    void BuildFieldChildren(const std::shared_ptr<SVerticalBox>& column,
                            const TypeTreeIterator& parent,
                            std::vector<FieldBinding>& bindings,
                            size_t comp_index,
                            float scale,
                            int depth,
                            const FieldBuildContext& ctx)
    {
        for (TypeTreeIterator child = parent.Children(); !child.IsNull(); child = child.Next())
            BuildFieldNode(column, child, bindings, comp_index, scale, depth, ctx);
    }

    /// Draw one meta=(Category="..") group of depth-0 fields.
    void BuildCategoryGroup(const std::shared_ptr<SVerticalBox>& column,
                            const char* category,
                            const std::vector<TypeTreeIterator>& fields,
                            std::vector<FieldBinding>& bindings,
                            size_t comp_index,
                            float scale,
                            const FieldBuildContext& ctx)
    {
        if (category != nullptr && category[0] != '\0')
        {
            column->AddSlot(MakeText(category, 13.0f * scale, kDimColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 2.0f * scale));
        }
        for (const TypeTreeIterator& field : fields)
            BuildFieldNode(column, field, bindings, comp_index, scale, 1, ctx);
    }

    /// Depth-0 field list, grouped by meta=(Category="..") (ungrouped first, then
    /// one labelled block per category in first-seen order). Without any category
    /// annotation this is exactly the old flat walk.
    void BuildComponentFields(const std::shared_ptr<SVerticalBox>& column,
                              const TypeTreeIterator& root,
                              std::vector<FieldBinding>& bindings,
                              size_t comp_index,
                              float scale,
                              const FieldBuildContext& ctx)
    {
        std::vector<TypeTreeIterator> ungrouped;
        std::vector<std::pair<std::string, std::vector<TypeTreeIterator>>> groups;
        bool any_category = false;

        for (TypeTreeIterator child = root.Children(); !child.IsNull(); child = child.Next())
        {
            const ZPropertyMeta* meta = FindFieldMeta(ctx.type, child.Name().c_str());
            const char* category = (meta != nullptr) ? meta->category : nullptr;
            if (category == nullptr || category[0] == '\0')
            {
                ungrouped.push_back(child);
                continue;
            }
            any_category = true;
            auto it = std::find_if(groups.begin(), groups.end(),
                                   [category](const std::pair<std::string, std::vector<TypeTreeIterator>>& g) {
                                       return g.first == category;
                                   });
            if (it == groups.end())
            {
                groups.emplace_back(std::string(category), std::vector<TypeTreeIterator> {});
                groups.back().second.push_back(child);
            }
            else
            {
                it->second.push_back(child);
            }
        }

        if (!any_category)
        {
            BuildFieldChildren(column, root, bindings, comp_index, scale, 0, ctx);
            return;
        }

        for (const TypeTreeIterator& field : ungrouped)
            BuildFieldNode(column, field, bindings, comp_index, scale, 0, ctx);
        for (const auto& group : groups)
            BuildCategoryGroup(column, group.first.c_str(), group.second, bindings, comp_index, scale, ctx);
    }

    // --- BaseRenderer custom drawing -----------------------------------------

    BaseRenderer* ResolveSelectedRenderer(size_t comp_index)
    {
        return Cast<BaseRenderer>(ResolveSelectedComponent(comp_index));
    }

    // Loads the material asset (if any) and refreshes the read-only info labels
    // (status / shader / blend / double-sided). Called on build and whenever the
    // material string changes.
    void RefreshMaterialInfo(const eastl::string& material_asset,
                             STextBlock* status,
                             STextBlock* shader,
                             STextBlock* blend,
                             STextBlock* double_sided)
    {
        Material* material_res = nullptr;
        if (!material_asset.empty())
            material_res = GET_SYSTEM(AssetManager)->loadAsset<Material>(material_asset);

        if (material_res != nullptr)
        {
            if (status)
                status->Text = "Loaded";
            if (shader)
                shader->Text = material_res->GetShaderName().empty() ? "StandardLit"
                                                                     : std::string(material_res->GetShaderName().c_str());
            if (blend)
                blend->Text = material_res->m_IsBlend ? "On" : "Off";
            if (double_sided)
                double_sided->Text = material_res->m_IsDoubleSided ? "On" : "Off";
        }
        else
        {
            const char* status_text = material_asset.empty() ? "Not Set" : "Could not load";
            if (status)
                status->Text = status_text;
            if (shader)
                shader->Text = "-";
            if (blend)
                blend->Text = "-";
            if (double_sided)
                double_sided->Text = "-";
        }
    }

    std::shared_ptr<SEditableTextBox> MakeAssetBox(float scale)
    {
        auto box = std::make_shared<SEditableTextBox>();
        box->FontSize = 14.0f * scale;
        box->MinWidth = 60.0f * scale;
        box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
        return box;
    }

    // Turns an asset path box into a two-way asset slot: a Content Browser drop
    // TARGET (in) and a drag SOURCE (out).
    //
    // `wanted_type` is a resolved inspector asset type ("material", "meshdata");
    // empty accepts any `.zasset`. The drop writes the STORED (project-relative)
    // form and then runs the box's own commit handler, so dropping goes through
    // exactly the same code path as typing the path -- validation and the
    // read-only info labels cannot drift between the two.
    //
    // Drag-out reads the box's current text, so a path typed a second ago is the
    // one that travels.
    std::shared_ptr<SWidget> MakeAssetSlot(const std::shared_ptr<SEditableTextBox>& box, const std::string& wanted_type)
    {
        auto target = std::make_shared<SDropTarget>();
        target->SetContent(box);
        target->CanAcceptDrop = [wanted_type](const std::shared_ptr<FDragDropOperation>& op) -> bool {
            const FAssetDragDropOp* asset_op = EditorDragDrop::AsAssetDrop(op);
            if (asset_op == nullptr)
            {
                return false;
            }
            if (std::filesystem::path(asset_op->AssetPath).extension().generic_string() != ".zasset")
            {
                return false;
            }
            if (wanted_type.empty())
            {
                return true;
            }
            return ResolveInspectorAssetType(asset_op->AssetPath, std::string()) == wanted_type;
        };
        target->OnDropHandler = [box](const std::shared_ptr<FDragDropOperation>& op) {
            const FAssetDragDropOp* asset_op = EditorDragDrop::AsAssetDrop(op);
            if (asset_op == nullptr)
            {
                return;
            }
            const std::string stored_path = EditorDragDrop::MakeDroppedAssetStoredPath(asset_op->AssetPath);
            box->Text = stored_path;
            if (box->OnTextCommitted)
            {
                box->OnTextCommitted(stored_path);
            }
        };
        // A path row is both directions: drop one in, drag this one out (onto
        // another object's field, the Content Browser, ...). Drag-out wraps the
        // drop target, so both live on the same row without either knowing about
        // the other.
        return EditorDragDrop::MakeAssetPathDragSource(std::static_pointer_cast<SWidget>(target),
                                                       [box]() -> std::string { return box != nullptr ? box->Text : std::string(); });
    }

    void BuildBaseRendererSection(const std::shared_ptr<SVerticalBox>& column,
                                  BaseRenderer* renderer,
                                  size_t comp_index,
                                  std::vector<FieldBinding>& bindings,
                                  float scale)
    {
        const std::vector<SubMeshRes>& sub_meshes = renderer->getSubMeshes();
        const std::vector<eastl::string>& shared_material_assets = renderer->getSharedMaterialAssets();

        {
            auto count_label = MakeText("0", 14.0f * scale, kValueColor);
            count_label->Text = std::to_string(sub_meshes.size());
            AddFieldRow(column, "Sub Mesh Count", count_label, 0, scale);
        }

        if (sub_meshes.empty())
        {
            column->AddSlot(MakeText("No sub meshes assigned.", 13.0f * scale, kDimColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 2.0f * scale, 0.0f, 0.0f));
            return;
        }

        for (size_t s = 0; s < sub_meshes.size(); ++s)
        {
            const std::string element_label = "Element " + std::to_string(s);
            column->AddSlot(MakeText(element_label.c_str(), 14.0f * scale, kLabelColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 2.0f * scale));

            // Mesh asset (editable string -> SetSubMeshes).
            auto mesh_box = MakeAssetBox(scale);
            mesh_box->Text = std::string(sub_meshes[s].m_MeshAsset.c_str());
            mesh_box->OnTextCommitted = [comp_index, s](const std::string& text) {
                BaseRenderer* r = ResolveSelectedRenderer(comp_index);
                if (r == nullptr)
                    return;
                std::vector<SubMeshRes> subs = r->getSubMeshes();
                if (s >= subs.size())
                    return;
                EditorUndo::ScopedTransaction undo_scope("Set Mesh");
                EditorUndo::RecordComponent(r);
                subs[s].m_MeshAsset = text.c_str();
                r->SetSubMeshes(subs);
                r->OnSerializedFieldsUpdated();
            };
            // Drop target: drag a MeshData .zasset from the Content Browser.
            AddFieldRow(column, "Mesh", MakeAssetSlot(mesh_box, "meshdata"), 1, scale);

            // Material asset (editable string -> SetSharedMaterialAssets).
            auto material_box = MakeAssetBox(scale);
            eastl::string current_material;
            if (s < shared_material_assets.size())
                current_material = shared_material_assets[s];
            else if (shared_material_assets.size() == 1)
                current_material = shared_material_assets.front();
            material_box->Text = std::string(current_material.c_str());

            // Read-only material info labels.
            auto status_label = MakeText("", 14.0f * scale, kValueColor);
            auto shader_label = MakeText("", 14.0f * scale, kValueColor);
            auto blend_label = MakeText("", 14.0f * scale, kValueColor);
            auto double_sided_label = MakeText("", 14.0f * scale, kValueColor);

            STextBlock* status_raw = status_label.get();
            STextBlock* shader_raw = shader_label.get();
            STextBlock* blend_raw = blend_label.get();
            STextBlock* double_sided_raw = double_sided_label.get();

            material_box->OnTextCommitted =
                [comp_index, s, status_raw, shader_raw, blend_raw, double_sided_raw](const std::string& text) {
                    BaseRenderer* r = ResolveSelectedRenderer(comp_index);
                    if (r == nullptr)
                        return;
                    EditorUndo::ScopedTransaction undo_scope("Set Material");
                    EditorUndo::RecordComponent(r);
                    std::vector<eastl::string> mats = r->getSharedMaterialAssets();
                    if (mats.size() <= s)
                        mats.resize(s + 1);
                    mats[s] = text.c_str();
                    r->SetSharedMaterialAssets(mats);
                    r->OnSerializedFieldsUpdated();
                    RefreshMaterialInfo(text.c_str(), status_raw, shader_raw, blend_raw, double_sided_raw);
                };

            // Drop target: drag a Material .zasset from the Content Browser.
            AddFieldRow(column, "Material", MakeAssetSlot(material_box, "material"), 1, scale);
            AddFieldRow(column, "Status", status_label, 1, scale);
            AddFieldRow(column, "Shader", shader_label, 1, scale);
            AddFieldRow(column, "Blend", blend_label, 1, scale);
            AddFieldRow(column, "Double Sided", double_sided_label, 1, scale);

            RefreshMaterialInfo(current_material, status_raw, shader_raw, blend_raw, double_sided_raw);

            // Per-sub-mesh local Transform (Position / Rotation(euler) / Scale).
            // Written back through SetSubMeshes(copy); rotation uses the shared
            // euler hint keyed on a stable (renderer, index) pointer so the
            // displayed degrees don't flip under gimbal aliasing.
            const void* euler_key = reinterpret_cast<const char*>(renderer) + s;

            std::array<SEditableTextBox*, 3> pos_raw {};
            std::array<SEditableTextBox*, 3> rot_raw {};
            std::array<SEditableTextBox*, 3> scale_raw {};

            auto make_xform_row = [&](const char* label, int which, std::array<SEditableTextBox*, 3>& out_raw) {
                auto axis_row = std::make_shared<SHorizontalBox>();
                const char* axis_names[3] = {"X", "Y", "Z"};
                std::array<std::shared_ptr<SEditableTextBox>, 3> boxes;
                for (int a = 0; a < 3; ++a)
                {
                    auto box = std::make_shared<SEditableTextBox>();
                    box->FontSize = 14.0f * scale;
                    box->MinWidth = 44.0f * scale;
                    box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
                    boxes[static_cast<size_t>(a)] = box;
                    out_raw[static_cast<size_t>(a)] = box.get();

                    axis_row->AddSlot(MakeText(axis_names[a], 13.0f * scale, kDimColor))
                        .AutoSize()
                        .SetVAlign(EVerticalAlignment::Center)
                        .SetPadding(ZSlate::FMargin(a == 0 ? 0.0f : 6.0f * scale, 0.0f, 2.0f * scale, 0.0f));
                    axis_row->AddSlot(box).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
                }
                SEditableTextBox* p0 = boxes[0].get();
                SEditableTextBox* p1 = boxes[1].get();
                SEditableTextBox* p2 = boxes[2].get();
                auto commit = [comp_index, s, which, euler_key, p0, p1, p2](const std::string&) {
                    BaseRenderer* r = ResolveSelectedRenderer(comp_index);
                    if (r == nullptr)
                        return;
                    std::vector<SubMeshRes> subs = r->getSubMeshes();
                    if (s >= subs.size())
                        return;
                    EditorUndo::ScopedTransaction undo_scope("Modify Sub Mesh Transform");
                    EditorUndo::RecordComponent(r);
                    TransformTRS& t = subs[s].m_Transform;
                    Vector3 v;
                    v.x = ParseFloat(p0->Text);
                    v.y = ParseFloat(p1->Text);
                    v.z = ParseFloat(p2->Text);
                    if (which == 0)
                    {
                        t.m_Position = Vector3d(v);
                    }
                    else if (which == 2)
                    {
                        t.m_Scale = v;
                    }
                    else
                    {
                        const Quaternion q = EditorEuler::MakeQuaternionFromEulerDegrees(v);
                        t.m_Rotation = q;
                        EditorEuler::SetEulerHint(euler_key, v, q);
                    }
                    r->SetSubMeshes(subs);
                    r->OnSerializedFieldsUpdated();
                    if (auto scene = GET_SYSTEM(EditorSceneManager))
                        scene->DrawSelectedEntityAxis();
                };
                for (int a = 0; a < 3; ++a)
                    boxes[static_cast<size_t>(a)]->OnTextCommitted = commit;
                AddFieldRow(column, label, axis_row, 1, scale);
            };

            make_xform_row("Position", 0, pos_raw);
            make_xform_row("Rotation", 1, rot_raw);
            make_xform_row("Scale", 2, scale_raw);

            // Custom per-frame sync: refresh editable boxes (when unfocused) and the
            // material info labels (only when the material string actually changes,
            // to avoid reloading the asset every frame).
            SEditableTextBox* mesh_raw = mesh_box.get();
            SEditableTextBox* material_raw = material_box.get();
            auto last_material = std::make_shared<eastl::string>("\x01");  // sentinel != any real value

            FieldBinding b;
            b.component_index = comp_index;
            b.custom_sync = [comp_index, s, mesh_raw, material_raw, status_raw, shader_raw, blend_raw, double_sided_raw,
                             last_material, euler_key, pos_raw, rot_raw, scale_raw]() {
                BaseRenderer* r = ResolveSelectedRenderer(comp_index);
                if (r == nullptr)
                    return;
                const std::vector<SubMeshRes>& subs = r->getSubMeshes();
                const std::vector<eastl::string>& mats = r->getSharedMaterialAssets();

                if (mesh_raw && !mesh_raw->IsFocused() && s < subs.size())
                    mesh_raw->Text = std::string(subs[s].m_MeshAsset.c_str());

                eastl::string mat_val;
                if (s < mats.size())
                    mat_val = mats[s];
                else if (mats.size() == 1)
                    mat_val = mats.front();

                if (material_raw && !material_raw->IsFocused())
                    material_raw->Text = std::string(mat_val.c_str());

                if (*last_material != mat_val)
                {
                    *last_material = mat_val;
                    RefreshMaterialInfo(mat_val, status_raw, shader_raw, blend_raw, double_sided_raw);
                }

                if (s < subs.size())
                {
                    const TransformTRS& t = subs[s].m_Transform;
                    const Vector3 p = t.m_Position.ToVector3();
                    const Vector3& sc = t.m_Scale;
                    const Vector3 euler = EditorEuler::GetEulerHint(euler_key, t.m_Rotation);
                    for (int a = 0; a < 3; ++a)
                    {
                        const size_t ai = static_cast<size_t>(a);
                        if (pos_raw[ai] && !pos_raw[ai]->IsFocused())
                            pos_raw[ai]->Text = FormatFloat(p[ai]);
                        if (rot_raw[ai] && !rot_raw[ai]->IsFocused())
                            rot_raw[ai]->Text = FormatFloat(euler[ai]);
                        if (scale_raw[ai] && !scale_raw[ai]->IsFocused())
                            scale_raw[ai]->Text = FormatFloat(sc[ai]);
                    }
                }
            };
            bindings.push_back(b);
        }
    }

    AnimationComponent* ResolveSelectedAnimationComponent(size_t index)
    {
        return Cast<AnimationComponent>(ResolveSelectedComponent(index));
    }

    // AnimationComponent gets its own layout: the generic TypeTree walk shows
    // `blend_clip_file_path` as "<array> (not editable)", which is exactly the
    // slot a dragged clip needs to land in.
    void BuildAnimationComponentSection(const std::shared_ptr<SVerticalBox>& column,
                                        AnimationComponent* anim,
                                        size_t comp_index,
                                        std::vector<FieldBinding>& bindings,
                                        float scale)
    {
        // Every row is an asset path box that also accepts a Content Browser /
        // Animator drop of the matching type.
        auto add_asset_row = [&](const char* label,
                                 const std::string& value,
                                 const std::string& wanted_type,
                                 void (*apply)(AnimationComponent*, const std::string&)) {
            auto box = MakeAssetBox(scale);
            box->Text = value;
            box->OnTextCommitted = [comp_index, apply](const std::string& text) {
                AnimationComponent* a = ResolveSelectedAnimationComponent(comp_index);
                if (a == nullptr)
                {
                    return;
                }
                EditorUndo::ScopedTransaction undo_scope(MakeComponentUndoTitle(a));
                EditorUndo::RecordComponent(a);
                apply(a, text);
                a->OnSerializedFieldsUpdated();
            };
            AddFieldRow(column, label, MakeAssetSlot(box, wanted_type), 1, scale);
        };

        add_asset_row("Controller", std::string(anim->GetControllerPath().c_str()), "animatorcontroller",
                      [](AnimationComponent* a, const std::string& text) {
                          a->SetControllerPath(text.c_str());
                          // The controller is resolved in PostLoadResource; without
                          // re-running it the new path would only apply on reload.
                          a->PostLoadResource(a->GetParentObject());
                      });
        add_asset_row("Skeleton", std::string(anim->GetSkeletonPath().c_str()), "skeletondata",
                      [](AnimationComponent* a, const std::string& text) {
                          a->SetSkeletonPath(text.c_str());
                          a->PostLoadResource(a->GetParentObject());
                      });
        add_asset_row("Clip", std::string(anim->GetClipPath().c_str()), "animationasset",
                      [](AnimationComponent* a, const std::string& text) { a->SetClipPath(text.c_str()); });
        add_asset_row("Anim Skel Map", std::string(anim->GetAnimSkelMapPath().c_str()), "animskelmap",
                      [](AnimationComponent* a, const std::string& text) { a->SetAnimSkelMapPath(text.c_str()); });

        // Length is needed because a clip stores frames, not seconds: there is
        // nothing to derive it from, so it has to be authored.
        auto length_box = MakeScalarBox(FieldKind::Float, comp_index, 0, scale);
        length_box->Text = FormatFloat(anim->GetClipLength());
        length_box->OnTextCommitted = [comp_index](const std::string& text) {
            AnimationComponent* a = ResolveSelectedAnimationComponent(comp_index);
            if (a == nullptr)
            {
                return;
            }
            EditorUndo::ScopedTransaction undo_scope("Set Clip Length");
            EditorUndo::RecordComponent(a);
            a->SetClipLength(ParseFloat(text));
            a->OnSerializedFieldsUpdated();
        };
        AddFieldRow(column, "Clip Length (s)", length_box, 1, scale);

        column->AddSlot(MakeText("A clip only plays together with its bone map.", 12.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));
    }

    // 测试接缝：本页建出来的组件下拉框（标签 → 选项 + 选中回调）。`inspector.pick` 控制台命令照它调同一个
    // on_select —— 与鼠标点开 popup 再点一项走的是**同一条**路径（撤销事务 + 改字段 + OnSerializedFieldsUpdated），
    // 只是不经过 popup。每次 BuildForObject 开头清空（回调里捕获的是组件下标，页一重建就作废）。
    struct ComboHook
    {
        std::vector<std::string> options;
        std::function<void(int)> on_select;
    };
    std::map<std::string, ComboHook> g_combo_hooks;

    // 组件段落里要用到的窗口服务（下拉框要窗口的 popup；改了资产要整页重建让下拉框换选项）
    struct SectionServices
    {
        std::function<std::shared_ptr<SWidget>(const std::string& current, std::vector<std::string> options,
                                               std::function<void(int)> on_select)>
            make_combo;
        std::function<void()> request_rebuild;
    };

    SpineRenderer* ResolveSelectedSpineRenderer(size_t index)
    {
        return Cast<SpineRenderer>(ResolveSelectedComponent(index));
    }

    // SpineRenderer：资产槽（接 Content Browser 拖来的 .zspg）+ 动画 / 皮肤 / 朝向下拉（选项读自资产本身，
    // SpineGpuGetAssetInfo 按文件修改时间缓存）+ 其余字段走通用 TypeTree 绘制。
    // 下拉选中 = 撤销事务 + 改序列化字段 + OnSerializedFieldsUpdated（组件按差异补发给渲染器）。
    void BuildSpineRendererSection(const std::shared_ptr<SVerticalBox>& column,
                                   SpineRenderer* spine,
                                   size_t comp_index,
                                   std::vector<FieldBinding>& bindings,
                                   float scale,
                                   const SectionServices& services)
    {
        const auto edit = [comp_index](const char* title, const std::function<void(SpineRenderer*)>& apply) {
            SpineRenderer* sr = ResolveSelectedSpineRenderer(comp_index);
            if (sr == nullptr)
            {
                return;
            }
            EditorUndo::ScopedTransaction undo_scope(title);
            EditorUndo::RecordComponent(sr);
            apply(sr);
            sr->OnSerializedFieldsUpdated();
        };

        // ---- Skeleton Data：路径框 + .zspg 拖放 ----
        auto box = MakeAssetBox(scale);
        box->Text = spine->GetSkeletonDataAsset().c_str();
        const std::function<void()> rebuild = services.request_rebuild;
        box->OnTextCommitted = [edit, rebuild](const std::string& text) {
            edit("Set Skeleton Data", [&text](SpineRenderer* sr) { sr->SetSkeletonDataAsset(text.c_str()); });
            if (rebuild)
            {
                rebuild();   // 动画 / 皮肤下拉要换成新资产的选项
            }
        };
        auto target = std::make_shared<SDropTarget>();
        target->SetContent(box);
        target->CanAcceptDrop = [](const std::shared_ptr<FDragDropOperation>& op) -> bool {
            const FAssetDragDropOp* asset_op = EditorDragDrop::AsAssetDrop(op);
            return asset_op != nullptr && AssetManager::IsSpineSkeletonAssetPath(std::filesystem::path(asset_op->AssetPath));
        };
        target->OnDropHandler = [box](const std::shared_ptr<FDragDropOperation>& op) {
            const FAssetDragDropOp* asset_op = EditorDragDrop::AsAssetDrop(op);
            if (asset_op == nullptr)
            {
                return;
            }
            const std::string stored_path = EditorDragDrop::MakeDroppedAssetStoredPath(asset_op->AssetPath);
            box->Text = stored_path;
            if (box->OnTextCommitted)
            {
                box->OnTextCommitted(stored_path);
            }
        };
        AddFieldRow(column, "Skeleton Data",
                    EditorDragDrop::MakeAssetPathDragSource(std::static_pointer_cast<SWidget>(target),
                                                            [box]() -> std::string { return box != nullptr ? box->Text : std::string(); }),
                    1, scale);

        // ---- 资产元数据：状态行 + 下拉选项 ----
        const std::shared_ptr<const SpineGpuAssetInfo> info = spine->GetAssetInfo();
        std::string status;
        if (spine->GetSkeletonDataAsset().empty())
        {
            status = "No skeleton data. Drag a .zspg from the Content Browser.";
        }
        else if (!info->error.empty())
        {
            status = "Could not load: " + info->error;
        }
        else
        {
            status = std::to_string(info->animations.size()) + " animation(s), " + std::to_string(info->skins.size()) +
                     " skin(s), " + std::to_string(info->bone_count) + " bones, " + std::to_string(info->slot_count) +
                     " slots, " + std::to_string(info->event_names.size()) + " event(s)";
        }
        AddFieldRow(column, "", MakeText(status.c_str(), 12.0f * scale, kDimColor), 1, scale);
        // 用户事件（v16）：只读列出资产里定义的事件名（带音频的标出来），脚本按这些名字分支
        if (info->error.empty() && !info->event_names.empty())
        {
            std::string events;
            for (size_t i = 0; i < info->event_names.size(); ++i)
            {
                events += (i == 0 ? "" : ", ") + info->event_names[i];
                if (i < info->event_audio.size() && !info->event_audio[i].empty())
                {
                    events += " (audio)";
                }
            }
            AddFieldRow(column, "Events", MakeText(events.c_str(), 12.0f * scale, kDimColor), 1, scale);
        }
        if (!spine->GetSkeletonDataGuid().empty())
        {
            AddFieldRow(column, "GUID", MakeText(spine->GetSkeletonDataGuid().c_str(), 12.0f * scale, kDimColor), 1, scale);
        }

        const auto add_combo = [&](const char* label, const std::string& current, std::vector<std::string> options,
                                   std::function<void(int)> on_select) {
            const std::function<void()> rebuild_after = services.request_rebuild;
            g_combo_hooks[label] = ComboHook {options, [on_select, rebuild_after](int i) {
                                                  on_select(i);
                                                  if (rebuild_after)
                                                  {
                                                      rebuild_after();   // 同 MakeComboButton 选中后的 m_ForceRebuild
                                                  }
                                              }};
            if (services.make_combo)
            {
                AddFieldRow(column, label, services.make_combo(current, std::move(options), std::move(on_select)), 1, scale);
            }
        };

        // Animation：选项 = 资产里的动画名；空 = 第 0 个
        {
            std::vector<std::string> options = info->animations;
            const std::string current = spine->GetAnimationNameField().empty()
                                            ? (options.empty() ? std::string("<none>") : options.front() + " (default)")
                                            : std::string(spine->GetAnimationNameField().c_str());
            if (!options.empty())
            {
                add_combo("Animation", current, options, [edit, options](int i) {
                    const std::string name = options[static_cast<size_t>(i)];
                    edit("Set Spine Animation", [&name](SpineRenderer* sr) { sr->SetAnimationNameField(name.c_str()); });
                });
            }
            else
            {
                AddFieldRow(column, "Animation", MakeText(current.c_str(), 14.0f * scale, kDimColor), 1, scale);
            }
        }

        // Initial Skin：<default only> + 资产里的皮肤名
        {
            std::vector<std::string> options;
            options.push_back("<default only>");
            options.insert(options.end(), info->skins.begin(), info->skins.end());
            const std::string current = spine->GetInitialSkinName().empty() ? std::string("<default only>")
                                                                            : std::string(spine->GetInitialSkinName().c_str());
            add_combo("Initial Skin", current, options, [edit, options](int i) {
                const std::string name = i == 0 ? std::string() : options[static_cast<size_t>(i)];
                edit("Set Spine Skin", [&name](SpineRenderer* sr) { sr->SetInitialSkinName(name.c_str()); });
            });
        }

        // Face Camera（通用绘制不支持枚举）
        {
            static const char* const kModes[] = {"none", "full", "vertical"};
            const int mode = std::clamp(static_cast<int>(spine->GetBillboard()), 0, 2);
            add_combo("Face Camera", kModes[mode], std::vector<std::string>(std::begin(kModes), std::end(kModes)),
                      [edit](int i) {
                          edit("Set Spine Face Camera",
                               [i](SpineRenderer* sr) { sr->SetBillboard(static_cast<SpineBillboardMode>(i)); });
                      });
        }

        // 其余字段（Loop / Time Scale / Scale）走通用绘制：上面画过的跳过
        TypeTree type_tree;
        if (!GET_SYSTEM(TypeTreeCache)->GetTypeTree(spine, kDontRequireAllMetaFlags, type_tree))
        {
            return;
        }
        FieldBuildContext ctx;
        ctx.type = spine->GetType();
        ctx.root = type_tree.Root();
        static const char* const kCustom[] = {"skeleton_data_asset", "skeleton_data_guid", "animation_name",
                                              "initial_skin_name", "billboard"};
        for (TypeTreeIterator child = ctx.root.Children(); !child.IsNull(); child = child.Next())
        {
            const std::string name = child.Name().c_str();
            if (std::find(std::begin(kCustom), std::end(kCustom), name) != std::end(kCustom))
            {
                continue;
            }
            BuildFieldNode(column, child, bindings, comp_index, scale, 1, ctx);
        }
    }

    void BuildComponentSection(const std::shared_ptr<SVerticalBox>& column,
                               Component* component,
                               size_t comp_index,
                               std::vector<FieldBinding>& bindings,
                               std::vector<ComponentEnableBinding>& enable_bindings,
                               float scale,
                               const SectionServices& services)
    {
        const std::string header = EditorPropertyDrawer::MakeTypeHeaderLabel(component->GetTypeName());

        auto header_row = std::make_shared<SHorizontalBox>();
        auto enable_check = std::make_shared<SCheckBox>();
        enable_check->BoxSize = 18.0f * scale;
        enable_check->Checked = component->isEnabled();
        enable_check->OnCheckStateChanged = [comp_index](bool enabled) {
            Component* c = ResolveSelectedComponent(comp_index);
            if (c == nullptr)
                return;
            EditorUndo::ScopedTransaction undo_scope(std::string(enabled ? "Enable " : "Disable ") +
                                                     EditorPropertyDrawer::MakeTypeHeaderLabel(c->GetTypeName()));
            EditorUndo::RecordComponent(c);
            c->SetEnabled(enabled);
            c->OnSerializedFieldsUpdated();
            if (auto scene = GET_SYSTEM(EditorSceneManager))
                scene->DrawSelectedEntityAxis();
        };
        header_row->AddSlot(enable_check)
            .AutoSize()
            .SetVAlign(EVerticalAlignment::Center)
            .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 8.0f * scale, 0.0f));

        auto title = MakeText(header.c_str(), 16.0f * scale, kComponentHeaderColor);
        header_row->AddSlot(title).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
        column->AddSlot(header_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 12.0f * scale, 0.0f, 6.0f * scale));

        ComponentEnableBinding enable_binding;
        enable_binding.component_index = comp_index;
        enable_binding.check = enable_check;
        enable_binding.title = title;
        enable_bindings.push_back(enable_binding);

        // BaseRenderer (incl. MeshRenderer) gets a dedicated layout: per-sub-mesh
        // mesh + material assets with read-only material introspection, mirroring
        // the legacy ImGui DrawBaseRendererComponentValue.
        if (BaseRenderer* renderer = Cast<BaseRenderer>(component))
        {
            BuildBaseRendererSection(column, renderer, comp_index, bindings, scale);
            return;
        }
        if (AnimationComponent* anim = Cast<AnimationComponent>(component))
        {
            BuildAnimationComponentSection(column, anim, comp_index, bindings, scale);
            return;
        }
        if (SpineRenderer* spine = Cast<SpineRenderer>(component))
        {
            BuildSpineRendererSection(column, spine, comp_index, bindings, scale, services);
            return;
        }

        TypeTree type_tree;
        if (!GET_SYSTEM(TypeTreeCache)->GetTypeTree(component, kDontRequireAllMetaFlags, type_tree))
        {
            column->AddSlot(MakeText("Unable to build serialized type tree.", 13.0f * scale, kDimColor)).AutoSize();
            return;
        }
        FieldBuildContext ctx;
        ctx.type = component->GetType();
        ctx.root = type_tree.Root();
        BuildComponentFields(column, ctx.root, bindings, comp_index, scale, ctx);
    }
}  // namespace

ZSlateInspectorWindow::ZSlateInspectorWindow(EditorUI* editor_ui)
    : EditorWindow(editor_ui, EditorLayoutWindowIds::kInspector)
{
    m_Open = true;
    BuildEmpty(1.0f);
}

void ZSlateInspectorWindow::BuildEmpty(float scale)
{
    m_GoActiveCheck = nullptr;
    m_ComponentEnableBindings.clear();
    m_NameLabel = nullptr;
    m_PositionFields = {};
    m_RotationFields = {};
    m_ScaleFields = {};
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs

    auto msg = MakeText("Nothing selected", 16.0f * scale, kDimColor);
    msg->Alignment = ZSlate::TextAnchor::MiddleCenter;

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
    border->Padding = ZSlate::FMargin(16.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;
    border->SetContent(msg);
    m_Root = border;
}

void ZSlateInspectorWindow::BuildFontAsset(const std::filesystem::path& asset_path, float scale)
{
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs
    m_NameLabel = nullptr;
    m_PositionFields = {};
    m_RotationFields = {};
    m_ScaleFields = {};

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(MakeText("Font", 20.0f * scale, kHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale));

    auto asset_manager = GET_SYSTEM(AssetManager);
    std::filesystem::path read_path = asset_path;
    Font* font = asset_manager != nullptr ? asset_manager->ReadObject<Font>(read_path) : nullptr;

    if (font == nullptr)
    {
        column->AddSlot(MakeText("Failed to load Font asset.", 14.0f * scale, kDimColor)).AutoSize();
    }
    else
    {
        const eastl::string& rel = font->GetSourceRelPath();
        AddFieldRow(column, "Source (rel)",
                    MakeText(rel.empty() ? "-" : rel.c_str(), 14.0f * scale, kValueColor), 0, scale);
        AddFieldRow(column, "Absolute",
                    MakeText(font->GetSourcePath().c_str(), 13.0f * scale, kDimColor), 0, scale);

        const std::string path_str = asset_path.generic_string();

        auto size_box = std::make_shared<SEditableTextBox>();
        size_box->FontSize = 14.0f * scale;
        size_box->MinWidth = 60.0f * scale;
        size_box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
        size_box->Text = std::to_string(font->GetDefaultSize());
        size_box->OnTextCommitted = [path_str](const std::string& s) {
            const int v = static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
            if (v <= 0)
                return;
            auto am = GET_SYSTEM(AssetManager);
            if (am == nullptr)
                return;
            std::filesystem::path p = path_str;
            if (Font* f = am->ReadObject<Font>(p))
                f->SetDefaultSize(v);
        };
        AddFieldRow(column, "Default Size", size_box, 0, scale);

        auto reimport = std::make_shared<SButton>();
        reimport->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        reimport->SetContent(MakeText("Reimport", 14.0f * scale, kValueColor));
        reimport->OnClicked = [path_str]() {
            if (auto editor_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
                editor_mgr->reimportAsset(path_str, nullptr);
        };
        auto reimport_row = std::make_shared<SHorizontalBox>();
        reimport_row->AddSlot(reimport).AutoSize();
        column->AddSlot(reimport_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 12.0f * scale, 0.0f, 0.0f));
    }

    auto scroll = std::make_shared<SScrollBox>();
    scroll->AddChild(column);

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
    border->Padding = ZSlate::FMargin(16.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;
    border->SetContent(scroll);
    m_Root = border;
}

void ZSlateInspectorWindow::BuildGenericAsset(const std::filesystem::path& asset_path, float scale)
{
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs
    m_NameLabel = nullptr;
    m_PositionFields = {};
    m_RotationFields = {};
    m_ScaleFields = {};

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(MakeText("Asset", 20.0f * scale, kHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale));

    AddFieldRow(column, "Name",
                MakeText(asset_path.stem().generic_string().c_str(), 14.0f * scale, kValueColor), 0, scale);
    AddFieldRow(column, "Path",
                MakeText(asset_path.generic_string().c_str(), 13.0f * scale, kDimColor), 0, scale);

    column
        ->AddSlot(MakeText("This .zasset file could not be resolved to a supported runtime asset type.",
                           13.0f * scale, kDimColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 8.0f * scale, 0.0f, 0.0f));

    auto scroll = std::make_shared<SScrollBox>();
    scroll->AddChild(column);

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
    border->Padding = ZSlate::FMargin(16.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;
    border->SetContent(scroll);
    m_Root = border;
}

bool ZSlateInspectorWindow::PickCombo(const std::string& label, const std::string& option, std::string& message)
{
    const auto it = g_combo_hooks.find(label);
    if (it == g_combo_hooks.end())
    {
        message = "no combo '" + label + "' on the current Inspector page (" + std::to_string(g_combo_hooks.size()) +
                  " combo(s): ";
        for (const auto& [name, hook] : g_combo_hooks)
        {
            message += "'" + name + "' ";
        }
        message += ")";
        return false;
    }
    const std::vector<std::string>& options = it->second.options;
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (options[i] == option)
        {
            // 先拷一份回调：on_select 会触发整页重建，重建会清空 g_combo_hooks
            const std::function<void(int)> select = it->second.on_select;
            select(static_cast<int>(i));
            message = "'" + label + "' <- '" + option + "' (option " + std::to_string(i) + "/" +
                      std::to_string(options.size()) + ")";
            return true;
        }
    }
    message = "'" + label + "' has no option '" + option + "'; options:";
    for (const std::string& o : options)
    {
        message += " '" + o + "'";
    }
    return false;
}

std::vector<std::string> ZSlateInspectorWindow::ListCombos()
{
    std::vector<std::string> out;
    for (const auto& [name, hook] : g_combo_hooks)
    {
        std::string line = name + ":";
        for (const std::string& o : hook.options)
        {
            line += " '" + o + "'";
        }
        out.push_back(std::move(line));
    }
    return out;
}

std::shared_ptr<SWidget> ZSlateInspectorWindow::MakeComboButton(const std::string& current_label,
                                                                std::vector<std::string> options,
                                                                std::function<void(int)> on_select,
                                                                float scale)
{
    auto btn = std::make_shared<SButton>();
    btn->Padding = ZSlate::FMargin(8.0f * scale, 3.0f * scale);
    btn->HAlign = EHorizontalAlignment::Left;
    btn->SetContent(MakeText((current_label + "   v").c_str(), 14.0f * scale, kValueColor));
    btn->OnClicked = [this, scale, options = std::move(options), on_select = std::move(on_select)]() {
        const ZSlate::Vector2 anchor = ZSlate::EditorSlateHost::Get().GetPointerPos();
        m_Popup.Open(anchor, scale, [this, options, on_select](SMenu& menu, float s) {
            menu.MinWidth = 150.0f * s;
            for (size_t i = 0; i < options.size(); ++i)
            {
                const int idx = static_cast<int>(i);
                menu.AddItem(
                    options[i],
                    [this, idx, on_select]() {
                        on_select(idx);
                        m_ForceRebuild = true;
                    },
                    s);
            }
        });
    };
    return btn;
}

void ZSlateInspectorWindow::BuildTextureAsset(const std::filesystem::path& asset_path, float scale)
{
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs
    m_NameLabel = nullptr;
    m_PositionFields = {};
    m_RotationFields = {};
    m_ScaleFields = {};

    auto wrap = [this, scale](const std::shared_ptr<SVerticalBox>& col) {
        auto scroll = std::make_shared<SScrollBox>();
        scroll->AddChild(col);
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
        border->Padding = ZSlate::FMargin(16.0f * scale);
        border->HAlign = EHorizontalAlignment::Fill;
        border->VAlign = EVerticalAlignment::Fill;
        border->SetContent(scroll);
        m_Root = border;
    };

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(MakeText("Texture Import Settings", 20.0f * scale, kHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale));

    auto editor_asset_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
    if (editor_asset_mgr == nullptr)
    {
        column->AddSlot(MakeText("Texture import settings unavailable (EditorAssetManager missing).",
                                 14.0f * scale, kDimColor))
            .AutoSize();
        column->AddSlot(MakeText("Use the Preview window to view this texture.", 13.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));
        wrap(column);
        return;
    }

    const std::filesystem::path zasset_abs = ResolveTexAbsolutePath(asset_path);
    if (s_tex_cached_path != zasset_abs)
    {
        s_tex_cached_path = zasset_abs;
        s_tex_settings = editor_asset_mgr->GetTextureImportSettingsRegistry().GetOrCreate(zasset_abs);
        s_tex_target = TextureImporterSettings::BuildTarget::Default;
        s_tex_dirty = false;
        s_tex_status.clear();
    }

    AddFieldRow(column, "Asset",
                MakeText(asset_path.stem().generic_string().c_str(), 14.0f * scale, kValueColor), 0, scale);
    AddFieldRow(column, "Path",
                MakeText(asset_path.generic_string().c_str(), 13.0f * scale, kDimColor), 0, scale);

    if (const auto source_entry = editor_asset_mgr->GetSourceAssetRegistry().Lookup(zasset_abs))
        AddFieldRow(column, "Source", MakeText(source_entry->source_path.c_str(), 13.0f * scale, kDimColor), 0, scale);
    else
        AddFieldRow(column, "Source",
                    MakeText("(unknown -- re-import once to enable Reimport)", 13.0f * scale, kDimColor), 0, scale);

    std::filesystem::path read_path = asset_path;
    if (auto* texture = GET_SYSTEM(AssetManager)->ReadObject<Texture2D>(read_path))
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%ux%u (format ord %u)", texture->m_Width, texture->m_Height,
                      texture->m_Format);
        AddFieldRow(column, "Imported", MakeText(buf, 13.0f * scale, kValueColor), 0, scale);
    }

    AddFieldRow(column, "Preview target",
                MakeText(TextureImporterSettings::BuildTargetDisplayName(
                             TextureImporterSettings::EditorPreviewBuildTarget()),
                         13.0f * scale, kDimColor),
                0, scale);

    // Platform selector.
    {
        const int platform_index = static_cast<int>(s_tex_target);
        std::vector<std::string> opts(std::begin(kTexPlatformLabels), std::end(kTexPlatformLabels));
        AddFieldRow(column, "Platform",
                    MakeComboButton(opts[static_cast<size_t>(platform_index)], opts,
                                    [](int idx) { s_tex_target = static_cast<TextureImporterSettings::BuildTarget>(idx); },
                                    scale),
                    0, scale);
    }

    const bool editing_default = (s_tex_target == TextureImporterSettings::BuildTarget::Default);
    bool override_enabled = editing_default ? false : s_tex_settings.HasOverride(s_tex_target);

    if (!editing_default)
    {
        auto ov = std::make_shared<SCheckBox>();
        ov->Checked = override_enabled;
        ov->BoxSize = 16.0f * scale;
        ov->OnCheckStateChanged = [this](bool checked) {
            if (checked)
            {
                TextureImporterSettings::PlatformSettings copy = s_tex_settings.default_settings;
                copy.overridden = true;
                s_tex_settings.SetOverride(s_tex_target, copy);
            }
            else
            {
                s_tex_settings.ClearOverride(s_tex_target);
            }
            s_tex_dirty = true;
            m_ForceRebuild = true;
        };
        AddFieldRow(column, "Override platform", ov, 0, scale);
    }
    else
    {
        column->AddSlot(MakeText("Default settings apply when a platform has no override.", 12.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 2.0f * scale, 0.0f, 6.0f * scale));
    }

    const bool fields_read_only = !editing_default && !override_enabled;
    TextureImporterSettings::PlatformSettings* editable =
        editing_default ? &s_tex_settings.default_settings
                        : (override_enabled ? &s_tex_settings.platform_overrides[s_tex_target]
                                            : &s_tex_settings.default_settings);

    if (fields_read_only)
    {
        // No override for this platform: show the effective values read-only.
        const TextureImporterSettings::PlatformSettings eff = s_tex_settings.GetEffective(s_tex_target);
        column->AddSlot(MakeText("Using Default settings:", 12.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 2.0f * scale, 0.0f, 4.0f * scale));
        AddFieldRow(column, "Max Size", MakeText(std::to_string(eff.max_size).c_str(), 14.0f * scale, kDimColor), 0,
                    scale);
        AddFieldRow(column, "Format",
                    MakeText(kTexFormatLabels[std::clamp(static_cast<int>(eff.format), 0,
                                                           static_cast<int>(std::size(kTexFormatLabels)) - 1)],
                             14.0f * scale, kDimColor),
                    0, scale);
        AddFieldRow(column, "sRGB", MakeText(eff.sRGB ? "true" : "false", 14.0f * scale, kDimColor), 0, scale);
        AddFieldRow(column, "Generate Mip Maps", MakeText(eff.generate_mipmaps ? "true" : "false", 14.0f * scale, kDimColor),
                    0, scale);
        AddFieldRow(column, "Compression Quality",
                    MakeText(std::to_string(eff.compression_quality).c_str(), 14.0f * scale, kDimColor), 0, scale);
    }
    else
    {
        // Max Size combo.
        {
            const int idx = TexMaxSizeToIndex(editable->max_size);
            std::vector<std::string> opts;
            opts.reserve(std::size(kTexMaxSizeChoices));
            for (int v : kTexMaxSizeChoices)
                opts.push_back(std::to_string(v));
            AddFieldRow(column, "Max Size",
                        MakeComboButton(opts[static_cast<size_t>(idx)], opts,
                                        [editable](int i) {
                                            editable->max_size = kTexMaxSizeChoices[i];
                                            s_tex_dirty = true;
                                        },
                                        scale),
                        0, scale);
        }

        // Format combo.
        {
            const int idx =
                std::clamp(static_cast<int>(editable->format), 0, static_cast<int>(std::size(kTexFormatLabels)) - 1);
            std::vector<std::string> opts(std::begin(kTexFormatLabels), std::end(kTexFormatLabels));
            AddFieldRow(column, "Format",
                        MakeComboButton(opts[static_cast<size_t>(idx)], opts,
                                        [editable](int i) {
                                            editable->format = static_cast<TextureImporterSettings::Format>(i);
                                            s_tex_dirty = true;
                                        },
                                        scale),
                        0, scale);
        }

        // sRGB toggle.
        {
            auto cb = std::make_shared<SCheckBox>();
            cb->Checked = editable->sRGB;
            cb->BoxSize = 16.0f * scale;
            cb->OnCheckStateChanged = [editable](bool checked) {
                editable->sRGB = checked;
                s_tex_dirty = true;
            };
            AddFieldRow(column, "sRGB (Color)", cb, 0, scale);
        }

        // Generate mip maps toggle (flips the hint text -> rebuild).
        {
            auto cb = std::make_shared<SCheckBox>();
            cb->Checked = editable->generate_mipmaps;
            cb->BoxSize = 16.0f * scale;
            cb->OnCheckStateChanged = [this, editable](bool checked) {
                editable->generate_mipmaps = checked;
                s_tex_dirty = true;
                m_ForceRebuild = true;
            };
            AddFieldRow(column, "Generate Mip Maps", cb, 0, scale);
        }

        // Compression quality slider (0..100); the live value text updates without
        // a rebuild because STextBlock::Text is sampled every paint.
        {
            auto value_text = MakeText(std::to_string(editable->compression_quality).c_str(), 13.0f * scale, kValueColor);
            auto slider = std::make_shared<SSlider>();
            slider->Value = std::clamp(editable->compression_quality, 0, 100) / 100.0f;
            slider->OnValueChanged = [editable, value_text](float v) {
                const int q = static_cast<int>(std::lround(v * 100.0f));
                editable->compression_quality = q;
                value_text->Text = std::to_string(q);
                s_tex_dirty = true;
            };
            auto quality_row = std::make_shared<SHorizontalBox>();
            quality_row->AddSlot(slider).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
            quality_row->AddSlot(value_text)
                .AutoSize()
                .SetVAlign(EVerticalAlignment::Center)
                .SetPadding(ZSlate::FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
            AddFieldRow(column, "Compression Quality", quality_row, 0, scale);
        }

        if (editable->format == TextureImporterSettings::Format::BC7 ||
            editable->format == TextureImporterSettings::Format::BC1 ||
            editable->format == TextureImporterSettings::Format::BC3 ||
            editable->format == TextureImporterSettings::Format::ASTC4x4 ||
            editable->format == TextureImporterSettings::Format::ASTC6x6 ||
            editable->format == TextureImporterSettings::Format::ASTC8x8)
        {
            const bool is_astc =
                editable->format == TextureImporterSettings::Format::ASTC4x4 ||
                editable->format == TextureImporterSettings::Format::ASTC6x6 ||
                editable->format == TextureImporterSettings::Format::ASTC8x8;
            const char* hint = is_astc
                                   ? "ASTC cooks for mobile; not sampleable in the DX12 editor preview."
                                   : "BC block compression is wired for import and cook.";
            column
                ->AddSlot(MakeText(hint, 12.0f * scale, kDimColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));
        }
        if (editable->generate_mipmaps)
        {
            column
                ->AddSlot(MakeText("Mip generation is saved but not applied on import yet.", 12.0f * scale, kDimColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));
        }
    }

    // Apply / Reimport actions.
    {
        const std::string zasset_str = zasset_abs.generic_string();
        auto apply = std::make_shared<SButton>();
        apply->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        apply->SetContent(MakeText("Apply", 14.0f * scale, kValueColor));
        apply->OnClicked = [this, zasset_abs]() {
            if (auto mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager)))
                mgr->GetTextureImportSettingsRegistry().Set(zasset_abs, s_tex_settings);
            s_tex_dirty = false;
            s_tex_status = "Import settings saved.";
            m_ForceRebuild = true;
        };

        auto reimport = std::make_shared<SButton>();
        reimport->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        reimport->SetContent(MakeText("Reimport", 14.0f * scale, kValueColor));
        reimport->OnClicked = [this, zasset_abs, zasset_str]() {
            auto mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
            if (mgr == nullptr)
                return;
            mgr->GetTextureImportSettingsRegistry().Set(zasset_abs, s_tex_settings);
            s_tex_dirty = false;
            if (mgr->reimportAsset(zasset_str, &s_tex_settings))
            {
                s_tex_status = "Reimport finished.";
            }
            else
            {
                s_tex_status = "Reimport failed (see log).";
            }
            m_ForceRebuild = true;
        };

        auto button_row = std::make_shared<SHorizontalBox>();
        button_row->AddSlot(apply).AutoSize();
        button_row->AddSlot(reimport).AutoSize().SetPadding(ZSlate::FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
        column->AddSlot(button_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 12.0f * scale, 0.0f, 0.0f));
    }

    if (!s_tex_status.empty())
        column->AddSlot(MakeText(s_tex_status.c_str(), 13.0f * scale, kValueColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
    if (s_tex_dirty)
        column->AddSlot(MakeText("Unsaved import setting changes.", 13.0f * scale, ZSlate::UIColor(1.0f, 0.85f, 0.2f, 1.0f)))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));

    // ASTC Preview button (Phase 6)
    {
        // Check if texture is ASTC-compressed
        bool is_astc = false;
        
        // Create a non-const copy for ReadObject (which takes std::filesystem::path&)
        std::filesystem::path read_path = asset_path;
        if (const auto tex = GET_SYSTEM(AssetManager)->ReadObject<Texture2D>(read_path))
        {
            // TODO: Check m_Format to confirm it's ASTC
            // For now, assume all textures can be previewed
            is_astc = true;  // TODO: Actually detect ASTC format
        }

        if (is_astc)
        {
            auto preview_btn = std::make_shared<SButton>();
            preview_btn->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
            preview_btn->SetContent(MakeText("Preview Texture", 14.0f * scale, kValueColor));
            preview_btn->OnClicked = [this, asset_path]() {
                // TODO: launch the standalone TexPreview tool
                // (engine/tools/TexPreview) on this asset path.
                // A TexPreviewWindow editor panel used to be stubbed out here;
                // it was never implemented (every method only logged) and has
                // been deleted -- do not re-add a second previewer, call the
                // existing tool instead.
            };

            column->AddSlot(preview_btn);
            // TODO: Apply AutoSize and SetPadding to the slot if needed
        }
    }

    wrap(column);
}

void ZSlateInspectorWindow::BuildMaterialAsset(const std::filesystem::path& asset_path, float scale)
{
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs

    Material& s_mat = MatRef();
    ShaderRes& s_mat_shader = MatShaderRef();

    auto wrap = [this, scale](const std::shared_ptr<SVerticalBox>& col) {
        auto scroll = std::make_shared<SScrollBox>();
        scroll->AddChild(col);
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
        border->Padding = ZSlate::FMargin(16.0f * scale);
        border->HAlign = EHorizontalAlignment::Fill;
        border->VAlign = EVerticalAlignment::Fill;
        border->SetContent(scroll);
        m_Root = border;
    };

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(MakeText("Material", 20.0f * scale, kHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale));

    // Reload material when the selected asset changes. Editing mutates s_mat in
    // place; the user commits with Apply (see button row below).
    if (s_mat_cached_path != asset_path)
    {
        s_mat_cached_path = asset_path;
        s_mat_loaded = false;
        s_mat_shader_loaded = false;
        s_mat_shader_name.clear();
        s_mat_shader_path.clear();
        s_mat_dirty = false;
        s_mat_status.clear();
        if (std::filesystem::exists(asset_path))
        {
            s_mat_loaded = LoadMaterialDefinitionForInspector(s_mat, asset_path);
            if (s_mat_loaded)
                SanitizeMaterialShaderBindingForInspector(s_mat);
        }
        else
        {
            SetInspectorLiveMaterialSource({}, nullptr);
        }
    }

    if (s_mat_loaded)
    {
        SetInspectorLiveMaterialSource(s_mat_cached_path, &s_mat);
    }
    else
    {
        SetInspectorLiveMaterialSource({}, nullptr);
    }

    AddFieldRow(column, "Name",
                MakeText(asset_path.stem().generic_string().c_str(), 14.0f * scale, kValueColor), 0, scale);
    AddFieldRow(column, "Path",
                MakeText(asset_path.generic_string().c_str(), 13.0f * scale, kDimColor), 0, scale);

    if (!s_mat_loaded)
    {
        column->AddSlot(MakeText("Unable to load material asset.", 14.0f * scale, kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 8.0f * scale, 0.0f, 0.0f));
        wrap(column);
        return;
    }

    // Resolve / refresh the bound project shader definition (drives the reflected
    // property list below). Reloaded whenever the authoring shader name changes.
    const eastl::string authoring_shader_name = GetMaterialAuthoringShaderName(s_mat);
    if (!s_mat_shader_loaded || s_mat_shader_name != authoring_shader_name)
    {
        s_mat_shader_name = authoring_shader_name;
        s_mat_shader_path.clear();
        s_mat_shader_loaded =
            LoadProjectShaderDefinitionForInspector(s_mat_shader, authoring_shader_name, &s_mat_shader_path);
        if (s_mat_shader_loaded)
        {
            EnsureShaderPassCompatibility(s_mat_shader);
            SyncLegacyShaderFieldsFromPrimaryPass(s_mat_shader);
        }
    }

    if (!IsShaderSourceAvailableInProject(authoring_shader_name) && !authoring_shader_name.empty() &&
        authoring_shader_name != "StandardLit")
    {
        column
            ->AddSlot(MakeText((std::string("Shader '") + authoring_shader_name.c_str() +
                                "' source not found. Select a valid shader below.")
                                   .c_str(),
                               13.0f * scale, ZSlate::UIColor(1.0f, 0.45f, 0.35f, 1.0f)))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 6.0f * scale));
    }

    // Shader selector (combo over StandardLit + all project shaders).
    {
        std::vector<std::string> shader_opts;
        shader_opts.push_back("StandardLit");
        for (const eastl::string& s : FindProjectShaders())
        {
            std::string name = s.c_str();
            if (std::find(shader_opts.begin(), shader_opts.end(), name) == shader_opts.end())
                shader_opts.push_back(name);
        }
        const std::string current_name = authoring_shader_name.empty() ? std::string("StandardLit")
                                                                        : std::string(authoring_shader_name.c_str());
        AddFieldRow(column, "Shader",
                    MakeComboButton(current_name, shader_opts,
                                    [shader_opts](int idx) {
                                        if (idx < 0 || idx >= static_cast<int>(shader_opts.size()))
                                            return;
                                        MatRef().SetShaderByName(shader_opts[static_cast<size_t>(idx)].c_str());
                                        s_mat_shader_loaded = false;  // force shader def reload on rebuild
                                        MarkMaterialEdited();
                                    },
                                    scale),
                    0, scale);
    }

    // Shader variant keyword toggles.
    {
        std::vector<std::string> keywords =
            CollectMaterialShaderVariantKeywords(authoring_shader_name, s_mat_shader_path);
        if (!keywords.empty())
        {
            auto variant_box = std::make_shared<SVerticalBox>();
            for (const std::string& kw : keywords)
            {
                auto row = std::make_shared<SHorizontalBox>();
                auto cb = std::make_shared<SCheckBox>();
                cb->Checked = s_mat.IsShaderKeywordEnabled(kw.c_str());
                cb->BoxSize = 16.0f * scale;
                const std::string kw_copy = kw;
                cb->OnCheckStateChanged = [this, kw_copy](bool checked) {
                    MatRef().SetShaderKeywordEnabled(kw_copy.c_str(), checked);
                    MarkMaterialEdited();
                    m_ForceRebuild = true;  // refresh the active-variant string
                };
                row->AddSlot(cb).AutoSize().SetVAlign(EVerticalAlignment::Center);
                row->AddSlot(MakeText(kw.c_str(), 13.0f * scale, kValueColor))
                    .AutoSize()
                    .SetVAlign(EVerticalAlignment::Center)
                    .SetPadding(ZSlate::FMargin(6.0f * scale, 0.0f, 0.0f, 0.0f));
                variant_box->AddSlot(row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 3.0f * scale));
            }
            if (!s_mat.m_EnabledShaderKeywords.empty())
            {
                variant_box
                    ->AddSlot(MakeText((std::string("Active variant: ") + s_mat.BuildShaderVariantKeyString().c_str())
                                           .c_str(),
                                       12.0f * scale, kDimColor))
                    .AutoSize()
                    .SetPadding(ZSlate::FMargin(0.0f, 2.0f * scale, 0.0f, 4.0f * scale));

                auto clear_btn = std::make_shared<SButton>();
                clear_btn->Padding = ZSlate::FMargin(10.0f * scale, 3.0f * scale);
                clear_btn->SetContent(MakeText("Clear All Keywords", 13.0f * scale, kValueColor));
                clear_btn->OnClicked = [this]() {
                    MatRef().ClearShaderKeywords();
                    MarkMaterialEdited();
                    m_ForceRebuild = true;
                };
                variant_box->AddSlot(clear_btn).AutoSize();
            }

            auto area = std::make_shared<SExpandableArea>();
            area->HeaderLabel = "Shader Variants";
            area->SetExpanded(true);
            area->FontSize = 15.0f * scale;
            area->HeaderHeight = 26.0f * scale;
            area->SetContent(variant_box);
            column->AddSlot(area).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 8.0f * scale, 0.0f, 4.0f * scale));
        }
    }

    // Property rows (reflected from the shader, or built-in PBR fallback).
    const bool has_custom_props = s_mat_shader_loaded && !s_mat_shader.m_Properties.empty();
    std::vector<MaterialInspectorRow> rows;
    if (has_custom_props)
    {
        bool created = false;
        rows = EnumerateShaderMaterialRows(s_mat, s_mat_shader, created);
        if (created)
            MarkMaterialEdited();  // materialised default properties -> user Apply
    }
    else
    {
        rows = EnumerateBuiltInMaterialRows(s_mat);
    }

    column->AddSlot(MakeText("Properties", 16.0f * scale, kLabelColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 10.0f * scale, 0.0f, 6.0f * scale));

    for (const MaterialInspectorRow& row : rows)
    {
        switch (row.kind)
        {
            case MaterialInspectorRowKind::Color:
            {
                Vector3* color_ptr = row.color;
                float* alpha_ptr = row.alpha;
                if (color_ptr == nullptr)
                    break;

                auto picker = std::make_shared<SColorPicker>();
                // SColorPicker sizes itself from ComputeDesiredSize() and its
                // R/G/B/A are the public color state -- the old ShowAlpha /
                // SquareSize / BarWidth / SetColorRGBA knobs are gone from ZSlate.
                picker->R = color_ptr->x;
                picker->G = color_ptr->y;
                picker->B = color_ptr->z;
                picker->A = alpha_ptr ? *alpha_ptr : 1.0f;
                picker->OnColorChanged = [color_ptr, alpha_ptr](float r, float g, float b, float a) {
                    color_ptr->x = r;
                    color_ptr->y = g;
                    color_ptr->z = b;
                    if (alpha_ptr != nullptr)
                        *alpha_ptr = a;
                    MarkMaterialEdited();
                };

                auto area = std::make_shared<SExpandableArea>();
                area->HeaderLabel = row.label;  // was Title
                area->SetExpanded(false);       // was `Expanded = bool`
                // ShowHeaderSwatch / HeaderSwatchColor no longer exist on
                // ZSlate's SExpandableArea; the header swatch is simply not drawn.
                area->FontSize = 14.0f * scale;
                area->HeaderHeight = 24.0f * scale;
                area->SetContent(picker);
                column->AddSlot(area).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));
                break;
            }
            case MaterialInspectorRowKind::Float:
            {
                float* value_ptr = row.value;
                if (value_ptr == nullptr)
                    break;
                auto df = std::make_shared<SDragFloat>();
                df->Value = *value_ptr;
                df->MinValue = row.range_min;
                df->MaxValue = row.range_max;
                const float span = row.range_max - row.range_min;
                df->Speed = (span > 0.0f) ? std::max(0.001f, span / 250.0f) : 0.01f;
                df->Height = 22.0f * scale;
                df->FontSize = 14.0f * scale;
                df->OnValueChanged = [value_ptr](float v) {
                    *value_ptr = v;
                    MarkMaterialEdited();
                };
                AddFieldRow(column, row.label, df, 0, scale);
                break;
            }
            case MaterialInspectorRowKind::Bool:
            {
                bool* bool_ptr = row.boolean;
                if (bool_ptr == nullptr)
                    break;
                auto cb = std::make_shared<SCheckBox>();
                cb->Checked = *bool_ptr;
                cb->BoxSize = 16.0f * scale;
                cb->OnCheckStateChanged = [bool_ptr](bool checked) {
                    *bool_ptr = checked;
                    MarkMaterialEdited();
                };
                AddFieldRow(column, row.label, cb, 0, scale);
                break;
            }
            case MaterialInspectorRowKind::Texture:
            {
                PPtr<Texture2D>* tex_ptr = row.texture;
                if (tex_ptr == nullptr)
                    break;
                auto box = std::make_shared<SEditableTextBox>();
                box->FontSize = 14.0f * scale;
                box->MinWidth = 120.0f * scale;
                box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
                box->Text = Material::ResolveTextureAssetPath(*tex_ptr).c_str();
                box->OnTextCommitted = [tex_ptr](const std::string& s) {
                    Material::AssignTextureFromAssetPath(*tex_ptr, s.c_str());
                    MarkMaterialEdited();
                };
                AddFieldRow(column, row.label, box, 0, scale);
                break;
            }
            case MaterialInspectorRowKind::String:
            {
                eastl::string* str_ptr = row.str;
                if (str_ptr == nullptr)
                    break;
                auto box = std::make_shared<SEditableTextBox>();
                box->FontSize = 14.0f * scale;
                box->MinWidth = 120.0f * scale;
                box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
                box->Text = str_ptr->c_str();
                box->OnTextCommitted = [str_ptr](const std::string& s) {
                    str_ptr->assign(s.c_str());
                    MarkMaterialEdited();
                };
                AddFieldRow(column, row.label, box, 0, scale);
                break;
            }
        }
    }

    {
        auto apply = std::make_shared<SButton>();
        apply->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        apply->SetContent(MakeText("Apply", 14.0f * scale, kValueColor));
        apply->OnClicked = [this]() {
            if (!s_mat_dirty)
            {
                s_mat_status = "No pending changes.";
                return;
            }
            if (SaveCachedMaterialToDisk())
            {
                s_mat_dirty = false;
                s_mat_status = "Material saved.";
            }
            else
            {
                s_mat_status = "Save failed (see log).";
            }
            m_ForceRebuild = true;
        };

        auto revert = std::make_shared<SButton>();
        revert->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        revert->SetContent(MakeText("Revert", 14.0f * scale, kValueColor));
        revert->OnClicked = [this]() {
            ReloadCachedMaterialFromDisk();
            s_mat_status = "Changes discarded.";
            m_ForceRebuild = true;
        };

        auto button_row = std::make_shared<SHorizontalBox>();
        button_row->AddSlot(apply).AutoSize();
        button_row->AddSlot(revert).AutoSize().SetPadding(ZSlate::FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
        column->AddSlot(button_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 12.0f * scale, 0.0f, 0.0f));
    }

    if (!s_mat_status.empty())
    {
        column->AddSlot(MakeText(s_mat_status.c_str(), 13.0f * scale, kValueColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
    }
    if (s_mat_dirty)
    {
        column->AddSlot(MakeText("Unsaved material changes.", 13.0f * scale, ZSlate::UIColor(1.0f, 0.85f, 0.2f, 1.0f)))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));
    }

    wrap(column);
}

void ZSlateInspectorWindow::BuildDataTableAsset(const std::filesystem::path& asset_path, const Type* asset_type,
                                                float scale)
{
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs

    auto wrap = [this, scale](const std::shared_ptr<SVerticalBox>& col) {
        auto scroll = std::make_shared<SScrollBox>();
        scroll->AddChild(col);
        auto border = std::make_shared<SBorder>();
        border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
        border->Padding = ZSlate::FMargin(16.0f * scale);
        border->HAlign = EHorizontalAlignment::Fill;
        border->VAlign = EVerticalAlignment::Fill;
        border->SetContent(scroll);
        m_Root = border;
    };

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(MakeText("Data Table", 20.0f * scale, kHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale));

    auto asset_manager = GET_SYSTEM(AssetManager);
    auto object_manager = GET_SYSTEM(ObjectManager);
    if (asset_manager == nullptr || object_manager == nullptr || asset_type == nullptr)
    {
        column->AddSlot(MakeText("DataTable inspector unavailable (systems missing).", 14.0f * scale, kDimColor))
            .AutoSize();
        wrap(column);
        return;
    }

    std::filesystem::path read_path = asset_path;
    Object* produced = asset_manager->ReadObject(read_path, asset_type);
    if (produced == nullptr)
    {
        AddFieldRow(column, "Path", MakeText(asset_path.generic_string().c_str(), 13.0f * scale, kDimColor), 0, scale);
        column->AddSlot(MakeText("Failed to load DataTable (the .zasset may be corrupt or stale).", 14.0f * scale,
                                 ZSlate::UIColor(1.0f, 0.45f, 0.35f, 1.0f)))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 8.0f * scale, 0.0f, 0.0f));
        column->AddSlot(MakeText("Edit the source CSV; the importer overwrites this file on save.", 13.0f * scale,
                                 kDimColor))
            .AutoSize()
            .SetPadding(ZSlate::FMargin(0.0f, 4.0f * scale, 0.0f, 0.0f));
        wrap(column);
        return;
    }

    auto* table = static_cast<DataTableBase*>(produced);
    table->onPostLoad();

    const std::string asset_name = Path::GetFilePureName(asset_path.filename().generic_string().c_str()).c_str();
    const std::string class_name = asset_type->GetName() ? asset_type->GetName() : "DataTable";
    const std::string source_csv = std::string(table->m_SourceCsvRelpath.c_str());
    const size_t row_count = table->rowCount();

    // Source extension detection (the member is historically m_SourceCsvRelpath
    // but XLSX paths are stored in it verbatim).
    std::string ext_lower;
    {
        const auto dot = source_csv.find_last_of('.');
        if (dot != std::string::npos)
        {
            ext_lower = source_csv.substr(dot);
            std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
    }
    const bool is_xlsx_source = (ext_lower == ".xlsx");
    const char* source_label = (ext_lower == ".xlsx") ? "Source XLSX" : (ext_lower == ".csv" ? "Source CSV" : "Source");

    AddFieldRow(column, "Name", MakeText(asset_name.c_str(), 14.0f * scale, kValueColor), 0, scale);
    AddFieldRow(column, "Class", MakeText(class_name.c_str(), 14.0f * scale, kValueColor), 0, scale);
    AddFieldRow(column, source_label, MakeText(source_csv.empty() ? "(unknown)" : source_csv.c_str(), 13.0f * scale, kDimColor),
                0, scale);
    AddFieldRow(column, "Path", MakeText(asset_path.generic_string().c_str(), 13.0f * scale, kDimColor), 0, scale);
    AddFieldRow(column, "Rows", MakeText(std::to_string(row_count).c_str(), 14.0f * scale, kValueColor), 0, scale);

    // Resolve absolute source path for reimport / open / parse.
    std::filesystem::path absolute_csv;
    if (!source_csv.empty())
    {
        if (auto project_info = GET_SYSTEM(ProjectInfo))
            absolute_csv = project_info->GetProjectRoot() / source_csv;
    }
    const bool csv_exists = !absolute_csv.empty() && std::filesystem::exists(absolute_csv);

    // Reimport / Open Source toolbar.
    {
        auto reimport = std::make_shared<SButton>();
        reimport->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        reimport->SetContent(MakeText("Reimport", 14.0f * scale, kValueColor));
        reimport->OnClicked = [this, absolute_csv, csv_exists, is_xlsx_source]() {
            if (!csv_exists)
            {
                LOG_WARNING(ZDataTable, "manual reimport: source {} not found",
                            absolute_csv.empty() ? std::string("<unknown>") : absolute_csv.generic_string());
                return;
            }
            const bool ok = is_xlsx_source ? XlsxImporter::CompileOne(absolute_csv)
                                           : DataTableImporter::CompileOne(absolute_csv);
            if (ok)
                LOG_INFO(ZDataTable, "manual reimport ok: {}", absolute_csv.generic_string());
            s_dt.asset_key.clear();  // force CSV matrix reload on next build
            m_ForceRebuild = true;
        };

        auto button_row = std::make_shared<SHorizontalBox>();
        button_row->AddSlot(reimport).AutoSize();

#ifdef _WIN32
        auto open_src = std::make_shared<SButton>();
        open_src->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
        open_src->SetContent(
            MakeText(is_xlsx_source ? "Open Source XLSX" : "Open Source CSV", 14.0f * scale, kValueColor));
        open_src->OnClicked = [absolute_csv, csv_exists]() {
            if (!csv_exists)
                return;
            const std::wstring wide = absolute_csv.wstring();
            ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        button_row->AddSlot(open_src).AutoSize().SetPadding(ZSlate::FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));
#endif
        column->AddSlot(button_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 10.0f * scale, 0.0f, 8.0f * scale));
    }

    // (Re)load the CSV edit-session when the selected asset changes. XLSX
    // sources stay non-editable (the binary container can't go through the CSV
    // text parser); they fall through to the read-only memory view.
    const std::string current_asset_key = asset_path.generic_string();
    if (s_dt.asset_key != current_asset_key)
    {
        s_dt = DTEditSession{};
        s_dt.asset_key = current_asset_key;
        s_dt.csv_path = absolute_csv;
        if (csv_exists && !is_xlsx_source)
        {
            eastl::string parse_err;
            if (DataTableImporter::ParseCsv(absolute_csv, s_dt.headers, s_dt.rows, parse_err))
                s_dt.loaded = true;
            else
                s_dt.last_error = parse_err;
        }
    }

    // Build the row-struct column descriptors (used for typed widgets in edit
    // mode and byte-offset access in read-only mode).
    TypeTree row_tree;
    table->fillRowTypeTree(row_tree);
    const std::vector<DTRowColumn> columns = DTCollectRowColumns(row_tree);

    // Filter box (applies on Enter).
    {
        auto filter_box = std::make_shared<SEditableTextBox>();
        filter_box->FontSize = 14.0f * scale;
        filter_box->MinWidth = 200.0f * scale;
        filter_box->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
        filter_box->Text = s_dt.filter;
        filter_box->OnTextCommitted = [this](const std::string& s) {
            s_dt.filter = s;
            m_ForceRebuild = true;
        };
        AddFieldRow(column, "Filter id", filter_box, 0, scale);
    }
    std::string filter_lower = s_dt.filter;
    std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const float cell_w = 130.0f * scale;

    if (s_dt.loaded)
    {
        // ----------------------------- EDIT MODE -----------------------------
        // Map CSV header -> row-struct column for typed widgets.
        std::vector<const DTRowColumn*> csv_to_struct(s_dt.headers.size(), nullptr);
        for (size_t hc = 0; hc < s_dt.headers.size(); ++hc)
        {
            const std::string h(s_dt.headers[hc].c_str());
            for (const auto& rc : columns)
            {
                if (rc.name == h)
                {
                    csv_to_struct[hc] = &rc;
                    break;
                }
            }
        }

        // Duplicate / blank id detection (blocks Save).
        std::unordered_map<std::string, int> id_freq;
        bool has_empty_id = false;
        for (const auto& r : s_dt.rows)
        {
            if (r.empty())
                continue;
            const std::string id_str(r[0].c_str());
            if (id_str.empty())
                has_empty_id = true;
            else
                id_freq[id_str] += 1;
        }
        std::vector<std::string> conflict_ids;
        for (const auto& kv : id_freq)
            if (kv.second > 1)
                conflict_ids.push_back(kv.first);
        std::sort(conflict_ids.begin(), conflict_ids.end());
        const bool has_conflict = has_empty_id || !conflict_ids.empty();
        const bool can_save = s_dt.dirty && !s_dt.headers.empty() && !has_conflict;

        // Save / Discard / Add Row toolbar.
        {
            auto toolbar = std::make_shared<SHorizontalBox>();

            auto save = std::make_shared<SButton>();
            save->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
            save->SetContent(MakeText("Save to CSV", 14.0f * scale, can_save ? kValueColor : kDimColor));
            const std::filesystem::path csv_path = absolute_csv;
            save->OnClicked = [this, csv_path, can_save]() {
                if (!can_save)
                    return;
                eastl::string write_err;
                if (DataTableImporter::WriteCsv(csv_path, s_dt.headers, s_dt.rows, write_err))
                {
                    s_dt.dirty = false;
                    s_dt.last_error.clear();
                    LOG_INFO(ZDataTable, "saved CSV {}; watcher will recompile shortly", csv_path.generic_string());
                }
                else
                {
                    s_dt.last_error = write_err;
                    LOG_WARNING(ZDataTable, "save CSV {} failed: {}", csv_path.generic_string(),
                                std::string(write_err.c_str()));
                }
                m_ForceRebuild = true;
            };
            toolbar->AddSlot(save).AutoSize();

            auto discard = std::make_shared<SButton>();
            discard->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
            discard->SetContent(MakeText("Discard", 14.0f * scale, s_dt.dirty ? kValueColor : kDimColor));
            const std::filesystem::path csv_path2 = absolute_csv;
            discard->OnClicked = [this, csv_path2]() {
                if (!s_dt.dirty)
                    return;
                eastl::string parse_err;
                eastl::vector<eastl::string> fresh_headers;
                eastl::vector<eastl::vector<eastl::string>> fresh_rows;
                if (DataTableImporter::ParseCsv(csv_path2, fresh_headers, fresh_rows, parse_err))
                {
                    s_dt.headers = std::move(fresh_headers);
                    s_dt.rows = std::move(fresh_rows);
                    s_dt.dirty = false;
                    s_dt.last_error.clear();
                }
                else
                {
                    s_dt.last_error = parse_err;
                }
                m_ForceRebuild = true;
            };
            toolbar->AddSlot(discard).AutoSize().SetPadding(ZSlate::FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));

            auto add_row = std::make_shared<SButton>();
            add_row->Padding = ZSlate::FMargin(10.0f * scale, 4.0f * scale);
            add_row->SetContent(MakeText("Add Row", 14.0f * scale, s_dt.headers.empty() ? kDimColor : kValueColor));
            add_row->OnClicked = [this]() {
                if (s_dt.headers.empty())
                    return;
                std::unordered_map<std::string, int> used;
                for (const auto& r : s_dt.rows)
                    if (!r.empty())
                        used[std::string(r[0].c_str())] += 1;
                std::string candidate = "new_row";
                int suffix = 1;
                while (used.count(candidate))
                    candidate = std::string("new_row_") + std::to_string(suffix++);
                eastl::vector<eastl::string> new_row;
                new_row.resize(s_dt.headers.size());
                new_row[0] = eastl::string(candidate.c_str());
                s_dt.rows.push_back(std::move(new_row));
                s_dt.dirty = true;
                m_ForceRebuild = true;
            };
            toolbar->AddSlot(add_row).AutoSize().SetPadding(ZSlate::FMargin(8.0f * scale, 0.0f, 0.0f, 0.0f));

            column->AddSlot(toolbar).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 8.0f * scale, 0.0f, 4.0f * scale));
        }

        if (s_dt.dirty)
            column->AddSlot(MakeText("(unsaved edits)", 13.0f * scale, ZSlate::UIColor(1.0f, 0.85f, 0.2f, 1.0f)))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));
        if (has_conflict)
            column
                ->AddSlot(MakeText("ID conflicts (duplicate or blank). Save is blocked until resolved.", 13.0f * scale,
                                   ZSlate::UIColor(1.0f, 0.35f, 0.35f, 1.0f)))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));
        if (!s_dt.last_error.empty())
            column->AddSlot(MakeText((std::string("Error: ") + s_dt.last_error.c_str()).c_str(), 13.0f * scale,
                                     ZSlate::UIColor(1.0f, 0.4f, 0.4f, 1.0f)))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));

        auto grid = std::make_shared<SVerticalBox>();

        // Header row.
        if (s_dt.headers.empty())
        {
            grid->AddSlot(MakeText("(CSV has no header row)", 13.0f * scale, kDimColor)).AutoSize();
        }
        else
        {
            auto header = std::make_shared<SHorizontalBox>();
            header->AddSlot(MakeText("#", 13.0f * scale, kLabelColor)).AutoSize().SetPadding(ZSlate::FMargin(0, 0, 6.0f * scale, 0));
            // reserve a small fixed gutter the width of the per-row delete button + index
            for (size_t hc = 0; hc < s_dt.headers.size(); ++hc)
            {
                std::string label = s_dt.headers[hc].c_str();
                const auto* rc = csv_to_struct[hc];
                if (rc != nullptr)
                    label += rc->is_supported ? ("  (" + rc->type_string + ")") : ("  <" + rc->type_string + ">");
                auto hb = MakeText(label.c_str(), 13.0f * scale, kLabelColor);
                header->AddSlot(hb).AutoSize().SetPadding(ZSlate::FMargin(0, 0, 8.0f * scale, 0));
            }
            grid->AddSlot(header).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

            for (size_t i = 0; i < s_dt.rows.size(); ++i)
            {
                auto& row = s_dt.rows[i];
                if (row.empty())
                    continue;
                if (!filter_lower.empty())
                {
                    std::string id_lower(row[0].c_str());
                    std::transform(id_lower.begin(), id_lower.end(), id_lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (id_lower.find(filter_lower) == std::string::npos)
                        continue;
                }

                auto row_box = std::make_shared<SHorizontalBox>();

                // Per-row delete button (replaces the legacy multi-select model).
                auto del = std::make_shared<SButton>();
                del->Padding = ZSlate::FMargin(5.0f * scale, 1.0f * scale);
                del->SetContent(MakeText("x", 13.0f * scale, ZSlate::UIColor(1.0f, 0.5f, 0.5f, 1.0f)));
                const size_t row_index = i;
                del->OnClicked = [this, row_index]() {
                    if (row_index < s_dt.rows.size())
                        s_dt.rows.erase(s_dt.rows.begin() + static_cast<std::ptrdiff_t>(row_index));
                    s_dt.dirty = true;
                    m_ForceRebuild = true;
                };
                row_box->AddSlot(del).AutoSize().SetPadding(ZSlate::FMargin(0, 0, 6.0f * scale, 0)).SetVAlign(EVerticalAlignment::Center);

                for (size_t c = 0; c < row.size() && c < s_dt.headers.size(); ++c)
                {
                    const auto* rc = csv_to_struct[c];
                    const bool is_bool = (rc != nullptr && rc->is_supported && rc->type_string == "bool");
                    const bool read_only = (rc != nullptr && !rc->is_supported);
                    eastl::string* cell = &row[c];

                    std::shared_ptr<SWidget> widget;
                    if (read_only)
                    {
                        widget = MakeText(cell->c_str(), 13.0f * scale, kDimColor);
                    }
                    else if (is_bool)
                    {
                        std::string lower(cell->c_str());
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        const bool v = !(lower.empty() || lower == "0" || lower == "false");
                        auto cb = std::make_shared<SCheckBox>();
                        cb->Checked = v;
                        cb->BoxSize = 16.0f * scale;
                        cb->OnCheckStateChanged = [this, cell](bool checked) {
                            cell->assign(checked ? "true" : "false");
                            s_dt.dirty = true;
                        };
                        widget = cb;
                    }
                    else
                    {
                        auto tb = std::make_shared<SEditableTextBox>();
                        tb->FontSize = 13.0f * scale;
                        tb->MinWidth = cell_w;
                        tb->Padding = ZSlate::FMargin(5.0f * scale, 2.0f * scale);
                        tb->Text = cell->c_str();
                        tb->OnTextCommitted = [this, cell](const std::string& s) {
                            cell->assign(s.c_str());
                            s_dt.dirty = true;
                        };
                        widget = tb;
                    }
                    row_box->AddSlot(widget).AutoSize().SetPadding(ZSlate::FMargin(0, 0, 8.0f * scale, 0)).SetVAlign(EVerticalAlignment::Center);
                }
                grid->AddSlot(row_box).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 3.0f * scale));
            }
            if (s_dt.rows.empty())
                grid->AddSlot(MakeText("(table is empty)", 13.0f * scale, kDimColor)).AutoSize();
        }

        column->AddSlot(grid).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
    }
    else
    {
        // --------------------------- READ-ONLY MODE --------------------------
        if (is_xlsx_source)
            column
                ->AddSlot(MakeText("XLSX source: read-only in inspector. Edit in Excel and save -- the watcher reimports.",
                                   13.0f * scale, ZSlate::UIColor(0.6f, 0.85f, 1.0f, 1.0f)))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
        else if (!s_dt.last_error.empty())
            column->AddSlot(MakeText((std::string("Source CSV unavailable for editing: ") + s_dt.last_error.c_str()).c_str(),
                                     13.0f * scale, ZSlate::UIColor(1.0f, 0.4f, 0.4f, 1.0f)))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));
        else if (!csv_exists)
            column->AddSlot(MakeText("Source CSV not found; rendering read-only memory view.", 13.0f * scale, kDimColor))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 4.0f * scale));

        const int row_size_bytes = static_cast<int>(table->rowSize());
        auto grid = std::make_shared<SVerticalBox>();
        if (columns.empty())
        {
            grid->AddSlot(MakeText("(row struct has no introspectable fields)", 13.0f * scale, kDimColor)).AutoSize();
        }
        else
        {
            auto header = std::make_shared<SHorizontalBox>();
            header->AddSlot(MakeText("#", 13.0f * scale, kLabelColor)).AutoSize().SetPadding(ZSlate::FMargin(0, 0, 8.0f * scale, 0));
            for (const auto& col : columns)
            {
                std::string label = col.name;
                label += col.is_supported ? ("  (" + col.type_string + ")") : ("  <" + col.type_string + ">");
                header->AddSlot(MakeText(label.c_str(), 13.0f * scale, kLabelColor))
                    .AutoSize()
                    .SetPadding(ZSlate::FMargin(0, 0, 8.0f * scale, 0));
            }
            grid->AddSlot(header).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

            for (size_t i = 0; i < row_count; ++i)
            {
                const eastl::string key = table->rowKeyAt(i);
                if (!filter_lower.empty())
                {
                    std::string key_lower(key.c_str());
                    std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (key_lower.find(filter_lower) == std::string::npos)
                        continue;
                }
                const auto* row_base = static_cast<const uint8_t*>(table->rowDataAt(i));
                if (row_base == nullptr)
                    continue;

                auto row_box = std::make_shared<SHorizontalBox>();
                row_box->AddSlot(MakeText(std::to_string(i).c_str(), 13.0f * scale, kDimColor))
                    .AutoSize()
                    .SetPadding(ZSlate::FMargin(0, 0, 8.0f * scale, 0));
                for (const auto& col : columns)
                {
                    std::string cell_text;
                    if (col.byte_size > 0 && static_cast<int>(col.byte_offset) + col.byte_size > row_size_bytes)
                        cell_text = "<oob>";
                    else if (!col.is_supported)
                        cell_text = "<" + col.type_string + ">";
                    else
                        cell_text = DTFormatCell(col, row_base + col.byte_offset);
                    row_box->AddSlot(MakeText(cell_text.c_str(), 13.0f * scale, kValueColor))
                        .AutoSize()
                        .SetPadding(ZSlate::FMargin(0, 0, 8.0f * scale, 0));
                }
                grid->AddSlot(row_box).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 3.0f * scale));
            }
            if (row_count == 0)
                grid->AddSlot(MakeText("(table is empty)", 13.0f * scale, kDimColor)).AutoSize();
        }
        column->AddSlot(grid).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 6.0f * scale, 0.0f, 0.0f));
    }

    // `produced` 来自 AssetManager::ReadObject，底层是 ObjectManager::Produce（`new T()`），
    // 所以这里必须走 ObjectManager::Destroy（= delete），不能用 MemoryManager::DestroyObject
    // （mi_free）：Apple 上全局 new/delete 不走 mimalloc，喂给 mi_free 会在
    // mi_usable_size → mi_checked_ptr_segment 里 EXC_BAD_ACCESS / SIGBUS。
    object_manager->Destroy(produced);
    wrap(column);
}

void ZSlateInspectorWindow::BuildShaderAsset(const std::filesystem::path& asset_path, float scale)
{
    m_Bindings.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs
    // The shader inspector widget tree is built inside InspectorShaderInspector's
    // TU (where the ShaderLab parse / source-IO helpers live). The request_rebuild
    // hook lets its action buttons (Refresh / Reimport / Compile / Save) force a
    // retained-tree rebuild so freshly-read state is reflected.
    m_Root = BuildShaderInspectorWidget(asset_path, scale, [this]() { m_ForceRebuild = true; });
}

void ZSlateInspectorWindow::BuildForObject(float scale)
{
    m_Bindings.clear();
    g_combo_hooks.clear();
    s_tooltips.clear();  // rows are rebuilt below; drop the stale widget refs

    auto make_field = [this, scale](int which, int axis) {
        auto f = std::make_shared<SEditableTextBox>();
        f->FontSize = 14.0f * scale;
        f->MinWidth = 44.0f * scale;
        f->Padding = ZSlate::FMargin(6.0f * scale, 3.0f * scale);
        f->OnTextCommitted = [this, which, axis](const std::string& s) {
            const float value = ParseFloat(s);
            auto go = EditorSelection::GetActiveGameObject().lock();
            if (!go)
                return;
            Transform* xf = go->tryGetComponent(Transform);
            if (xf == nullptr)
                return;
            EditorUndo::ScopedTransaction undo_scope(which == 0 ? "Set Position" : which == 1 ? "Set Scale" : "Set Rotation");
            EditorUndo::RecordTransform(xf);
            RecordPrefabOverridesForUndo();
            if (which == 0)
            {
                Vector3 p = xf->GetLocalPosition();
                p[static_cast<size_t>(axis)] = value;
                xf->SetLocalPosition(p);
            }
            else if (which == 1)
            {
                Vector3 sc = xf->GetLocalScale();
                sc[static_cast<size_t>(axis)] = value;
                xf->SetLocalScale(sc);
            }
            else
            {
                Vector3 degrees;
                for (int a = 0; a < 3; ++a)
                    degrees[static_cast<size_t>(a)] =
                        m_RotationFields[static_cast<size_t>(a)] ? ParseFloat(m_RotationFields[static_cast<size_t>(a)]->Text) : 0.0f;
                const Quaternion rot = EditorEuler::MakeQuaternionFromEulerDegrees(degrees);
                xf->SetLocalRotation(rot);
                EditorEuler::SetEulerHint(xf, degrees, rot);
            }
            // m_LocalPositionD is the authoritative copy (m_LocalPosition is the float
            // legacy one the writer derives from it).
            SyncPrefabOverride(xf, which == 0 ? "m_LocalPositionD" : which == 1 ? "m_LocalScale" : "m_LocalRotation");
            if (auto scene = GET_SYSTEM(EditorSceneManager))
                scene->DrawSelectedEntityAxis();
        };
        return f;
    };

    auto vec_row = [&](const char* label, std::array<std::shared_ptr<SEditableTextBox>, 3>& fields, int which) {
        auto row = std::make_shared<SHorizontalBox>();
        row->AddSlot(MakeLabelBox(label, scale, 78.0f * scale)).AutoSize().SetVAlign(EVerticalAlignment::Center);
        const char* axis_names[3] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a)
        {
            row->AddSlot(MakeText(axis_names[a], 13.0f * scale, kDimColor))
                .AutoSize()
                .SetVAlign(EVerticalAlignment::Center)
                .SetPadding(ZSlate::FMargin(6.0f * scale, 0.0f, 2.0f * scale, 0.0f));
            fields[static_cast<size_t>(a)] = make_field(which, a);
            row->AddSlot(fields[static_cast<size_t>(a)]).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);
        }
        return row;
    };

    m_ComponentEnableBindings.clear();
    m_GoActiveCheck = std::make_shared<SCheckBox>();
    m_GoActiveCheck->BoxSize = 18.0f * scale;
    m_GoActiveCheck->OnCheckStateChanged = [](bool active) {
        auto go = EditorSelection::GetActiveGameObject().lock();
        if (!go)
            return;
        Transform* xf = go->tryGetComponent(Transform);
        if (xf == nullptr)
            return;
        EditorUndo::ScopedTransaction undo_scope(active ? "Activate GameObject" : "Deactivate GameObject");
        EditorUndo::RecordTransform(xf);
        // 走 GameObject::SetActive 而不是 xf->SetEnabled：后者只写自己那一个 flag，
        // 子树里的 renderer 收不到通知，父级开关一变它们就留在旧状态上（停用后仍
        // 在画面里）。SetActive 会连整棵子树一起重推。
        go->SetActive(active);
        xf->OnSerializedFieldsUpdated();
        NotifyAllComponentsUpdated(go.get());
    };

    auto name_row = std::make_shared<SHorizontalBox>();
    name_row->AddSlot(m_GoActiveCheck)
        .AutoSize()
        .SetVAlign(EVerticalAlignment::Center)
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 8.0f * scale, 0.0f));
    name_row->AddSlot(MakeLabelBox("Name", scale, 70.0f * scale)).AutoSize().SetVAlign(EVerticalAlignment::Center);
    m_NameLabel = MakeText("", 15.0f * scale, kValueColor);
    name_row->AddSlot(m_NameLabel).Fill(1.0f).SetVAlign(EVerticalAlignment::Center);

    auto column = std::make_shared<SVerticalBox>();
    column->AddSlot(MakeText("Inspector (ZSlate)", 20.0f * scale, kHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 10.0f * scale));
    column->AddSlot(name_row).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 12.0f * scale));

    // Prefab instance: source, Apply All / Revert All, one row per override.
    m_BuiltPrefabSection = false;
    m_BuiltPrefabOverrideCount = 0;
    if (PrefabInstance* instance = SelectedPrefabInstance())
    {
        m_BuiltPrefabSection = true;
        const std::vector<PrefabOverrideModel::OverrideRow> rows = PrefabOverrideModel::ListOverrides(instance);
        m_BuiltPrefabOverrideCount = rows.size();

        std::string source_label = "<unsaved prefab>";
        if (Object* source = static_cast<Object*>(instance->GetSourcePrefab()))
        {
            std::filesystem::path source_path;
            int64_t file_id = 0;
            if (AssetManager* am = GET_SYSTEM(AssetManager);
                am != nullptr && am->TryGetIdentityForInstance(source->GetInstanceID(), source_path, file_id))
                source_label = source_path.filename().generic_string();
        }

        auto make_button = [scale](const char* text, std::function<void()> on_click) {
            auto button = std::make_shared<SButton>();
            button->Padding = ZSlate::FMargin(8.0f * scale, 3.0f * scale);
            button->SetContent(MakeText(text, 13.0f * scale, kValueColor));
            button->OnClicked = std::move(on_click);
            return button;
        };

        auto header = std::make_shared<SHorizontalBox>();
        header->AddSlot(MakeText(("Prefab  " + source_label).c_str(), 15.0f * scale, kOverrideColor))
            .Fill(1.0f)
            .SetVAlign(EVerticalAlignment::Center);
        header->AddSlot(MakeText((std::to_string(rows.size()) + (rows.size() == 1 ? " override" : " overrides")).c_str(),
                                 13.0f * scale, kDimColor))
            .AutoSize()
            .SetVAlign(EVerticalAlignment::Center)
            .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 8.0f * scale, 0.0f));
        if (!rows.empty())
        {
            header->AddSlot(make_button("Revert All", [this]() {
                       PrefabInstance* inst = SelectedPrefabInstance();
                       if (inst == nullptr)
                           return;
                       EditorUndo::ScopedTransaction undo_scope("Revert Prefab Overrides");
                       RecordOverrideTargetsForUndo(inst, nullptr);
                       RecordPrefabOverridesForUndo();
                       RecordPrefabStructureForUndo();  // Revert All also detaches / re-introduces components
                       PrefabUtilityEditor::Revert(inst);
                       m_ForceRebuild = true;
                   }))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 6.0f * scale, 0.0f));
            // Apply writes the source prefab file and refreshes the level's other
            // instances; like Unity, that part is not undoable. Hidden when nothing
            // can be applied (a plain prefab whose only overrides are component rows
            // — it has no structural Apply).
            const bool any_applicable = std::any_of(rows.begin(), rows.end(),
                                                    [](const PrefabOverrideModel::OverrideRow& r) { return r.can_apply; });
            if (any_applicable)
            {
                header->AddSlot(make_button("Apply All", [this]() {
                           if (PrefabInstance* inst = SelectedPrefabInstance())
                               PrefabOverrideModel::ApplyAll(ActiveLevel(), inst);
                           m_ForceRebuild = true;
                       }))
                    .AutoSize();
            }
        }
        column->AddSlot(header).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 4.0f * scale));

        for (const PrefabOverrideModel::OverrideRow& row : rows)
        {
            using Kind = PrefabOverrideModel::OverrideRow::Kind;
            const PPtr<Object> target = row.target;
            const eastl::string path = row.property_path;
            std::string text = row.label.c_str();
            if (row.kind == Kind::AddedComponent)
                text += "  (added component)";
            else if (row.kind == Kind::RemovedComponent)
                text += "  (removed component)";
            else
                text += std::string("  ") + path.c_str();

            auto line = std::make_shared<SHorizontalBox>();
            line->AddSlot(MakeText(text.c_str(), 13.0f * scale, kLabelColor))
                .Fill(1.0f)
                .SetVAlign(EVerticalAlignment::Center)
                .SetPadding(ZSlate::FMargin(12.0f * scale, 0.0f, 0.0f, 0.0f));

            if (row.kind != Kind::Property)
            {
                // Component rows: Revert only per row (Apply All commits them on a
                // Variant). Revert is undoable via the structure record below.
                line->AddSlot(make_button("Revert", [this, row]() {
                         if (PrefabInstance* inst = SelectedPrefabInstance())
                         {
                             // Undoable: the component list, the added / removed lists and the
                             // source map are restored together by one structure record.
                             EditorUndo::ScopedTransaction undo_scope("Revert Prefab Component");
                             RecordPrefabStructureForUndo();
                             PrefabOverrideModel::RevertStructuralRow(inst, row);
                         }
                         m_ForceRebuild = true;
                     }))
                    .AutoSize();
                column->AddSlot(line).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));
                continue;
            }

            line->AddSlot(make_button("Revert", [this, target, path]() {
                     PrefabInstance* inst = SelectedPrefabInstance();
                     Object* obj = static_cast<Object*>(target);
                     if (inst == nullptr || obj == nullptr)
                         return;
                     EditorUndo::ScopedTransaction undo_scope("Revert Prefab Override");
                     RecordOverrideTargetsForUndo(inst, obj);
                     RecordPrefabOverridesForUndo();
                     PrefabOverrideModel::RevertField(inst, obj, path);
                     m_ForceRebuild = true;
                 }))
                .AutoSize()
                .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 6.0f * scale, 0.0f));
            line->AddSlot(make_button("Apply", [this, target, path]() {
                     PrefabInstance* inst = SelectedPrefabInstance();
                     Object* obj = static_cast<Object*>(target);
                     if (inst != nullptr && obj != nullptr)
                         PrefabOverrideModel::ApplyField(ActiveLevel(), inst, obj, path);
                     m_ForceRebuild = true;
                 }))
                .AutoSize();
            column->AddSlot(line).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 2.0f * scale));
        }
        column->AddSlot(MakeText("", 6.0f * scale, kDimColor)).AutoSize();
    }

    column->AddSlot(MakeText("Transform", 16.0f * scale, kComponentHeaderColor))
        .AutoSize()
        .SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 6.0f * scale));
    column->AddSlot(vec_row("Position", m_PositionFields, 0)).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 5.0f * scale));
    column->AddSlot(vec_row("Rotation", m_RotationFields, 2)).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 5.0f * scale));
    column->AddSlot(vec_row("Scale", m_ScaleFields, 1)).AutoSize().SetPadding(ZSlate::FMargin(0.0f, 0.0f, 0.0f, 5.0f * scale));

    // Generic sections for every other component.
    if (auto go = EditorSelection::GetActiveGameObject().lock())
    {
        Transform* xf = go->tryGetComponent(Transform);
        auto comps = go->getComponents();
        for (size_t i = 0; i < comps.size(); ++i)
        {
            Component* c = comps[i];
            if (c == nullptr || c == xf)
                continue;
            SectionServices services;
            services.make_combo = [this, scale](const std::string& current, std::vector<std::string> options,
                                                std::function<void(int)> on_select) {
                return MakeComboButton(current, std::move(options), std::move(on_select), scale);
            };
            services.request_rebuild = [this]() { m_ForceRebuild = true; };
            BuildComponentSection(column, c, i, m_Bindings, m_ComponentEnableBindings, scale, services);
        }
    }

    auto scroll = std::make_shared<SScrollBox>();
    scroll->AddChild(column);

    auto border = std::make_shared<SBorder>();
    border->BackgroundColor = ZSlate::UIColor(0.10f, 0.10f, 0.13f, 1.0f);
    border->Padding = ZSlate::FMargin(16.0f * scale);
    border->HAlign = EHorizontalAlignment::Fill;
    border->VAlign = EVerticalAlignment::Fill;
    border->SetContent(scroll);
    m_Root = border;
}

void ZSlateInspectorWindow::SyncFromSelection()
{
    auto go = EditorSelection::GetActiveGameObject().lock();
    if (!go)
        return;

    const bool object_active = IsGameObjectActive(go);

    if (m_GoActiveCheck)
        m_GoActiveCheck->Checked = object_active;

    if (m_NameLabel)
    {
        m_NameLabel->Text = std::string(go->GetName().c_str());
        m_NameLabel->Color = object_active ? kValueColor : kDimColor;
    }

    Transform* xf = go->tryGetComponent(Transform);
    if (xf == nullptr)
        return;

    const Vector3 p = xf->GetLocalPosition();
    const Vector3 sc = xf->GetLocalScale();
    const Vector3 euler = EditorEuler::GetEulerHint(xf, xf->GetLocalRotation());
    for (int a = 0; a < 3; ++a)
    {
        const size_t i = static_cast<size_t>(a);
        if (m_PositionFields[i] && !m_PositionFields[i]->IsFocused())
            m_PositionFields[i]->Text = FormatFloat(p[i]);
        if (m_RotationFields[i] && !m_RotationFields[i]->IsFocused())
            m_RotationFields[i]->Text = FormatFloat(euler[i]);
        if (m_ScaleFields[i] && !m_ScaleFields[i]->IsFocused())
            m_ScaleFields[i]->Text = FormatFloat(sc[i]);
    }
}

void ZSlateInspectorWindow::SyncBindings()
{
    auto go = EditorSelection::GetActiveGameObject().lock();
    const bool object_active = IsGameObjectActive(go);

    for (ComponentEnableBinding& enable_binding : m_ComponentEnableBindings)
    {
        Component* c = ResolveSelectedComponent(enable_binding.component_index);
        if (c == nullptr)
            continue;
        if (enable_binding.check)
            enable_binding.check->Checked = c->isEnabled();
        if (enable_binding.title)
        {
            const bool show_enabled = object_active && c->isEnabled();
            enable_binding.title->Color = show_enabled ? kComponentHeaderColor : kDimColor;
        }
    }

    PrefabInstance* prefab_instance = SelectedPrefabInstance();
    const size_t override_count = prefab_instance != nullptr ? prefab_instance->GetModifications().size() : 0;
    if ((prefab_instance != nullptr) != m_BuiltPrefabSection || override_count != m_BuiltPrefabOverrideCount)
        m_ForceRebuild = true;  // the override list changed (edit / undo / apply elsewhere)

    for (FieldBinding& b : m_Bindings)
    {
        if (b.custom_sync)
        {
            b.custom_sync();
            continue;
        }

        Component* c = ResolveSelectedComponent(b.component_index);
        if (c == nullptr)
            continue;
        void* a = reinterpret_cast<char*>(c) + b.byte_offset;

        // meta=(EditCondition="[!]flag"): dim the row label and let the commit
        // lambdas refuse writes while the condition is closed. An overridden field of
        // a prefab instance is drawn in the override colour (Unity's blue label).
        if (b.row_label)
        {
            if (b.gate.active && !IsGateOpen(b.gate, c))
                b.row_label->Color = kDimColor;
            else if (prefab_instance != nullptr && !b.prefab_path.empty() &&
                     PrefabOverrideModel::IsOverridden(prefab_instance, c, eastl::string(b.prefab_path.c_str())))
                b.row_label->Color = kOverrideColor;
            else
                b.row_label->Color = kLabelColor;
        }

        if (b.read_only && b.label)
        {
            b.label->Text = FormatScalar(b.kind, a);
            continue;
        }

        switch (b.kind)
        {
            case FieldKind::Bool:
                if (b.check)
                    b.check->Checked = *static_cast<const bool*>(a);
                break;
            case FieldKind::Vec3:
            {
                const Vector3* v = static_cast<const Vector3*>(a);
                for (int axis = 0; axis < 3; ++axis)
                {
                    auto& box = b.boxes[static_cast<size_t>(axis)];
                    if (box && !box->IsFocused())
                        box->Text = FormatFloat((*v)[static_cast<size_t>(axis)]);
                }
                break;
            }
            default:
                if (b.boxes[0] && !b.boxes[0]->IsFocused())
                    b.boxes[0]->Text = FormatScalar(b.kind, a);
                break;
        }
    }
}

void ZSlateInspectorWindow::OnGUI()
{
    // P10c: process-wide measurer installed by ZSlateEditorOverlay::BeginFrameIfEnabled.

    float ui_scale = ZSlate::EditorSlateHost::Get().GetUiScale();
    if (ui_scale < 0.5f)
        ui_scale = 1.0f;

    // Asset selection takes priority (matches the legacy InspectorWindow dispatch
    // order). Every asset inspector (Font, Texture, Shader, Material, DataTable,
    // generic .zasset) now builds a ZSlate widget tree painted through the canvas
    // path below -- there is no remaining immediate-mode ImGui drawer fall-through.
    NativeAssetKind native_kind = NativeAssetKind::None;
    std::filesystem::path asset_path = GET_SYSTEM(EditorSceneManager)->getSelectedAssetPath();
    if (!asset_path.empty())
    {
        const std::string selected_type = GET_SYSTEM(EditorSceneManager)->getSelectedAssetType();
        const std::string resolved = ResolveInspectorAssetType(asset_path, selected_type);
        if (resolved == "font")
        {
            native_kind = NativeAssetKind::Font;  // built below, painted via the ZSlate canvas
        }
        else if (resolved == "shader")
        {
            native_kind = NativeAssetKind::Shader;  // built below, painted via the ZSlate canvas
        }
        else if (IsTexture2DInspectorAssetType(resolved))
        {
            // Texture2D `.zasset`: the native inspector shows import settings; the actual
            // image is viewed in the Preview window. (The old inline bindless ImGui preview
            // was removed -- it had no remaining call site.)
            native_kind = NativeAssetKind::Texture;  // built below, painted via the ZSlate canvas
        }
        else if (resolved == "material")
        {
            native_kind = NativeAssetKind::Material;  // built below, painted via the ZSlate canvas
        }
        else if (const Type* dt_type = ResolveDataTableType(asset_path))
        {
            native_kind = NativeAssetKind::DataTable;  // built below, painted via the ZSlate canvas
            m_DataTableType = dt_type;
        }
        else if (IsGenericInspectorZAssetType(asset_path, resolved))
        {
            native_kind = NativeAssetKind::Generic;  // built below, painted via the ZSlate canvas
        }
    }

    const bool native_asset = (native_kind != NativeAssetKind::None);

    GObjectID object_id = k_invalid_gobject_id;
    size_t component_count = 0;
    std::shared_ptr<GameObject> go;
    bool has_object = false;
    if (!native_asset)
    {
        go = EditorSelection::GetActiveGameObject().lock();
        has_object = (go != nullptr);
        object_id = EditorSelection::GetActiveGameObjectId();
        component_count = has_object ? go->getComponents().size() : 0;
    }

    const std::string asset_key = native_asset ? asset_path.generic_string() : std::string();
    const bool scale_changed = (m_Root == nullptr) || (ui_scale != m_BuiltScale);
    const bool selection_changed = m_ForceRebuild || (native_kind != m_BuiltNativeKind) ||
                                   (asset_key != m_BuiltAssetPath) || (has_object != m_BuiltHasObject) ||
                                   (object_id != m_BuiltObjectId) || (component_count != m_BuiltComponentCount);
    if (scale_changed || selection_changed)
    {
        m_ForceRebuild = false;
        if (native_kind == NativeAssetKind::Font)
            BuildFontAsset(asset_path, ui_scale);
        else if (native_kind == NativeAssetKind::Texture)
            BuildTextureAsset(asset_path, ui_scale);
        else if (native_kind == NativeAssetKind::Generic)
            BuildGenericAsset(asset_path, ui_scale);
        else if (native_kind == NativeAssetKind::Material)
            BuildMaterialAsset(asset_path, ui_scale);
        else if (native_kind == NativeAssetKind::DataTable)
            BuildDataTableAsset(asset_path, m_DataTableType, ui_scale);
        else if (native_kind == NativeAssetKind::Shader)
            BuildShaderAsset(asset_path, ui_scale);
        else if (has_object)
            BuildForObject(ui_scale);
        else
            BuildEmpty(ui_scale);
        m_BuiltScale = ui_scale;
        m_BuiltObjectId = object_id;
        m_BuiltHasObject = has_object;
        m_BuiltComponentCount = component_count;
        m_BuiltNativeAsset = native_asset;
        m_BuiltNativeKind = native_kind;
        m_BuiltAssetPath = asset_key;
        m_Input.Reset();
    }

    if (!native_asset)
    {
        SyncFromSelection();
        SyncBindings();
    }

    // Native dock hosting is unconditional: ReconcileNativeTreeWithOpenWindows (run
    // before any panel OnGUI) guarantees an open window is in the dock tree, so the
    // leaf rect always comes from NativeRect().
    const float* native_rect = NativeRect();
    ZSlate::Vector2 pos(native_rect[0], native_rect[1]);
    ZSlate::Vector2 avail(native_rect[2], native_rect[3]);
    if (avail.x < 1.0f)
        avail.x = 1.0f;
    if (avail.y < 1.0f)
        avail.y = 1.0f;

    const ZSlate::UIRect region(pos.x, pos.y, avail.x, avail.y);
    const FGeometry geometry(ZSlate::Vector2(pos.x, pos.y), ZSlate::Vector2(avail.x, avail.y));

    // P9: paint the widget tree into the shared BatchedUIRenderer (frame begun/ended by
    // ZSlateEditorOverlay around WindowUI::PreRender) with this panel's rect as a clip.
    // The legacy per-window SlateImGuiRenderer fallback was retired.
    auto& overlay = ZSlate::ZSlateEditorOverlay::Get();
    {
        BatchedUIRenderer& renderer = overlay.GetRenderer();
        overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZPanel);

        m_Root->CacheDesiredSize();

        FPaintContext ctx;
        ctx.Renderer = &overlay;  // ZSlateEditorOverlay implements ISlateRenderer
        ctx.LayerId = 0;

        renderer.PushClipRect(region, true);
        m_Root->Paint(ctx, geometry);
        renderer.PopClipRect();

        // ZPROPERTY(meta=(Tooltip="..")): floating label for the hovered row,
        // painted in the foreground layer so it can overflow the panel body.
        if (!m_HoverTooltip.empty() && !m_Popup.IsOpen())
        {
            ZSlate::STooltip tip;
            tip.Text = m_HoverTooltip;
            tip.FontSize = 12.0f * ui_scale;
            const ZSlate::Vector2 size = tip.ComputeDesiredSize();
            ZSlate::Vector2 at(m_HoverTooltipPos.x + 16.0f * ui_scale, m_HoverTooltipPos.y + 20.0f * ui_scale);
            if (at.x + size.x > region.x + region.w)
                at.x = region.x + region.w - size.x;
            if (at.y + size.y > region.y + region.h)
                at.y = region.y + region.h - size.y;
            overlay.BeginWindowGroup(ZSlate::ZSlateEditorOverlay::kZForeground);
            tip.OnPaint(ctx, FGeometry(at, size));
        }
    }

    // P11a: input / hover / wheel / keyboard all come from the GLFW-backed
    // EditorSlateHost (the transitional r.ZSlate.NativeInput CVar was retired in
    // P10c, so the old ImGui::GetIO() fallback branches were dead and are gone).
    ZSlate::EditorSlateHost& host = ZSlate::EditorSlateHost::Get();
    const int surface_id = ZSlate::EditorSlateHost::HashId(m_Title);
    host.BeginSurface(surface_id, region, ZSlate::ESurfaceLayer::Panels);
    const ZSlate::Vector2 mouse = host.GetPointerPos();
    const bool over_canvas = host.IsSurfaceHovered(surface_id, mouse);
    const bool left_down = host.IsLeftDown();
    const float wheel = over_canvas ? host.GetWheelDelta() : 0.0f;

    if (m_Popup.IsOpen())
    {
        // A combo dropdown (native asset inspector) is open: paint + route it as a
        // foreground overlay so it can overflow the panel, and swallow tree input
        // this frame to avoid click-through. Mirrors ZSlateHierarchyWindow.
        // Clamp to the native display rect (== the editor's full-window viewport).
        const ZSlate::UIRect viewport_rect(host.GetDisplayPos().x, host.GetDisplayPos().y,
                                   host.GetDisplaySize().x, host.GetDisplaySize().y);

        {
            m_Popup.Render(overlay, mouse, left_down, wheel, viewport_rect, 1);
        }
    }
    else
    {
        m_Input.ProcessMouse(m_Root, mouse, over_canvas, left_down, wheel);

        if (m_Input.HasKeyboardFocus())
        {
            host.NotifyNativeTextInputActive();
            for (unsigned int cp : host.GetCharsThisFrame())
                m_Input.ProcessChar(cp);
            // UE pattern: route ALL keys to the focused widget
            for (EKey key : host.GetKeysThisFrame())
                m_Input.ProcessKey(key);
        }
    }

    // Resolve the hover tooltip for the NEXT frame's paint (the tree is painted
    // before input is routed, so the label always trails the pointer by a frame).
    if (over_canvas && !m_Popup.IsOpen())
    {
        if (const std::string* tip = FindTooltipAt(mouse))
        {
            m_HoverTooltip = *tip;
            m_HoverTooltipPos = mouse;
        }
        else
        {
            m_HoverTooltip.clear();
        }
    }
    else
    {
        m_HoverTooltip.clear();
    }
}
