file(GLOB stb_sources CONFIGURE_DEPENDS  "${ENGINE_TOOL_DIR}/renderdoc/renderdoc/3rdparty/stb/*.h")
add_library(stb INTERFACE ${stb_sources})
# Use generator expressions so the INTERFACE include path isn't a raw source dir in the property
target_include_directories(stb INTERFACE
	$<BUILD_INTERFACE:${ENGINE_TOOL_DIR}/renderdoc/renderdoc/3rdparty/stb>
	$<INSTALL_INTERFACE:include/stb>
)