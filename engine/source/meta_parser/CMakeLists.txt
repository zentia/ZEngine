#cmake_policy(SET CMP0074 OLD)
set(TARGET_NAME ZParser)

#set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../CMake")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

file(GLOB_RECURSE HEADERS "*.h")
file(GLOB_RECURSE SOURCES "*.cpp")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${HEADERS} ${SOURCES})


set(LLVM_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/include ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/mustache ${CMAKE_CURRENT_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${ENGINE_ROOT_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${ENGINE_ROOT_DIR}/bin)
# add LLVM includes
include_directories(${LLVM_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/parser)

# header files are superflous, but some IDEs (Visual Studio) don't include
# them in the solution explorer without them being added to the list of sources
add_executable(${TARGET_NAME} ${HEADERS} ${SOURCES})

#set_target_properties(meta_parser PROPERTIES FOLDER "generator" ) 

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 17)
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "Tools")

if (CMAKE_HOST_WIN32)
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/x64)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/x64)
    # Apply MSVC specific optimization flags only for Release-like configurations
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:/O2;/Ob2>
    )
    target_link_libraries(${TARGET_NAME}  ${LLVM_LIBRARY_DIR}/libclang.lib)
elseif(${CMAKE_HOST_SYSTEM_NAME} STREQUAL "Linux")
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/Linux)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/Linux)
    # Use -O3 for non-MSVC compilers (clang/gcc) only for Release-like configurations
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:-O3>
    )
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.so.12)
elseif(${CMAKE_HOST_SYSTEM_PROCESSOR} STREQUAL "arm64") 
    # link to XCode Toolchains' universal binary libclang.dylib 
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/macOS)
    set(LLVM_SHARED_LIBRARY_DIR  ${OSX_DEVELOPER_ROOT}/Toolchains/XcodeDefault.xctoolchain/usr/lib)
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>>:-O3>
    )
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.dylib)    
else()
    set(LLVM_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/lib/macOS)
    set(LLVM_SHARED_LIBRARY_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/3rd_party/LLVM/bin/macOS)
    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>
    )
    target_link_libraries(${TARGET_NAME}  ${LLVM_SHARED_LIBRARY_DIR}/libclang.dylib)
endif()

#link_directories(${LLVM_LIBRARY_DIR})

# statically link with Boost & LibClang

add_definitions(-DTIXML_USE_STL)

# copy resources on post build
add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
	# mustache templates directory
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${LLVM_SHARED_LIBRARY_DIR}"
		$<TARGET_FILE_DIR:${TARGET_NAME}>
)

