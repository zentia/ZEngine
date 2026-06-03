#include "TextureImporterSettings.h"

#include "Runtime/Core/Base/Platform.h"

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <algorithm>
#include <cmath>

namespace
{
    const char* kFormatNames[] = {"RGBA8", "RGB8", "RGBA16F", "BC7", "BC1", "BC3", "ASTC4x4", "ASTC6x6", "ASTC8x8"};

    const char* kTargetNames[] = {"Default", "Standalone", "Android", "iOS", "OHOS", "WebGL"};

    void ReadPlatformObject(const rapidjson::Value& obj, TextureImporterSettings::PlatformSettings& out)
    {
        if (obj.HasMember("overridden") && obj["overridden"].IsBool())
        {
            out.overridden = obj["overridden"].GetBool();
        }
        if (obj.HasMember("format") && obj["format"].IsString())
        {
            out.format = TextureImporterSettings::FormatFromName(obj["format"].GetString());
        }
        if (obj.HasMember("generate_mipmaps") && obj["generate_mipmaps"].IsBool())
        {
            out.generate_mipmaps = obj["generate_mipmaps"].GetBool();
        }
        if (obj.HasMember("sRGB") && obj["sRGB"].IsBool())
        {
            out.sRGB = obj["sRGB"].GetBool();
        }
        if (obj.HasMember("max_size") && obj["max_size"].IsInt())
        {
            out.max_size = TextureImporterSettings::ClampMaxSize(obj["max_size"].GetInt());
        }
        if (obj.HasMember("compression_quality") && obj["compression_quality"].IsInt())
        {
            out.compression_quality = std::clamp(obj["compression_quality"].GetInt(), 0, 100);
        }
    }

    void WritePlatformObject(rapidjson::Value& obj,
                             rapidjson::Document::AllocatorType& alloc,
                             const TextureImporterSettings::PlatformSettings& settings)
    {
        obj.AddMember("overridden", settings.overridden, alloc);
        obj.AddMember("format", rapidjson::Value(TextureImporterSettings::FormatDisplayName(settings.format), alloc), alloc);
        obj.AddMember("generate_mipmaps", settings.generate_mipmaps, alloc);
        obj.AddMember("sRGB", settings.sRGB, alloc);
        obj.AddMember("max_size", settings.max_size, alloc);
        obj.AddMember("compression_quality", settings.compression_quality, alloc);
    }
}  // namespace

TextureImporterSettings::BuildTarget TextureImporterSettings::EditorPreviewBuildTarget()
{
#if defined(__ANDROID__)
    return BuildTarget::Android;
#elif defined(Z_PLATFORM_OHOS) || defined(__OHOS__)
    return BuildTarget::OHOS;
#elif defined(__EMSCRIPTEN__)
    return BuildTarget::WebGL;
#else
    return BuildTarget::Standalone;
#endif
}

const char* TextureImporterSettings::BuildTargetDisplayName(BuildTarget target)
{
    const int idx = static_cast<int>(target);
    if (idx >= 0 && idx < static_cast<int>(BuildTarget::Count))
    {
        return kTargetNames[idx];
    }
    return "Unknown";
}

const char* TextureImporterSettings::FormatDisplayName(Format format)
{
    const int idx = static_cast<int>(format);
    if (idx >= 0 && idx <= static_cast<int>(Format::ASTC8x8))
    {
        return kFormatNames[idx];
    }
    return "RGBA8";
}

TextureImporterSettings::BuildTarget TextureImporterSettings::BuildTargetFromName(const char* name)
{
    if (name == nullptr)
    {
        return BuildTarget::Default;
    }
    for (int i = 0; i < static_cast<int>(BuildTarget::Count); ++i)
    {
        if (std::string(name) == kTargetNames[i])
        {
            return static_cast<BuildTarget>(i);
        }
    }
    // Aliases for cook console / scripts (canonical JSON key is "OHOS").
    const std::string lower = [&]() {
        std::string s(name);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    if (lower == "harmonyos" || lower == "openharmony" || lower == "harmony")
    {
        return BuildTarget::OHOS;
    }
    return BuildTarget::Default;
}

TextureImporterSettings::Format TextureImporterSettings::FormatFromName(const char* name)
{
    if (name == nullptr)
    {
        return Format::RGBA8;
    }
    for (int i = 0; i <= static_cast<int>(Format::ASTC8x8); ++i)
    {
        if (std::string(name) == kFormatNames[i])
        {
            return static_cast<Format>(i);
        }
    }
    if (name != nullptr)
    {
        std::string lower(name);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "astc" || lower == "astc_4x4")
        {
            return Format::ASTC4x4;
        }
        if (lower == "astc_6x6")
        {
            return Format::ASTC6x6;
        }
        if (lower == "astc_8x8")
        {
            return Format::ASTC8x8;
        }
    }
    return Format::RGBA8;
}

