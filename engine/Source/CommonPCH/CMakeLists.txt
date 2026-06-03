# Shared precompiled header - built once (C++23), reused by Runtime, Editor, Launcher, Profiler
if(NOT CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
  return()
endif()

set(PCH_TARGET_NAME ZEnginePCH)
add_library(${PCH_TARGET_NAME} STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/pch.h"
)

# ClangCL ignores EnableEnhancedInstructionSet; pass AVX2 via options that end up in AdditionalOptions.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # GCC-style flags go to AdditionalOptions; clang-cl accepts them and matches /arch:AVX2 codegen.
    target_compile_options(${PCH_TARGET_NAME} PRIVATE -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${PCH_TARGET_NAME} PRIVATE /arch:AVX2)
  endif()
endif()
target_link_libraries(${PCH_TARGET_NAME} PRIVATE EngineArchAVX2)
# Match pthread compile flags on consumers (ZRuntime links PapiQuickjs -> Threads::Threads).
# AppleClang rejects REUSE_FROM when the PCH was built without -pthread but the TU has it.
# Linking Threads::Threads alone does not pass -pthread to pch.cpp on macOS; set both.
if(NOT WIN32 AND NOT (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten"))
  find_package(Threads REQUIRED)
  target_compile_options(${PCH_TARGET_NAME} PRIVATE -pthread)
  target_link_options(${PCH_TARGET_NAME} PRIVATE -pthread)
  target_link_libraries(${PCH_TARGET_NAME} PRIVATE Threads::Threads)
endif()


set_target_properties(${PCH_TARGET_NAME} PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  FOLDER "Engine"
)

# Include dirs for pch.h and transitive includes (Runtime/Core, JSON->rapidjson, BqLog, mimalloc, xxHash, EASTL)
target_include_directories(${PCH_TARGET_NAME} PUBLIC
  $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source>
  $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source/Runtime>
  $<BUILD_INTERFACE:${ENGINE_ROOT_DIR}/Source/CommonPCH>
  $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/bqlog/include>
  $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/mimalloc/include>
  $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/rapidjson/include>
  $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/xxHash>
  $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/puerts/unity/native/EASTL/include>
)
# EASTL open-source build (used when including EASTL in PCH)
target_compile_definitions(${PCH_TARGET_NAME} PRIVATE EASTL_OPENSOURCE=1)

target_precompile_headers(${PCH_TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")

message(STATUS "Shared PCH target ${PCH_TARGET_NAME} configured (C++23)")
