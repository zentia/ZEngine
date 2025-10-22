set(BQLog_SOURCE_DIR_ ${CMAKE_CURRENT_SOURCE_DIR}/BqLog)

# Look for common C/C++ source file extensions, recursively so files in subdirs are found.
# CONFIGURE_DEPENDS makes CMake re-run when files are added/removed.
file(GLOB_RECURSE BQLog_sources CONFIGURE_DEPENDS
	"${BQLog_SOURCE_DIR_}/src/*.c"
	"${BQLog_SOURCE_DIR_}/src/*.cpp"
	"${BQLog_SOURCE_DIR_}/src/*.cc"
	"${BQLog_SOURCE_DIR_}/src/*.cxx"
)

if (BQLog_sources)
	message(STATUS "BqLog: found sources:")
	foreach(_f ${BQLog_sources})
		message(STATUS "  ${_f}")
	endforeach()
	add_library(BqLog STATIC ${BQLog_sources})
	# Headers live under both 'include/' and 'src/' (e.g. src/bq_common/*, src/bq_log/*).
	target_include_directories(BqLog PUBLIC
		$<BUILD_INTERFACE:${BQLog_SOURCE_DIR_}/include>
		$<BUILD_INTERFACE:${BQLog_SOURCE_DIR_}/src>
	)
else()
	message(STATUS "BqLog: no sources found in ${BQLog_SOURCE_DIR_}/src — skipping BqLog target")
endif()