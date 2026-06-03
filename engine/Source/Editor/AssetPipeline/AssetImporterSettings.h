#pragma once

#include "Runtime/BaseClasses/Object.h"

class AssetImporterSettings
{
public:
    virtual ~AssetImporterSettings() = default;

    bool generate_guid = true;
    std::string custom_guid;
};