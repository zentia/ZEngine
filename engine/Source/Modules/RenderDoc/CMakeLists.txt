file(GLOB_RECURSE HEADER_FILES "*.h" "*.inl")
file(GLOB_RECURSE SOURCE_FILES "*.cpp")
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${HEADER_FILES} ${SOURCE_FILES})
add_library(render_doc STATIC ${HEADER_FILES} ${SOURCE_FILES})
target_include_directories(render_doc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/Source>
    $<INSTALL_INTERFACE:include/render_doc>
)
target_include_directories(render_doc PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/3rdparty/bqlog/include
)
if(DEFINED Z_ROOT_DIR)
    target_compile_definitions(render_doc PRIVATE ZENGINE_REPO_ROOT="${Z_ROOT_DIR}")
endif()
target_link_libraries(render_doc PRIVATE BqLog)
