# AppManifest.cmake
# =============================================================================
# z_embed_app_manifest(<target> [MANIFEST <file>])
#
# Declares per-monitor DPI awareness for a Windows executable. One call replaces
# what WindowsEnvironmentCheck used to do by hand: keep a
# <Name>.manifest beside its sources and list it in its source list.
#
# Arguments:
#   target   - executable target to attach the manifest to
#   MANIFEST - use this file instead of the shared one. Escape hatch for a tool
#              needing something the default does not declare (execution level,
#              comctl32 v6, long paths, ...). A process carries exactly one
#              manifest, so this *replaces* the default rather than adding to it.
#
# Behaviour:
#   * Embedding is done by mt.exe, so this is MSVC-only. CMake ships no
#     equivalent rule for MinGW, where a .manifest in the sources would be
#     handed to windres instead. Off Windows or off MSVC the call is a no-op
#     with a STATUS message, so it can be committed anywhere.
#   * mt.exe *merges* this with the linker's own default manifest, so the
#     default <trustInfo> (asInvoker) survives -- checked on the built exes,
#     which come out with both the trustInfo and the DPI settings.
#
# Who needs it:
#   * A GUI that does NOT go through ZSlate::FSlateStandaloneApp. Today that is
#     only WindowsEnvironmentCheck, which registers its own WNDCLASSEX: no
#     DeclareDpiAwareness() runs on its path, so the manifest is its only
#     declaration and without it the window is DPI-virtualised (rendered at
#     96 dpi, bitmap-stretched by DWM, ClearType off -> soft text).
#   * Everything else declares the same mode in code before its first window --
#     DeclareDpiAwareness() in ZSlate, SetProcessDpiAwarenessContext() in
#     ZRuntime's WindowsApplication. Those do not need this, but calling it
#     anyway is harmless and slightly better: the manifest is in force before
#     main() runs, so it cannot be beaten by a window created too early.
#
# Why not a library: DPI declaration has to be in the executable's own manifest,
# so there is nothing to link -- this is a build-time concern only.
# =============================================================================

set(_Z_APP_MANIFEST_SHARED "${CMAKE_CURRENT_LIST_DIR}/AppManifest.manifest")

function(z_embed_app_manifest TARGET)
    if(NOT TARGET ${TARGET})
        message(FATAL_ERROR "z_embed_app_manifest: '${TARGET}' is not a target")
    endif()

    if(NOT WIN32)
        message(STATUS "[AppManifest] ${TARGET}: not Windows; manifest skipped")
        return()
    endif()

    if(NOT MSVC)
        message(STATUS "[AppManifest] ${TARGET}: not MSVC (no mt.exe); manifest skipped")
        return()
    endif()

    cmake_parse_arguments(MFST "" "MANIFEST" "" ${ARGN})
    if(MFST_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "z_embed_app_manifest: unknown arguments: ${MFST_UNPARSED_ARGUMENTS}")
    endif()

    set(_manifest "${_Z_APP_MANIFEST_SHARED}")
    set(_origin "shared")
    if(MFST_MANIFEST)
        if(NOT EXISTS "${MFST_MANIFEST}")
            message(FATAL_ERROR "z_embed_app_manifest: MANIFEST not found: ${MFST_MANIFEST}")
        endif()
        set(_manifest "${MFST_MANIFEST}")
        set(_origin "custom")
    endif()

    target_sources(${TARGET} PRIVATE "${_manifest}")
    message(STATUS "[AppManifest] ${TARGET}: ${_origin} manifest (per-monitor DPI v2)")
endfunction()
