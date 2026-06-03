#pragma once

#include <filesystem>

class Type;

const Type* ResolveDataTableType(const std::filesystem::path& asset_path);
