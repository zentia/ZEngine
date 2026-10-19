#pragma once

#include "Editor/AssetPipeline/AssetImporterSettings.h"

// Unity-style animation import settings (PR-SK2).
class AnimationImporterSettings : public AssetImporterSettings
{
public:
    /// Keyframes per second used when baking FBX curves into clip channels.
    /// 0 = use the frame rate stored in the file (falls back to 30).
    float sample_rate = 0.0f;

    /// Hard cap on the number of baked frames per clip. Guards against a
    /// 10-minute animation exploding the asset size; 0 means "no cap beyond the
    /// internal safety limit".
    int max_frames = 0;
};
