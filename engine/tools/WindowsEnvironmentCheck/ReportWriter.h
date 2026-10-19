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

// Writes `content` to `path` and publishes it by rename, so a reader that opens
// `path` at any moment sees either the previous complete file or the new
// complete file -- never an empty or half-written one.
//
// That matters because the launcher reads these files asynchronously: it polls
// the output directory while this tool is still running. Writing straight into
// the destination (what this used to do) leaves a window in which the file on
// disk is truncated, which for a JSON report is a parse failure at best.
//
// The destination is replaced if it already exists; a run that fails throws and
// leaves no temporary behind. Throwing is the caller's signal that no report was
// produced, which is what lets the launcher treat "no file" as "no check ran".
//
// Known limit: replacing a file needs DELETE access to it, and a handle opened
// by MSVC's CRT (std::ifstream) shares read and write but not delete. A reader
// that keeps the report open therefore blocks publishing, and this waits about
// a second before giving up. Read the file and close it -- or open it with
// FILE_SHARE_DELETE -- and it never comes up.
void WriteReportFile(const std::filesystem::path& path, const std::string& content);
} // namespace zengine::envcheck
