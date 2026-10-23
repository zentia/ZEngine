# =====================================================================
# 分配器配对冒烟测试（MemoryPairingSmokeTest）
#
#   cmake -B build -DZENGINE_BUILD_MEMORY_PAIRING_SMOKE_TEST=ON
#   cmake --build build --target MemoryPairingSmokeTest
#
# 盯的是一类**只在 Apple 上致命、其余平台全程隐形**的 bug：
#
#   ObjectManager::Produce()  -> ProduceHelper 里的 `new T()`
#   MemoryManager::CreateObject() -> mi_malloc + placement new
#
# 非 Apple 上 mimalloc-new-delete.h 接管了全局 operator new/delete，`new` 就是
# mimalloc ⇒ 两条路互相喂也碰巧能跑。Apple 上**故意不接管**（否则 dylib 与 host
# 进程会交叉释放），于是把 Produce 出来的指针喂给 mi_free 会在
# mi_usable_size -> mi_checked_ptr_segment 里 EXC_BAD_ACCESS / SIGBUS —— 崩在释放
# 路径上，栈里完全看不出是谁配错的。
#
# 本测试两条腿：
#   1. 运行时：真的 Produce 一个出来，验证 IsMimallocOwned() == false（Apple），
#      并验证 DestroyObject() 会被守卫拦下而不是崩。
#   2. 源码文本：全库扫"new 出来的对象被 mimalloc 释放"，防回归。
# =====================================================================

option(ZENGINE_BUILD_MEMORY_PAIRING_SMOKE_TEST
       "Build the allocator-pairing (Produce vs CreateObject) smoke-test executable"
       OFF)

if(NOT ZENGINE_BUILD_MEMORY_PAIRING_SMOKE_TEST)
    return()
endif()

add_executable(MemoryPairingSmokeTest MemoryPairingSmokeTest.cpp)

set_target_properties(MemoryPairingSmokeTest PROPERTIES
    CXX_STANDARD 23
    FOLDER "Engine/Tests"
)

target_link_libraries(MemoryPairingSmokeTest PRIVATE ZRuntime)

if(TARGET ZEnginePCH)
    target_link_libraries(MemoryPairingSmokeTest PRIVATE ZEnginePCH)
    target_include_directories(MemoryPairingSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
    add_dependencies(MemoryPairingSmokeTest ZEnginePCH)
    target_precompile_headers(MemoryPairingSmokeTest REUSE_FROM ZEnginePCH)
endif()

if(MSVC)
    target_compile_options(MemoryPairingSmokeTest PRIVATE /W4)
endif()

# 文本契约按绝对路径读源码，不依赖工作目录。
if(DEFINED ENGINE_ROOT_DIR)
    target_compile_definitions(MemoryPairingSmokeTest PRIVATE
        "ZENGINE_ENGINE_ROOT_DIR=\"${ENGINE_ROOT_DIR}\"")
endif()

# macOS: libZRuntime.a 在 Apple 上有若干未解析的渲染符号，-force_load 会把它们的
# 调用者一起拖进来。本测试不碰渲染器，用 aborting stub 顶掉（同其它冒烟测试）。
if(APPLE)
    target_sources(MemoryPairingSmokeTest PRIVATE
        ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
endif()

add_test(NAME MemoryPairingSmokeTest COMMAND MemoryPairingSmokeTest)

message(STATUS "Configured allocator-pairing smoke test: MemoryPairingSmokeTest")
