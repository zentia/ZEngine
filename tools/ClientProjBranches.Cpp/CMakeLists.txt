cmake_minimum_required(VERSION 3.20)
project(ClientProjBranches.Cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ZSlate: the engine's canonical copy (git submodule), built as a static library.
set(ZSLATE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/Libraries/ZSlate")
if(NOT EXISTS "${ZSLATE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "ZSlate not found at ${ZSLATE_PATH} - run 'git submodule update --init engine/Libraries/ZSlate'")
endif()
add_subdirectory("${ZSLATE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/ZSlate")

# Our source files
set(SOURCES
    src/main.cpp
    src/StackTraceApp.cpp
    src/Services/JsonConfig.cpp
    src/UI/MainApp.cpp
    src/Svn/SvnProbe.cpp
)

# Create main executable (Win32 GUI, not console)
add_executable(ClientProjBranches.Cpp WIN32 ${SOURCES})

if(MSVC)
    target_compile_options(ClientProjBranches.Cpp PRIVATE /utf-8)
endif()

# Link libraries
target_link_libraries(ClientProjBranches.Cpp PRIVATE
    ZSlate
    d3d11
    dxgi
    d3dcompiler
    ole32
    user32
    gdi32
    comdlg32
    shell32
)

# Copy settings directory on build
add_custom_command(TARGET ClientProjBranches.Cpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$ENV{LOCALAPPDATA}/ClientProjBranches"
)
