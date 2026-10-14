cmake_minimum_required(VERSION 3.20)
project(ZSlate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ZSlate is consumed two ways:
#   1. Client projects glob ZSlate/src directly (see ClientProjBranches.Cpp).
#   2. This file builds it as a standalone static library.
file(GLOB_RECURSE ZSLATE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(ZSlate STATIC ${ZSLATE_SOURCES})

target_include_directories(ZSlate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(ZSlate PUBLIC
    d3d11
    dxgi
    d3dcompiler
    ole32
    user32
    gdi32
)

# MSVC specifics
if(MSVC)
    target_compile_options(ZSlate PRIVATE /W3 /permissive-)
    target_compile_definitions(ZSlate PRIVATE
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
    )
endif()
