# =====================================================================
# 本目录的冒烟测试可执行文件都自带 main()，因此必须从 ZRuntime 的
# GLOB_RECURSE 里 carve-out（见 Runtime/CMakeLists.txt 的 REMOVE_ITEM），
# 否则每个消费者 exe 都会 LNK2005 "main already defined"。
# =====================================================================

# =====================================================================
# UAF 检测冒烟测试（阶段一·夯地基 第一落点，#3 + #5 的运行期验证）
#
#   cmake -B build -DZENGINE_BUILD_UAF_DETECTION_SMOKE_TEST=ON
#   cmake --build build --target UafDetectionSmokeTest
#
# 证明 ImmediatePtr 的悬垂校验 + UafReporter 真的能抓到 use-after-free：
# 父进程跑安全判据（死活探测、活对象不误报），再 spawn 自身 --uaf-probe 子进程
# 触发真 UAF（Debug 下被 ASSERT 中断 → 非零退出；Release 下检测编译掉 → 零退出）。
#
# 测试可执行文件自身需要 ZENGINE_LIFETIME_CHECK 宏（父进程用它决定期望），
# 与 ZRuntime 的注入保持一致（同 Runtime/CMakeLists.txt 的多配置 + 单配置兜底）。
#
# 注意：本块必须放在 MemoryPairing 的 return() 之前 —— 否则当 MEMORY_PAIRING=OFF
# （默认）时那个 return 会直接跳出本文件，永远走不到这里（CMake 选项因此是
# UNINITIALIZED，选项形同虚设）。
# =====================================================================
option(ZENGINE_BUILD_UAF_DETECTION_SMOKE_TEST
       "Build the use-after-free detection smoke-test executable"
       OFF)

