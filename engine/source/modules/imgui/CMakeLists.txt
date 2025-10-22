file(GLOB imgui_sources CONFIGURE_DEPENDS  "*.cpp")
file(GLOB imgui_impl CONFIGURE_DEPENDS  
"./backends/imgui_impl_glfw.cpp" 
"./backends/imgui_impl_glfw.h"
"./backends/imgui_impl_vulkan.cpp" 
"./backends/imgui_impl_vulkan.h")
add_library(imgui STATIC ${imgui_sources} ${imgui_impl})
target_include_directories(imgui PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine/source
     $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/3rdparty/mimalloc/include>
)
target_include_directories(imgui PUBLIC $<BUILD_INTERFACE:${vulkan_include}>)
target_link_libraries(imgui PUBLIC glfw ${vulkan_lib})