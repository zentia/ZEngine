# =====================================================================
# TextureCompressor encode smoke test (texture cook, Phase 2).
# ---------------------------------------------------------------------
# Standalone executable that encodes a synthetic image through every
# TextureCompressor format and validates mip count / offset / blob-size
# structure. Default OFF; opt in with:
#
#   cmake -B build -DZENGINE_BUILD_TEXTURE_COMPRESSOR_SMOKE_TEST=ON
#   cmake --build build --target TextureCompressorSmokeTest
#
# Mirrors the schema-evolution smoke-test wiring: PRIVATE link to
# ZRuntime (for RenderType.h), the encoder static libs, and stb.
# =====================================================================

option(ZENGINE_BUILD_TEXTURE_COMPRESSOR_SMOKE_TEST
       "Build the TextureCompressor encode smoke-test executable (texture cook P2)"
       OFF)

if(NOT ZENGINE_BUILD_TEXTURE_COMPRESSOR_SMOKE_TEST)
    return()
endif()

add_executable(TextureCompressorSmokeTest
    TextureCompressorSmokeTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../TextureCompressor.cpp
)

set_target_properties(TextureCompressorSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

# TextureCompressor.h lives one directory up.
target_include_directories(TextureCompressorSmokeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(TextureCompressorSmokeTest PRIVATE ZRuntime stb)
if(TARGET bc7enc)
    target_link_libraries(TextureCompressorSmokeTest PRIVATE bc7enc)
endif()
if(TARGET astcenc)
    target_link_libraries(TextureCompressorSmokeTest PRIVATE astcenc)
endif()

if(TARGET ZEnginePCH)
    target_link_libraries(TextureCompressorSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(TextureCompressorSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(TextureCompressorSmokeTest ZEnginePCH)
    target_precompile_headers(TextureCompressorSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    # Encoder + stb headers are warning-noisy; keep the test's own code at /W4
    # would be ideal but REUSE_FROM ZEnginePCH fixes the flag set, so just
    # silence the third-party-driven noise here.
    target_compile_options(TextureCompressorSmokeTest PRIVATE /W3)
endif()

message(STATUS "Configured TextureCompressor encode smoke-test target: TextureCompressorSmokeTest")
