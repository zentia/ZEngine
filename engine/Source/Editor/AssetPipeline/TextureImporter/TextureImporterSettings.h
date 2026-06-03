#pragma once

#include "Editor/AssetPipeline/AssetImporterSettings.h"

#include <cstdint>
#include <map>
#include <string>

// Unity-style texture import settings. Default fields apply to all targets;
// per-platform entries override when `overridden` is true.
class TextureImporterSettings : public AssetImporterSettings
{
public:
    enum class Format
    {
        RGBA8,
        RGB8,
        RGBA16F,
        BC7,
        BC1,
        BC3
    };

    // Mirrors Unity's platform tabs (subset we can cook toward today).
    enum class BuildTarget
    {
        Default = 0,
        Standalone,
        Android,
        iOS,
        WebGL,
        Count
    };

    struct PlatformSettings
    {
        bool overridden = false;
        Format format = Format::RGBA8;
        bool generate_mipmaps = true;
        bool sRGB = true;
        int max_size = 4096;
        // 0 = fastest/lowest, 100 = best quality (reserved for BC encoders).
        int compression_quality = 50;
    };

    PlatformSettings default_settings;
    std::map<BuildTarget, PlatformSettings> platform_overrides;

    TextureImporterSettings() = default;

    static BuildTarget EditorPreviewBuildTarget();
    static const char* BuildTargetDisplayName(BuildTarget target);
    static const char* FormatDisplayName(Format format);
    static BuildTarget BuildTargetFromName(const char* name);
    static Format FormatFromName(const char* name);
    static int ClampMaxSize(int value);
    static bool IsPowerOfTwoSize(int value);

    // Effective settings for a build target (Default returns default_settings).
    PlatformSettings GetEffective(BuildTarget target) const;

    void SetOverride(BuildTarget target, const PlatformSettings& settings);
    void ClearOverride(BuildTarget target);
    bool HasOverride(BuildTarget target) const;

    // Deep copy for inspector editing sessions.
    static TextureImporterSettings Clone(const TextureImporterSettings& other);

    bool LoadFromJsonString(const std::string& json);
    std::string SaveToJsonString() const;
};
