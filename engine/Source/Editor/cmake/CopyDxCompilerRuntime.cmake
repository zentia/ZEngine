# Stage dxcompiler.dll + dxil.dll next to ZEditor so DXC works without Vulkan SDK on PATH.

if(NOT DEFINED DXC_RUNTIME_DST_DIR)
    message(FATAL_ERROR "CopyDxCompilerRuntime.cmake requires DXC_RUNTIME_DST_DIR")
endif()

if(NOT WIN32)
    return()
endif()

set(_dxc_bin_dir "")

if(DEFINED ENV{VULKAN_SDK} AND NOT "$ENV{VULKAN_SDK}" STREQUAL "")
    set(_candidate "$ENV{VULKAN_SDK}/Bin")
    if(EXISTS "${_candidate}/dxcompiler.dll")
        set(_dxc_bin_dir "${_candidate}")
    endif()
endif()

if(_dxc_bin_dir STREQUAL "")
    find_program(_dxc_exe NAMES dxc dxc.exe)
    if(_dxc_exe)
        cmake_path(GET _dxc_exe PARENT_PATH _dxc_bin_dir)
    endif()
endif()

if(_dxc_bin_dir STREQUAL "" OR NOT EXISTS "${_dxc_bin_dir}/dxcompiler.dll")
    message(WARNING "DX12 shaders: dxcompiler.dll not found (set VULKAN_SDK or install Windows 10 SDK). "
                    "RP1 mesh shaders will fail at runtime unless dxcompiler.dll is beside ZEditor.exe.")
    return()
endif()

file(MAKE_DIRECTORY "${DXC_RUNTIME_DST_DIR}")

set(_dxc_copied 0)
foreach(_dll IN ITEMS dxcompiler.dll dxil.dll)
    set(_src "${_dxc_bin_dir}/${_dll}")
    if(EXISTS "${_src}")
        file(COPY "${_src}" DESTINATION "${DXC_RUNTIME_DST_DIR}")
        math(EXPR _dxc_copied "${_dxc_copied} + 1")
        message(STATUS "DX12: copied ${_dll} -> ${DXC_RUNTIME_DST_DIR}")
    endif()
endforeach()

if(_dxc_copied EQUAL 0)
    message(WARNING "DX12 shaders: no DXC runtime DLLs copied from ${_dxc_bin_dir}")
endif()