if(ZENGINE_BUILD_UAF_DETECTION_SMOKE_TEST)
    add_executable(UafDetectionSmokeTest UafDetectionSmokeTest.cpp)

    set_target_properties(UafDetectionSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(UafDetectionSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(UafDetectionSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(UafDetectionSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(UafDetectionSmokeTest ZEnginePCH)
        target_precompile_headers(UafDetectionSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(UafDetectionSmokeTest PRIVATE /W4)
    endif()

    # 与 ZRuntime 一致的 ZENGINE_LIFETIME_CHECK 注入：父进程判据按配置期望子进程行为。
    if(CMAKE_CONFIGURATION_TYPES)
        set(_uaf_lifetime_def "ZENGINE_LIFETIME_CHECK=$<OR:$<CONFIG:Debug>,$<CONFIG:DebugV8>,$<CONFIG:RelWithDebInfo>>")
    else()
        if(CMAKE_BUILD_TYPE MATCHES "Debug|DebugV8|RelWithDebInfo")
            set(_uaf_lifetime_def "ZENGINE_LIFETIME_CHECK=1")
        else()
            set(_uaf_lifetime_def "ZENGINE_LIFETIME_CHECK=0")
        endif()
    endif()
    target_compile_definitions(UafDetectionSmokeTest PRIVATE ${_uaf_lifetime_def})
    unset(_uaf_lifetime_def)

    add_test(NAME UafDetectionSmokeTest COMMAND UafDetectionSmokeTest)

    message(STATUS "Configured UAF detection smoke test: UafDetectionSmokeTest")
endif()

# =====================================================================
# 对象生命周期冒烟测试（阶段一·夯地基 第二落点 #8 两阶段销毁的运行期验证）
#
#   cmake -B build -DZENGINE_BUILD_OBJECT_LIFETIME_SMOKE_TEST=ON
#   cmake --build build --target ObjectLifetimeSmokeTest
#
# 证明两阶段销毁真的被触发：ObjectManager::Destroy 与 MemoryManager::DestroyObject
# 两条入口都调用 Object::BeginDestroySequence()，从而真正调用 BeginDestroy()/
# FinishDestroy() 虚钩子、真正写入 ObjectFlags（OF_BeginDestroyed/OF_FinishDestroyed）。
# 判据用进程级 sink 回收钩子观测值（对象销毁后无法回传），全程主进程内安全完成，
# 不涉及断言/崩溃，无需子进程隔离。
#
# 同 UAF 块一样必须放在 MemoryPairing 的 return() 之前（理由见上方 UAF 块注释）。
# =====================================================================
option(ZENGINE_BUILD_OBJECT_LIFETIME_SMOKE_TEST
       "Build the object-lifetime (two-phase destruction, #8) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_OBJECT_LIFETIME_SMOKE_TEST)
    add_executable(ObjectLifetimeSmokeTest ObjectLifetimeSmokeTest.cpp)

    set_target_properties(ObjectLifetimeSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(ObjectLifetimeSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(ObjectLifetimeSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(ObjectLifetimeSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(ObjectLifetimeSmokeTest ZEnginePCH)
        target_precompile_headers(ObjectLifetimeSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(ObjectLifetimeSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME ObjectLifetimeSmokeTest COMMAND ObjectLifetimeSmokeTest)

    message(STATUS "Configured object-lifetime smoke test: ObjectLifetimeSmokeTest")
endif()

# =====================================================================
# PPtr 生命周期冒烟测试（阶段一·夯地基 第三落点 #6：PPtr 区分"已销毁"与"未解析"）
#
#   cmake -B build -DZENGINE_BUILD_PPTR_LIFETIME_SMOKE_TEST=ON
#   cmake --build build --target PPtrLifetimeSmokeTest
#
# 证明 PPtr<T>::operator T*() 现在用 instanceID 的符号区分 IDToPointer 返回 null 的
# 两种含义：负 ID = 堆对象已销毁（→ 报告 UAF + 返回 null，不再掉进磁盘 reify），
# 正 ID = 磁盘对象未加载（→ 保留惰性 reify）。父进程跑安全判据（活 PPtr 解析、空 PPtr
# 为 null），再 spawn 自身 --destroyed-probe 子进程解引用销毁堆 PPtr（Debug 下被
# ASSERT 中断 → 非零退出；Release 下返回 null → 零退出）。
#
# 同 UAF 块一样需要 ZENGINE_LIFETIME_CHECK 注入（父进程用它决定期望的子进程行为），
# 且必须放在 MemoryPairing 的 return() 之前（理由见上方 UAF 块注释）。
# =====================================================================
option(ZENGINE_BUILD_PPTR_LIFETIME_SMOKE_TEST
       "Build the PPtr lifetime (destroyed vs unresolved, #6) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_PPTR_LIFETIME_SMOKE_TEST)
    add_executable(PPtrLifetimeSmokeTest PPtrLifetimeSmokeTest.cpp)

    set_target_properties(PPtrLifetimeSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(PPtrLifetimeSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(PPtrLifetimeSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(PPtrLifetimeSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(PPtrLifetimeSmokeTest ZEnginePCH)
        target_precompile_headers(PPtrLifetimeSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(PPtrLifetimeSmokeTest PRIVATE /W4)
    endif()

    # 与 ZRuntime 一致的 ZENGINE_LIFETIME_CHECK 注入：父进程判据按配置期望子进程行为。
    if(CMAKE_CONFIGURATION_TYPES)
        set(_pptr_lt_def "ZENGINE_LIFETIME_CHECK=$<OR:$<CONFIG:Debug>,$<CONFIG:DebugV8>,$<CONFIG:RelWithDebInfo>>")
    else()
        if(CMAKE_BUILD_TYPE MATCHES "Debug|DebugV8|RelWithDebInfo")
            set(_pptr_lt_def "ZENGINE_LIFETIME_CHECK=1")
        else()
            set(_pptr_lt_def "ZENGINE_LIFETIME_CHECK=0")
        endif()
    endif()
    target_compile_definitions(PPtrLifetimeSmokeTest PRIVATE ${_pptr_lt_def})
    unset(_pptr_lt_def)

    add_test(NAME PPtrLifetimeSmokeTest COMMAND PPtrLifetimeSmokeTest)

    message(STATUS "Configured PPtr lifetime smoke test: PPtrLifetimeSmokeTest")
endif()

# =====================================================================
# 引用计数基础设施冒烟测试（阶段一·夯地基 第六落点 #7：强/弱双计数 + RefCounted）
#
#   cmake -B build -DZENGINE_BUILD_REF_COUNTED_SMOKE_TEST=ON
#   cmake --build build --target RefCountedSmokeTest
#
# 证明 AtomicRefCounter 从"只有强计数、95% 闲置"变成强/弱双计数（保持 SerializedFile
# 强计数 API 兼容），且新增 RefCounted<T> 成为其第二个真实消费者。纯计数逻辑、无崩溃/
# 断言，主进程内安全完成，无需系统 bring-up。
#
# 同其它块一样必须放在 MemoryPairing 的 return() 之前（理由见上方 UAF 块注释）。
# =====================================================================
option(ZENGINE_BUILD_REF_COUNTED_SMOKE_TEST
       "Build the ref-count infrastructure (#7) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_REF_COUNTED_SMOKE_TEST)
    add_executable(RefCountedSmokeTest RefCountedSmokeTest.cpp)

    set_target_properties(RefCountedSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(RefCountedSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(RefCountedSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(RefCountedSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(RefCountedSmokeTest ZEnginePCH)
        target_precompile_headers(RefCountedSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(RefCountedSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME RefCountedSmokeTest COMMAND RefCountedSmokeTest)

    message(STATUS "Configured ref-count infrastructure smoke test: RefCountedSmokeTest")
endif()

# =====================================================================
# 弱引用句柄冒烟测试（阶段一·夯地基 第六落点 #7：WeakObjectPtr）
#
#   cmake -B build -DZENGINE_BUILD_WEAK_OBJECT_PTR_SMOKE_TEST=ON
#   cmake --build build --target WeakObjectPtrSmokeTest
#
# 证明 WeakObjectPtr<T>（基于 instanceID 的弱句柄）在对象存活时解析为活指针、对象
# 销毁后优雅退化为 null，且**不报告 UAF**（弱引用访问死对象是合法语义，与 ImmediatePtr
# 强引用相反）。需要系统 bring-up（RegisterCore + InitializeAll）让 ObjectManager 可用。
#
# 同其它块一样必须放在 MemoryPairing 的 return() 之前（理由见上方 UAF 块注释）。
# =====================================================================
option(ZENGINE_BUILD_WEAK_OBJECT_PTR_SMOKE_TEST
       "Build the weak-object-pointer (#7) smoke-test executable"
       OFF)

if(ZENGINE_BUILD_WEAK_OBJECT_PTR_SMOKE_TEST)
    add_executable(WeakObjectPtrSmokeTest WeakObjectPtrSmokeTest.cpp)

    set_target_properties(WeakObjectPtrSmokeTest PROPERTIES
        CXX_STANDARD 23
        FOLDER "Engine/Tests"
    )

    target_link_libraries(WeakObjectPtrSmokeTest PRIVATE ZRuntime)

    if(TARGET ZEnginePCH)
        target_link_libraries(WeakObjectPtrSmokeTest PRIVATE ZEnginePCH)
        target_include_directories(WeakObjectPtrSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
        add_dependencies(WeakObjectPtrSmokeTest ZEnginePCH)
        target_precompile_headers(WeakObjectPtrSmokeTest REUSE_FROM ZEnginePCH)
    endif()

    if(MSVC)
        target_compile_options(WeakObjectPtrSmokeTest PRIVATE /W4)
    endif()

    add_test(NAME WeakObjectPtrSmokeTest COMMAND WeakObjectPtrSmokeTest)

    message(STATUS "Configured weak-object-pointer smoke test: WeakObjectPtrSmokeTest")
endif()

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
