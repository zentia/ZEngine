set(modules_folder "Modules")

# RenderDoc wraps a host-only RenderDoc import library and only makes sense on
# desktop developer builds. Skip it on Web (Emscripten) so wasm-ld is never asked
# to consume the Windows .lib import file (PE/COFF, not wasm).
if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    if(NOT TARGET render_doc)
        add_subdirectory(RenderDoc)
        set_target_properties(render_doc PROPERTIES FOLDER ${modules_folder}/RenderDoc)
        z_set_config_archive_dirs(render_doc "${CMAKE_CURRENT_BINARY_DIR}/RenderDoc")
    endif()
endif()
