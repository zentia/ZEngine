cmake_minimum_required(VERSION 3.20)
project(ClientProjBranches.Cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ZSlate path
set(ZSLATE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../ZSlate")

# Include directories
include_directories(${ZSLATE_PATH}/include)
include_directories(${ZSLATE_PATH}/thirdparty)

# ZSlate sources (recursive so every module directory is picked up)
file(GLOB_RECURSE ZSLATE_SOURCES ${ZSLATE_PATH}/src/*.cpp)
if(NOT ZSLATE_SOURCES)
    message(FATAL_ERROR "No ZSlate sources found under ${ZSLATE_PATH}/src — is the ZSlate submodule present?")
endif()

# Our source files
set(SOURCES
    src/main.cpp
    src/StackTraceApp.cpp
    src/Services/JsonConfig.cpp
    src/UI/MainApp.cpp
    src/Svn/SvnProbe.cpp
)

# Create main executable (Win32 GUI, not console)
add_executable(ClientProjBranches.Cpp WIN32 ${SOURCES} ${ZSLATE_SOURCES})

# Link libraries
target_link_libraries(ClientProjBranches.Cpp PRIVATE
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