int TextureImporterSettings::ClampMaxSize(int value)
{
    static constexpr int kSizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    int best = kSizes[0];
    for (int size : kSizes)
    {
        if (value >= size)
        {
            best = size;
        }
    }
    return best;
}

bool TextureImporterSettings::IsPowerOfTwoSize(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

TextureImporterSettings::PlatformSettings TextureImporterSettings::GetEffective(BuildTarget target) const
{
    if (target == BuildTarget::Default)
    {
        return default_settings;
    }
    const auto it = platform_overrides.find(target);
    if (it != platform_overrides.end() && it->second.overridden)
    {
        return it->second;
    }
    return default_settings;
}

void TextureImporterSettings::SetOverride(BuildTarget target, const PlatformSettings& settings)
{
    if (target == BuildTarget::Default)
    {
        default_settings = settings;
        default_settings.overridden = false;
        return;
    }
    PlatformSettings copy = settings;
    copy.overridden = true;
    platform_overrides[target] = copy;
}

void TextureImporterSettings::ClearOverride(BuildTarget target)
{
    if (target == BuildTarget::Default)
    {
        return;
    }
    platform_overrides.erase(target);
}

bool TextureImporterSettings::HasOverride(BuildTarget target) const
{
    if (target == BuildTarget::Default)
    {
        return false;
    }
    const auto it = platform_overrides.find(target);
    return it != platform_overrides.end() && it->second.overridden;
}

TextureImporterSettings TextureImporterSettings::Clone(const TextureImporterSettings& other)
{
    return other;
}

bool TextureImporterSettings::LoadFromJsonString(const std::string& json)
{
    if (json.empty())
    {
        return false;
    }
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsObject())
    {
        return false;
    }

    if (doc.HasMember("default") && doc["default"].IsObject())
    {
        ReadPlatformObject(doc["default"], default_settings);
        default_settings.overridden = false;
    }

    platform_overrides.clear();
    if (doc.HasMember("overrides") && doc["overrides"].IsObject())
    {
        const auto& overrides = doc["overrides"];
        for (auto it = overrides.MemberBegin(); it != overrides.MemberEnd(); ++it)
        {
            if (!it->name.IsString() || !it->value.IsObject())
            {
                continue;
            }
            const BuildTarget target = BuildTargetFromName(it->name.GetString());
            if (target == BuildTarget::Default || target == BuildTarget::Count)
            {
                continue;
            }
            PlatformSettings platform {};
            ReadPlatformObject(it->value, platform);
            if (platform.overridden)
            {
                platform_overrides[target] = platform;
            }
        }
    }

    if (doc.HasMember("generate_guid"))
    {
        generate_guid = doc["generate_guid"].GetBool();
    }
    if (doc.HasMember("custom_guid") && doc["custom_guid"].IsString())
    {
        custom_guid = doc["custom_guid"].GetString();
    }
    return true;
}

std::string TextureImporterSettings::SaveToJsonString() const
{
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    rapidjson::Value default_obj(rapidjson::kObjectType);
    WritePlatformObject(default_obj, alloc, default_settings);
    doc.AddMember("default", default_obj, alloc);

    rapidjson::Value overrides_obj(rapidjson::kObjectType);
    for (const auto& [target, platform] : platform_overrides)
    {
        if (!platform.overridden || target == BuildTarget::Default)
        {
            continue;
        }
        rapidjson::Value platform_obj(rapidjson::kObjectType);
        WritePlatformObject(platform_obj, alloc, platform);
        overrides_obj.AddMember(
            rapidjson::Value(BuildTargetDisplayName(target), alloc), platform_obj, alloc);
    }
    doc.AddMember("overrides", overrides_obj, alloc);
    doc.AddMember("generate_guid", generate_guid, alloc);
    doc.AddMember("custom_guid", rapidjson::Value(custom_guid.c_str(), alloc), alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}
