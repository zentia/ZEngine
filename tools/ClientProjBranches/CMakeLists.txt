cmake_minimum_required(VERSION 3.20)
project(ClientProjBranches LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ZSlate: the engine's canonical copy (git submodule), built as a static library.
set(ZSLATE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../engine/Libraries/ZSlate")
if(NOT EXISTS "${ZSLATE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "ZSlate not found at ${ZSLATE_PATH} - run 'git submodule update --init engine/Libraries/ZSlate'")
endif()
# Guarded so this project can also be added to a full-tree configure, where
# add_subdirectory(engine) has already created the ZSlate target. Adding it a
# second time is a hard CMake error ("another target with the same name
# already exists"), not a silent no-op.
if(NOT TARGET ZSlate)
    add_subdirectory("${ZSLATE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/ZSlate")
endif()

# Our source files
set(SOURCES
    src/main.cpp
    src/StackTraceApp.cpp
    src/Services/JsonConfig.cpp
    src/UI/MainApp.cpp
    src/Svn/SvnProbe.cpp
)

# App icon. rc.exe resolves the .ico next to the .rc, so both live in
# resource/. MSVC-only because .rc is a Windows resource script; the tool is
# Win32-only anyway (d3d11, shell32, comdlg32).
if(MSVC)
    list(APPEND SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resource/ClientProjBranches.rc)
endif()

# Create main executable (Win32 GUI, not console)
add_executable(ClientProjBranches WIN32 ${SOURCES})

if(MSVC)
    target_compile_options(ClientProjBranches PRIVATE /utf-8)
endif()

# Link libraries
target_link_libraries(ClientProjBranches PRIVATE
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
add_custom_command(TARGET ClientProjBranches POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$ENV{LOCALAPPDATA}/ClientProjBranches"
)
