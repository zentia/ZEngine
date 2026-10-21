# 跨语言对拍的驱动：
#   1. C++ 把夹具容器落盘（SpineGpuImportTest --dump）
#   2. C# 读回来再写一遍，要求逐字节相同
#
# 用 cmake -P 跑（而不是直接 add_test 一个 exe）是因为它是**两步**：
# 先出字节、再验字节，中间还要 dotnet build。
#
# 期望变量（由 CMakeLists 传进来）：
#   DUMP_EXE      SpineGpuImportTest 的路径
#   DUMP_DIR      容器落盘目录
#   CSPROJ        C# 测试工程
#   DOTNET        dotnet 可执行文件路径（空 ⇒ 跳过）

if(NOT DOTNET)
    message(STATUS "[skip] 没找到 dotnet ⇒ 跳过 GPU-Spine 跨语言对拍")
    return()
endif()

if(NOT EXISTS "${DUMP_EXE}")
    message(FATAL_ERROR "找不到 ${DUMP_EXE}")
endif()

file(REMOVE_RECURSE "${DUMP_DIR}")
file(MAKE_DIRECTORY "${DUMP_DIR}")

# ---- 1. C++ 落盘 ----
execute_process(
    COMMAND "${DUMP_EXE}" --dump "${DUMP_DIR}"
    RESULT_VARIABLE _dump_res
    OUTPUT_VARIABLE _dump_out
    ERROR_VARIABLE _dump_out)
if(NOT _dump_res EQUAL 0)
    message(STATUS "${_dump_out}")
    message(FATAL_ERROR "C++ 落盘失败（exit ${_dump_res}）")
endif()
string(REGEX MATCH "dump: ([0-9]+)" _m "${_dump_out}")
if(NOT _m)
    message(FATAL_ERROR "C++ 落盘没输出容器数：${_dump_out}")
endif()
message(STATUS "C++ 落盘 ${CMAKE_MATCH_1} 个容器 → ${DUMP_DIR}")

# ---- 2. C# 建 ----
execute_process(
    COMMAND "${DOTNET}" build "${CSPROJ}" -v q --nologo
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE _b_res
    OUTPUT_VARIABLE _b_out
    ERROR_VARIABLE _b_out)
if(NOT _b_res EQUAL 0)
    message(STATUS "${_b_out}")
    message(FATAL_ERROR "C# 工程编译失败（exit ${_b_res}）")
endif()

# ---- 3. C# 对拍 ----
get_filename_component(_csproj_dir "${CSPROJ}" DIRECTORY)
file(GLOB _dll "${_csproj_dir}/bin/Debug/net*/ZEngine.SpineGpu.Test.dll")
if(NOT _dll)
    message(FATAL_ERROR "找不到 C# 测试产物：${_csproj_dir}/bin/Debug/net*/ZEngine.SpineGpu.Test.dll")
endif()
list(GET _dll 0 _dll)

execute_process(
    COMMAND "${DOTNET}" "${_dll}" "${DUMP_DIR}"
    RESULT_VARIABLE _r_res
    OUTPUT_VARIABLE _r_out
    ERROR_VARIABLE _r_out)
message(STATUS "${_r_out}")
if(NOT _r_res EQUAL 0)
    message(FATAL_ERROR "C# 跨语言对拍失败（exit ${_r_res}）")
endif()

message(STATUS "GPU-Spine 跨语言对拍：PASS")
