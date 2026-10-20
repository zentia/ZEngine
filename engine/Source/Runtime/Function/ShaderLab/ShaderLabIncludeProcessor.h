#pragma once

// =====================================================================
// ShaderLab #include resolver
// ---------------------------------------------------------------------
// Deliberately free of any render-backend dependency: no Vulkan, no DX12,
// no Metal -- just <filesystem> and <string>. That is what lets it be
// compiled (and unit-tested) on every configuration, including
// ZENGINE_USE_VULKAN=OFF builds where ShaderLabCompiler.cpp itself is
// excluded from ZRuntime.
//
// Search order, matching Unity's ShaderLab semantics:
//
//   #include "foo.h"   -> 1. directory of the file holding the directive
//                         2. each entry of `include_paths`, in order
//   #include <foo.h>   -> each entry of `include_paths` only
//
// A missing or cyclic include is a HARD failure. The previous
// implementation replaced the directive with
//   // WARNING: Could not find include: foo.h
// which let compilation continue against a source the author never wrote;
// the resulting diagnostic (if any) pointed at symbols that simply were
// not there, or -- worse -- the shader compiled because those symbols
// happened to exist under a different name.
// =====================================================================

#include <filesystem>
#include <string>
#include <vector>

namespace ZEngine::ShaderLab
{

    struct IncludeResolution
    {
        // Fully expanded source. Only meaningful when success == true.
        std::string code;

        // Diagnostic for the first failure. Empty when success == true.
        std::string error_message;

        // Every file actually pulled in, canonicalised, in first-encounter
        // order. Use this as a cache-invalidation key: a compiled blob must
        // be rebuilt when any of these change, not only when the .shader
        // that was compiled changes.
        std::vector<std::filesystem::path> included_files;

        bool success = false;
    };

    // Expands every #include in `source`.
    //
    // `owning_file_dir` is the directory of the file that contains `source`
    // (i.e. m_SourcePath.parent_path() for a .shader loaded from disk). Pass
    // an empty path for a synthetic source, in which case only
    // `include_paths` is searched.
    IncludeResolution ResolveIncludes(const std::string& source,
                                      const std::vector<std::string>& include_paths,
                                      const std::filesystem::path& owning_file_dir = {});

}  // namespace ZEngine::ShaderLab
