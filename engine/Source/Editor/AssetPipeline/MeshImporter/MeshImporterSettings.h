#pragma once

#include "Editor/AssetPipeline/AssetImporterSettings.h"

// Unity-style model import settings (V1: geometry only).
class MeshImporterSettings : public AssetImporterSettings
{
public:
    // Uniform scale applied to vertex positions (FBX unit / cm -> m, etc.).
    float scale = 1.0f;

    // When true, flip the Y axis after import (common for DCC -> engine).
    bool flip_y = false;
};
