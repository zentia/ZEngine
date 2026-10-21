// =============================================================================
// PlayerSettings smoke test.
//
// Headless: no window, no GPU, no project. It covers the part of PlayerSettings
// that used to be the actual bug -- `m_ProductName` was never filled in, so
// UserPreferences derived its per-user directory from an empty string and every
// project on the machine shared the "ZEngine" preferences folder.
//
//   cmake -B build -DZENGINE_BUILD_PLAYER_SETTINGS_SMOKE_TEST=ON
//   cmake --build build --target PlayerSettingsSmokeTest
//   ./build/.../PlayerSettingsSmokeTest
// =============================================================================

#include "Runtime/Function/PlayerSettings/PlayerSettings.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", name);
        }
        else
        {
            std::printf("[ ok ] %s\n", name);
        }
    }

    bool WriteTextFile(const std::filesystem::path& path, const std::string& contents)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return false;
        }
        out << contents;
        return true;
    }
}  // namespace

int main()
{
    // Unbuffered: a crash must not swallow the checks that already passed.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "zplayer_settings_smoke";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    // ---- P1: defaults come from the project ------------------------------
    PlayerSettings settings;
    settings.ApplyProjectDefaults("My Game", "2.3.1");
    Check(settings.m_ProjectName == "My Game", "P1: project name comes from the project");
    Check(settings.m_ProductName == "My Game", "P1: product name defaults to the project name");
    Check(settings.m_CompanyName == "ZEngine", "P1: company name has a default");
    Check(settings.m_Version == "2.3.1", "P1: version comes from the project");

    PlayerSettings no_version;
    no_version.ApplyProjectDefaults("Untitled", "");
    Check(no_version.m_Version == "1.0.0", "P1: an empty project version falls back to 1.0.0");

    // ---- P2: no project registered -> no settings path -------------------
    // A player (or a standalone tool) launched without a project is a supported
    // state, so this must be "no file", not a crash.
    Check(settings.GetSettingsFilePath().empty(), "P2: no project means no settings file path");
    Check(!settings.Load(), "P2: Load() without a project is a no-op, not a failure");

    // ---- P3: a missing file keeps the defaults ---------------------------
    PlayerSettings fresh;
    fresh.ApplyProjectDefaults("My Game", "2.3.1");
    Check(!fresh.LoadFromFile(root / "does-not-exist.json"), "P3: a missing file reports false");
    Check(fresh.m_ProductName == "My Game" && fresh.m_DefaultScreenWidth == 1280 &&
              fresh.m_DefaultScreenHeight == 720,
          "P3: the defaults survive a missing file");
    Check(!fresh.LoadFromFile(std::filesystem::path()), "P3: an empty path is refused");

    // ---- P4: malformed JSON keeps the defaults ---------------------------
    const std::filesystem::path broken_path = root / "broken.json";
    Check(WriteTextFile(broken_path, "{ this is not json"), "P4: broken fixture written");
    Check(!fresh.LoadFromFile(broken_path), "P4: malformed JSON reports false");
    Check(fresh.m_ProductName == "My Game" && fresh.m_DefaultScreenWidth == 1280,
          "P4: the defaults survive malformed JSON");

    // ---- P5: a real file overrides ---------------------------------------
    const std::filesystem::path settings_path = root / "player_settings.json";
    Check(WriteTextFile(settings_path,
                        "{\n"
                        "  \"product_name\": \"Shipped Name\",\n"
                        "  \"company_name\": \"Acme\",\n"
                        "  \"version\": \"3.0.0\",\n"
                        "  \"default_screen_width\": 1920,\n"
                        "  \"default_screen_height\": 1080\n"
                        "}\n"),
          "P5: settings fixture written");
    Check(fresh.LoadFromFile(settings_path), "P5: the settings file loads");
    Check(fresh.m_ProductName == "Shipped Name", "P5: product name is overridden");
    Check(fresh.m_CompanyName == "Acme", "P5: company name is overridden");
    Check(fresh.m_Version == "3.0.0", "P5: version is overridden");
    Check(fresh.m_DefaultScreenWidth == 1920 && fresh.m_DefaultScreenHeight == 1080,
          "P5: the default resolution is overridden");
    Check(fresh.m_ProjectName == "My Game", "P5: the project name is not taken from the file");

    // ---- P6: a partial file only overrides what it names -----------------
    const std::filesystem::path partial_path = root / "partial.json";
    Check(WriteTextFile(partial_path, "{ \"version\": \"9.9.9\" }"), "P6: partial fixture written");
    PlayerSettings partial;
    partial.ApplyProjectDefaults("My Game", "2.3.1");
    Check(partial.LoadFromFile(partial_path), "P6: a partial file loads");
    Check(partial.m_Version == "9.9.9", "P6: the named key is applied");
    Check(partial.m_ProductName == "My Game" && partial.m_DefaultScreenWidth == 1280,
          "P6: the unnamed keys keep their defaults");

    // ---- P7: an unusable resolution is rejected at the file boundary -----
    // A width of 0 would otherwise reach the platform window code and fail
    // somewhere far away from here.
    const std::filesystem::path bad_size_path = root / "bad_size.json";
    Check(WriteTextFile(bad_size_path,
                        "{ \"default_screen_width\": 0, \"default_screen_height\": -5 }"),
          "P7: bad-resolution fixture written");
    PlayerSettings bad_size;
    bad_size.ApplyProjectDefaults("My Game", "2.3.1");
    Check(bad_size.LoadFromFile(bad_size_path), "P7: the file still loads");
    Check(bad_size.m_DefaultScreenWidth == 1280 && bad_size.m_DefaultScreenHeight == 720,
          "P7: a zero / negative resolution falls back to the default");

    // ---- P8: save round-trips --------------------------------------------
    const std::filesystem::path saved_path = root / "saved" / "player_settings.json";
    Check(fresh.Save(saved_path), "P8: the settings file is written");
    PlayerSettings reloaded;
    reloaded.ApplyProjectDefaults("Other", "0.0.1");
    Check(reloaded.LoadFromFile(saved_path), "P8: the written file loads back");
    Check(reloaded.m_ProductName == fresh.m_ProductName &&
              reloaded.m_CompanyName == fresh.m_CompanyName && reloaded.m_Version == fresh.m_Version &&
              reloaded.m_DefaultScreenWidth == fresh.m_DefaultScreenWidth &&
              reloaded.m_DefaultScreenHeight == fresh.m_DefaultScreenHeight,
          "P8: every field round-trips");
    Check(!std::filesystem::exists(saved_path.string() + ".tmp", ec), "P8: no temp file is left behind");

    // Save() into a directory that has to be created, and to an empty path.
    Check(!fresh.Save(std::filesystem::path()), "P8: saving to an empty path is refused");

    std::filesystem::remove_all(root, ec);

    std::printf("\nPlayerSettings smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
