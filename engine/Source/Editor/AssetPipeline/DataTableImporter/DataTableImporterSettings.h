#pragma once

#include "Editor/AssetPipeline/AssetImporterSettings.h"

// =============================================================================
// DataTableImporter has no V1 import knobs -- all behaviour is determined
// by the registered schema (REGISTER_DATA_TABLE) and the source CSV
// content. We still derive a settings type so AssetImportManager's settings
// plumbing stays uniform across importers; future knobs (e.g. UTF-8 vs UTF-16
// auto-detect, custom delimiter, ignore-row-prefix for comment markers) plug
// in here without touching the importer itself.
// =============================================================================
class DataTableImporterSettings : public AssetImporterSettings
{
public:
    ~DataTableImporterSettings() override = default;
};
