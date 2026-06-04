# Copy vendored RenderDoc replay runtime next to ZEditor.
# qrenderdoc.exe needs Qt/Python DLLs, qtplugins/, and pymodules/ in the same tree.
# Skips quietly when tools/renderdoc has not been built yet.

if(NOT DEFINED RENDERDOC_SRC_DIR OR NOT DEFINED RENDERDOC_DST_DIR)
    message(FATAL_ERROR "CopyRenderDocRuntime.cmake requires RENDERDOC_SRC_DIR and RENDERDOC_DST_DIR")
endif()

if(NOT IS_DIRECTORY "${RENDERDOC_SRC_DIR}")
    message(STATUS "RenderDoc: skipped (not found at ${RENDERDOC_SRC_DIR})")
    return()
endif()

file(MAKE_DIRECTORY "${RENDERDOC_DST_DIR}")

set(_renderdoc_copied 0)

foreach(_exe IN ITEMS qrenderdoc.exe renderdoccmd.exe)
    set(_src "${RENDERDOC_SRC_DIR}/${_exe}")
    if(EXISTS "${_src}")
        file(COPY "${_src}" DESTINATION "${RENDERDOC_DST_DIR}")
        math(EXPR _renderdoc_copied "${_renderdoc_copied} + 1")
        message(STATUS "RenderDoc: copied ${_exe} -> ${RENDERDOC_DST_DIR}")
    endif()
endforeach()

file(GLOB _renderdoc_dlls "${RENDERDOC_SRC_DIR}/*.dll")
foreach(_dll IN LISTS _renderdoc_dlls)
    file(COPY "${_dll}" DESTINATION "${RENDERDOC_DST_DIR}")
    math(EXPR _renderdoc_copied "${_renderdoc_copied} + 1")
endforeach()

foreach(_file IN ITEMS python36.zip renderdoc.json _ctypes.pyd)
    set(_src "${RENDERDOC_SRC_DIR}/${_file}")
    if(EXISTS "${_src}")
        file(COPY "${_src}" DESTINATION "${RENDERDOC_DST_DIR}")
        math(EXPR _renderdoc_copied "${_renderdoc_copied} + 1")
    endif()
endforeach()

foreach(_dir IN ITEMS qtplugins pymodules)
    set(_src "${RENDERDOC_SRC_DIR}/${_dir}")
    if(IS_DIRECTORY "${_src}")
        file(COPY "${_src}" DESTINATION "${RENDERDOC_DST_DIR}")
        math(EXPR _renderdoc_copied "${_renderdoc_copied} + 1")
        message(STATUS "RenderDoc: copied ${_dir}/ -> ${RENDERDOC_DST_DIR}")
    endif()
endforeach()

if(_renderdoc_copied EQUAL 0)
    message(STATUS "RenderDoc: no runtime files found under ${RENDERDOC_SRC_DIR} (run tools/build_renderdoc.bat)")
else()
    message(STATUS "RenderDoc: runtime bundle staged in ${RENDERDOC_DST_DIR}")
endif()
