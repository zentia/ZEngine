#include "PlayerSettings.h"

#include "Runtime/Core/Base/Macro.h"
#include "core/Log/LogSystem.h"
#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

#include <fstream>

namespace
{
    // JSON keys. Kept as plain literals rather than a generated schema: this
    // file is hand-editable and read by players built from older sources.
    constexpr const char* kKeyProductName = "product_name";
    constexpr const char* kKeyCompanyName = "company_name";
    constexpr const char* kKeyVersion = "version";
    constexpr const char* kKeyScreenWidth = "default_screen_width";
    constexpr const char* kKeyScreenHeight = "default_screen_height";

    constexpr const char* kSettingsFileName = "player_settings.json";

    constexpr const char* kDefaultCompanyName = "ZEngine";
    constexpr const char* kDefaultVersion = "1.0.0";
    constexpr int kDefaultScreenWidth = 1280;
    constexpr int kDefaultScreenHeight = 720;
    /// A resolution of 0 or less would reach the platform window code and fail
    /// somewhere far away from here, so it is rejected at the file boundary.
    constexpr int kMinScreenDimension = 1;

    const char* ReadString(const rapidjson::Value& object, const char* key)
    {
        const auto member = object.FindMember(key);
        if (member == object.MemberEnd() || !member->value.IsString())
        {
            return nullptr;
        }
        return member->value.GetString();
    }
}  // namespace

void PlayerSettings::ApplyProjectDefaults(const std::string& project_name, const std::string& project_version)
{
    // Not persisted: it IS the project name.
    m_ProjectName = project_name;
    // The important default. Before this was filled in, UserPreferences derived
    // its directory from an empty product name and every project on the machine
    // shared the "ZEngine" folder.
    m_ProductName = project_name;
    m_CompanyName = kDefaultCompanyName;
    m_Version = project_version.empty() ? kDefaultVersion : project_version;
    m_DefaultScreenWidth = kDefaultScreenWidth;
    m_DefaultScreenHeight = kDefaultScreenHeight;
}

std::filesystem::path PlayerSettings::GetSettingsFilePath() const
{
    const ProjectInfo* project = GET_SYSTEM(ProjectInfo);
    if (project == nullptr)
    {
        return {};
    }

    const std::filesystem::path root = project->GetProjectRoot();
    if (root.empty())
    {
        return {};
    }

    // ProjectInfo declares config_dir but nothing read it yet; this is the first
    // consumer, and it is the right one -- a project-level, checked-in config
    // directory, as opposed to saved/ which holds per-machine state.
    const std::string config = project->config_dir.empty() ? std::string("config") : project->config_dir;
    return root / config / kSettingsFileName;
}

bool PlayerSettings::LoadFromFile(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open())
    {
        return false;
    }

    rapidjson::Document document;
    rapidjson::IStreamWrapper stream(file);
    document.ParseStream(stream);
    if (document.HasParseError() || !document.IsObject())
    {
        LOG_WARNING(ZProjectInfo, "PlayerSettings: '{}' is not a valid JSON object; using defaults",
                    path.generic_string());
        return false;
    }

    // Absent or ill-typed keys keep the value they already have, so a file
    // written by an older build still loads and a partial file is legal.
    if (const char* value = ReadString(document, kKeyProductName))
    {
        m_ProductName = value;
    }
    if (const char* value = ReadString(document, kKeyCompanyName))
    {
        m_CompanyName = value;
    }
    if (const char* value = ReadString(document, kKeyVersion))
    {
        m_Version = value;
    }

    const auto width = document.FindMember(kKeyScreenWidth);
    if (width != document.MemberEnd() && width->value.IsInt() && width->value.GetInt() >= kMinScreenDimension)
    {
        m_DefaultScreenWidth = width->value.GetInt();
    }
    const auto height = document.FindMember(kKeyScreenHeight);
    if (height != document.MemberEnd() && height->value.IsInt() && height->value.GetInt() >= kMinScreenDimension)
    {
        m_DefaultScreenHeight = height->value.GetInt();
    }

    return true;
}

bool PlayerSettings::Load()
{
    return LoadFromFile(GetSettingsFilePath());
}

bool PlayerSettings::Save(const std::filesystem::path& path) const
{
    if (path.empty())
    {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    rapidjson::Document document;
    document.SetObject();
    auto& allocator = document.GetAllocator();

    auto add_string = [&allocator, &document](const char* key, const std::string& value) {
        rapidjson::Value name(key, allocator);
        rapidjson::Value stored(value.c_str(), allocator);
        document.AddMember(name, stored, allocator);
    };
    add_string(kKeyProductName, m_ProductName);
    add_string(kKeyCompanyName, m_CompanyName);
    add_string(kKeyVersion, m_Version);
    document.AddMember(rapidjson::Value(kKeyScreenWidth, allocator),
                       rapidjson::Value(m_DefaultScreenWidth),
                       allocator);
    document.AddMember(rapidjson::Value(kKeyScreenHeight, allocator),
                       rapidjson::Value(m_DefaultScreenHeight),
                       allocator);

    // Atomic: write a sibling and rename. A crash or a full disk mid-write must
    // not leave a truncated settings file behind -- that would silently reset
    // the product name on the next launch.
    const std::filesystem::path temp_path = path.string() + ".tmp";
    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            LOG_WARNING(ZProjectInfo, "PlayerSettings: cannot write {}", temp_path.generic_string());
            return false;
        }

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
        file << buffer.GetString();
    }

    std::filesystem::rename(temp_path, path, ec);
    if (ec)
    {
        // rename() does not replace an existing file on Windows, and can fail
        // across volumes; fall back to a copy.
        std::filesystem::copy_file(temp_path, path, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(temp_path, ec);
    }
    return !ec;
}

bool PlayerSettings::Save() const
{
    return Save(GetSettingsFilePath());
}

bool PlayerSettings::Initialize()
{
    const ProjectInfo* project = GET_SYSTEM(ProjectInfo);
    if (project == nullptr)
    {
        // No project: keep the header defaults. A player launched without one
        // is a supported state (standalone tools, hand-rolled smoke tests), and
        // failing here would take the whole engine down.
        LOG_WARNING(ZProjectInfo, "PlayerSettings: no project is open; using built-in defaults");
        return true;
    }

    ApplyProjectDefaults(project->name.c_str(), project->version);

    // A missing file is the normal case and is not an error: every project that
    // has never opened a settings UI gets the project defaults.
    Load();
    return true;
}
