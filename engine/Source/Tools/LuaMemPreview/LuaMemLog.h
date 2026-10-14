#pragma once

// ============================================================================
// LuaMemLog - bqlog front-end for the tool's own diagnostics
// ============================================================================
//
// Every other engine executable logs through bqlog (LogSystem), so this tool
// should too: its output then lands in the usual per-executable log file
// (%USERPROFILE%\AppData\LocalLow\ZEngine\Logs\LuaMemPreview_*.log) AND on the
// console, which is exactly the one place a GUI-subsystem tool can still be
// read from. Plain printf gets neither.
//
// The indirection exists because main.cpp cannot include CommonPCH/pch.h: that
// pulls windows.h, whose `DrawText` macro breaks the ZSlate headers
// (ISlateRenderer::DrawText). Only the .cpp - which sees no ZSlate header at
// all - is compiled with pch.h forced on (see CMakeLists).
//
// NOT for the data output: --stats tables, the exported-file list and --help
// stay on stdout, because that is the product of those modes, not a log.

#include <string>

namespace LuaMemPreview
{
void LogInfo(const std::string& message);
void LogWarning(const std::string& message);
void LogError(const std::string& message);

// bqlog is asynchronous; headless modes exit immediately, so the tail has to
// be drained explicitly.
void LogFlush();
}  // namespace LuaMemPreview
