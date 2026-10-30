# =============================================================================
# Optional Audiokinetic Wwise 2019.2.15 integration.
#
#   cmake -B build -DZENGINE_WWISE_SDK_DIR=E:/Agent2/SVN/Wwise/developer2019_os/wwise_sgame
#
# When ZENGINE_WWISE_SDK_DIR is empty (the default) nothing is defined and
# Runtime/Function/Audio/Wwise/WwiseAudioSystem.cpp compiles its "not compiled
# in" branch: IsCompiledIn() returns false, GetBackendName() says so, and every
# call reports failure. That is the honest state -- there is deliberately no
# build where the API exists but does nothing real.
#
# When the SDK is found, an INTERFACE target `WwiseSDK` carries the include
# dir, the per-config static library set and ZENGINE_HAS_WWISE=1.
# =============================================================================

set(ZENGINE_WWISE_SDK_DIR "" CACHE PATH "Path to a Wwise SDK root (the folder containing sdk/include and sdk/x64_vc160). Empty = no Wwise backend.")

# Wwise ships vc160 (VS2019) x64 libraries only. Anything else is not a
# supported combination, so say it once at configure time instead of letting
# the link fail with a wall of LNK2019.
function(z_wwise_platform_dir out_dir)
    set(${out_dir} "" PARENT_SCOPE)
    if(NOT WIN32)
        return()
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        return()
    endif()
    if(MSVC_VERSION LESS 1920)
        return()
    endif()
    set(${out_dir} "${ZENGINE_WWISE_SDK_DIR}/sdk/x64_vc160" PARENT_SCOPE)
endfunction()

set(ZENGINE_HAS_WWISE FALSE)

if(ZENGINE_WWISE_SDK_DIR STREQUAL "")
    message(STATUS "Wwise: disabled (set ZENGINE_WWISE_SDK_DIR to enable the Wwise audio backend)")
    return()
endif()

z_wwise_platform_dir(_wwise_platform)
if(_wwise_platform STREQUAL "")
    message(WARNING "Wwise: ZENGINE_WWISE_SDK_DIR is set but this toolchain has no matching SDK\n"
                    "       libraries (need Windows x64 + VS2019/vc160 or newer). Continuing without Wwise.")
    return()
endif()

if(NOT EXISTS "${ZENGINE_WWISE_SDK_DIR}/sdk/include/AK/SoundEngine/Common/AkSoundEngine.h")
    message(WARNING "Wwise: '${ZENGINE_WWISE_SDK_DIR}' does not look like a Wwise SDK root\n"
                    "       (missing sdk/include/AK/SoundEngine/Common/AkSoundEngine.h). Continuing without Wwise.")
    return()
endif()

# Debug/Profile/Release map 1:1 onto the SDK's own configuration names. ZEngine
# links the *non*-StaticCRT variants on purpose: the engine builds with
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded[$<CONFIG:Debug>:Debug]DLL, and mixing
# in the /MT StaticCRT libraries fails with LNK2038. The trade-off is that the
# Wwise DLLs have to sit next to the executable -- see the copy step below.
set(_wwise_configs "Debug;Profile;Release")
set(_wwise_found_all TRUE)
foreach(_cfg IN LISTS _wwise_configs)
    if(NOT IS_DIRECTORY "${_wwise_platform}/${_cfg}/lib")
        message(WARNING "Wwise: missing '${_wwise_platform}/${_cfg}/lib'. Continuing without Wwise.")
        set(_wwise_found_all FALSE)
    endif()
endforeach()
if(NOT _wwise_found_all)
    return()
endif()

# The minimum set that covers the sound engine, the music engine and every
# plug-in the shipped Wwise projects can reference. Taken from
# sdk/samples/IntegrationDemo/Windows/IntegrationDemo_Windows_vc160.vcxproj;
# dropping an entry only shows up at runtime as "plug-in not found" with the
# sound silently missing, so the list is deliberately not trimmed further.
set(_wwise_libs
    AkSoundEngine
    AkMusicEngine
    AkStreamMgr
    AkMemoryMgr
    AkSpatialAudio
    AkAudioInputSource
    AkVorbisDecoder
    AkOpusDecoder
    AkCompressorFX
    AkDelayFX
    AkExpanderFX
    AkFlangerFX
    AkGainFX
    AkGuitarDistortionFX
    AkHarmonizerFX
    AkMatrixReverbFX
    AkMeterFX
    AkParametricEQFX
    AkPeakLimiterFX
    AkPitchShifterFX
    AkRecorderFX
    AkRoomVerbFX
    AkSilenceSource
    AkSineSource
    AkStereoDelayFX
    AkSynthOneSource
    AkTimeStretchFX
    AkToneSource
    AkTremoloFX
)

add_library(WwiseSDK INTERFACE)
target_include_directories(WwiseSDK INTERFACE "${ZENGINE_WWISE_SDK_DIR}/sdk/include")
target_compile_definitions(WwiseSDK INTERFACE ZENGINE_HAS_WWISE=1)

# Wwise headers pick the platform from _WIN32/_WIN64 and the configuration from
# the compiler's own macros; AK_SUPPORT_WCHAR (wide paths) is on by default on
# Windows, which is what WwiseAudioSystem relies on for non-ASCII bank paths.
foreach(_cfg IN LISTS _wwise_configs)
    set(_lib_dir "${_wwise_platform}/${_cfg}/lib")
    unset(_abs_libs)
    foreach(_lib IN LISTS _wwise_libs)
        if(EXISTS "${_lib_dir}/${_lib}.lib")
            list(APPEND _abs_libs "${_lib_dir}/${_lib}.lib")
        else()
            message(STATUS "Wwise: ${_cfg} is missing ${_lib}.lib; skipping it")
        endif()
    endforeach()
    # Order matters: MSVC resolves symbols left to right, and the sound engine
    # depends on the memory/stream managers.
    target_link_libraries(WwiseSDK INTERFACE $<$<CONFIG:${_cfg}>:${_abs_libs}>)
endforeach()

# System dependencies the Wwise Windows sink and I/O hook need.
target_link_libraries(WwiseSDK INTERFACE ws2_32 winmm dsound msacm32 dbghelp dinput8)

# Runtime DLLs. The engine drops every executable in <repo>/bin/<Config>, so the
# Wwise DLLs have to land there too or the process dies before main().
foreach(_cfg IN LISTS _wwise_configs)
    set(_dll_dir "${_wwise_platform}/${_cfg}/bin")
    if(IS_DIRECTORY "${_dll_dir}")
        file(GLOB _wwise_dlls "${_dll_dir}/*.dll")
        if(_wwise_dlls)
            file(COPY ${_wwise_dlls} DESTINATION "${CMAKE_SOURCE_DIR}/bin/${_cfg}")
            list(LENGTH _wwise_dlls _dll_count)
            message(STATUS "Wwise: copied ${_dll_count} ${_cfg} DLL(s) into ${CMAKE_SOURCE_DIR}/bin/${_cfg}")
        endif()
    endif()
endforeach()

set(ZENGINE_HAS_WWISE TRUE)
message(STATUS "Wwise: enabled (${_wwise_platform})")
