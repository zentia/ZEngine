set(modules_folder "Modules")

# RenderDoc wraps a host-only RenderDoc import library and only makes sense on
# desktop developer builds. Skip it on mobile (Android/iOS/OHOS) and Web
# (Emscripten) so wasm-ld is never asked to consume the Windows .lib import file
# (PE/COFF, not wasm). This mirrors the link guard in Source/Runtime/CMakeLists.txt.
if(NOT ANDROID AND NOT IOS AND NOT APPLE AND NOT Z_PLATFORM_OHOS AND NOT OHOS AND NOT CMAKE_SYSTEM_NAME STREQUAL "OHOS"
   AND NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    if(NOT TARGET render_doc)
        add_subdirectory(RenderDoc)
        set_target_properties(render_doc PROPERTIES FOLDER ${modules_folder}/RenderDoc)
        z_set_config_archive_dirs(render_doc "${CMAKE_CURRENT_BINARY_DIR}/RenderDoc")
    endif()
endif()
