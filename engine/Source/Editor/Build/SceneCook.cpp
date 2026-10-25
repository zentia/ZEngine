#include "Editor/Build/SceneCook.h"

#include "Runtime/BaseClasses/Type.h"
#include "Runtime/BaseClasses/TypeManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/YamlSerialize/YamlObjectGraph.h"
#include "Runtime/Resource/Asset/AssetManager.h"

#include <EASTL/string.h>

#include <fstream>
#include <iterator>
#include <sstream>

namespace
{
    bool ReadText(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    bool WriteBytes(const std::filesystem::path& path, const char* data, size_t size)
    {
        std::error_code ec;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(data, static_cast<std::streamsize>(size));
        out.flush();
        return static_cast<bool>(out);
    }
}  // namespace

bool SceneCook::IsEditorOnlyTypeName(const char* type_name)
{
    if (type_name == nullptr)
        return false;
    const Type* type = TypeManager::GetInstance().ClassNameToType(type_name);
    return type != nullptr && type->IsEditorOnly();
}

SceneCookResult SceneCook::CookSceneFile(const std::filesystem::path& source, const std::filesystem::path& cooked)
{
    SceneCookResult result;
    std::string text;
    if (!ReadText(source, text))
    {
        result.error = "cannot read " + source.generic_string();
        return result;
    }

    if (!AssetManager::IsYamlAuthoringAssetPath(source))
    {
        result.ok = WriteBytes(cooked, text.data(), text.size());
        if (!result.ok)
            result.error = "cannot write " + cooked.generic_string();
        else
            LOG_WARNING(ZPak, "SceneCook: {} is not a YAML scene; copied unchanged (editor-only objects not removed)",
                        source.generic_string());
        return result;
    }

    eastl::string stripped;
    ZYaml::StripGraphStats stats;
    if (!ZYaml::StripObjectsFromGraph(text.c_str(), &SceneCook::IsEditorOnlyTypeName, stripped, &stats))
    {
        result.error = "not a YAML object graph: " + source.generic_string();
        return result;
    }
    if (!WriteBytes(cooked, stripped.data(), stripped.size()))
    {
        result.error = "cannot write " + cooked.generic_string();
        return result;
    }

    result.ok = true;
    result.stripped = true;
    result.removed_objects = stats.removedObjects;
    result.removed_externals = stats.removedExternals;
    result.dangling_references = stats.danglingReferences;
    if (stats.danglingReferences > 0)
    {
        LOG_WARNING(ZPak, "SceneCook: {}: {} reference(s) from player objects to editor-only objects were nulled",
                    source.generic_string(), stats.danglingReferences);
    }
    return result;
}
