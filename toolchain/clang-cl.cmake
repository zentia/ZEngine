# ClangCL Toolchain File for Windows
# This toolchain file configures CMake to use ClangCL (clang-cl) compiler on Windows

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Set the C and C++ compilers to clang-cl
# CMake will search for clang-cl in PATH or use Visual Studio's ClangCL toolset
set(CMAKE_C_COMPILER "clang-cl")
set(CMAKE_CXX_COMPILER "clang-cl")

# Set the compiler ID
set(CMAKE_C_COMPILER_ID "Clang")
set(CMAKE_CXX_COMPILER_ID "Clang")

# Set the compiler version (will be detected automatically)
# CMAKE_C_COMPILER_VERSION and CMAKE_CXX_COMPILER_VERSION will be set by CMake

# Use MSVC-style runtime library
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Set default flags for ClangCL
set(CMAKE_C_FLAGS_INIT "/EHsc /W3")
set(CMAKE_CXX_FLAGS_INIT "/EHsc /W3 /std:c++20")

# Set the linker
set(CMAKE_LINKER "lld-link")

# Enable MSVC compatibility mode
set(CMAKE_C_COMPILER_FRONTEND_VARIANT "MSVC")
set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT "MSVC")

# Try to find mt.exe (Windows Manifest Tool) from Windows SDK
# CMake's vs_link_exe wrapper requires mt.exe even when using lld-link
# Search in common Windows SDK locations, prioritizing x64 version
set(MT_SEARCH_PATHS "")

# Check environment variable first
if(DEFINED ENV{WindowsSdkDir})
    list(APPEND MT_SEARCH_PATHS "$ENV{WindowsSdkDir}bin/x64")
    list(APPEND MT_SEARCH_PATHS "$ENV{WindowsSdkDir}bin/x86")
endif()

# Search in Program Files (x86) - most common location
if(EXISTS "C:/Program Files (x86)/Windows Kits/10/bin")
    file(GLOB WINSDK_VERSIONS "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
    if(WINSDK_VERSIONS)
        list(SORT WINSDK_VERSIONS)
        list(REVERSE WINSDK_VERSIONS)  # Get latest version first
        foreach(VER ${WINSDK_VERSIONS})
            list(APPEND MT_SEARCH_PATHS "${VER}/x64")
            list(APPEND MT_SEARCH_PATHS "${VER}/x86")
        endforeach()
    endif()
endif()

# Search in Program Files (less common)
if(EXISTS "C:/Program Files/Windows Kits/10/bin")
    file(GLOB WINSDK_VERSIONS "C:/Program Files/Windows Kits/10/bin/10.*")
    if(WINSDK_VERSIONS)
        list(SORT WINSDK_VERSIONS)
        list(REVERSE WINSDK_VERSIONS)  # Get latest version first
        foreach(VER ${WINSDK_VERSIONS})
            list(APPEND MT_SEARCH_PATHS "${VER}/x64")
            list(APPEND MT_SEARCH_PATHS "${VER}/x86")
        endforeach()
    endif()
endif()

# Search for mt.exe
find_program(MT_EXE mt.exe
    PATHS ${MT_SEARCH_PATHS}
    DOC "Windows Manifest Tool"
    NO_DEFAULT_PATH
)

# If not found in SDK paths, try PATH as fallback
if(NOT MT_EXE)
    find_program(MT_EXE mt.exe DOC "Windows Manifest Tool")
endif()

if(MT_EXE)
    # Found mt.exe, use it
    set(CMAKE_MT "${MT_EXE}" CACHE FILEPATH "Manifest tool" FORCE)
    message(STATUS "Found Windows Manifest Tool: ${MT_EXE}")
else()
    # mt.exe not found - this will cause CMake compiler test to fail
    # We need to either find it or provide a workaround
    message(WARNING "Windows Manifest Tool (mt.exe) not found!")
    message(WARNING "CMake's compiler test may fail. Please ensure Windows SDK is installed.")
    message(WARNING "You can install it via Visual Studio Installer -> Individual Components -> Windows SDK")
    
    # Try to set CMAKE_MT to empty and disable manifest generation
    # Note: This may still cause issues during compiler test
    set(CMAKE_MT "" CACHE FILEPATH "Manifest tool (not found)" FORCE)
    
    # Disable manifest generation entirely for ClangCL builds
    set(CMAKE_EXE_LINKER_FLAGS_INIT "/manifest:no ${CMAKE_EXE_LINKER_FLAGS_INIT}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "/manifest:no ${CMAKE_SHARED_LINKER_FLAGS_INIT}")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "/manifest:no ${CMAKE_MODULE_LINKER_FLAGS_INIT}")
endif()

