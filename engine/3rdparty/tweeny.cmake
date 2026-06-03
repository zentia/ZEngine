# Tweeny - A modern C++ tweening library
# Header-only library, requires C++17 or higher
file(GLOB tweeny_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tweeny/include/tweeny.h")
add_library(tweeny INTERFACE ${tweeny_sources})
# Use BUILD/INSTALL interface include directories to avoid source-prefixed INTERFACE paths
target_include_directories(tweeny INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tweeny/include>
	$<INSTALL_INTERFACE:include/tweeny>
)
target_compile_features(tweeny INTERFACE cxx_std_17)

