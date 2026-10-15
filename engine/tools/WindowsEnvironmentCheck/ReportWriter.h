#pragma once

#include "EnvironmentProbe.h"

#include <filesystem>
#include <string>

namespace zengine::envcheck
{
std::string BuildTextReport(
    const EnvironmentSnapshot& snapshot,
    const Requirements& requirements,
    const Evaluation& evaluation);

std::string BuildJsonReport(
    const EnvironmentSnapshot& snapshot,
    const Requirements& requirements,
    const Evaluation& evaluation);

void WriteReportFile(const std::filesystem::path& path, const std::string& content);
} // namespace zengine::envcheck
