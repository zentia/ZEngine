# =====================================================================
# Script VM end-to-end smoke test (the pesapi binding chain):
#
#   ScriptVMSmokeTest  -DZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST=ON
#
#   cmake -B build -DZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST=ON
#   cmake --build build --target ScriptVMSmokeTest
#   ctest --test-dir build -R ScriptVMSmokeTest
#
# What it is for
# --------------
# The other script smoke tests (ScriptMath / ScriptTime / ScriptInput) call the
# static methods straight from C++. None of them goes through the *binding
# chain*, so until this test existed these five things had never once executed
# -- only been compiled and code-generated:
#
#   1. ScriptBindingRegistry::InstallAll   generated ZClassScriptBinding -> pesapi class
#   2. ScriptBindingRegistry::InstallGlobals  create_class -> a global name
#   3. ZENUM -> a plain global object (Key.Space)
#   4. ScriptEnv's console / Debug / __zNewInstance shim
#   5. The module loader (IIFE wrapper + module.exports) and instance lifecycle
#
# This test really boots a QuickJS VM, evals real JS into it, and reads the
# result back.
#
# Why the .gen.cpp files are NOT listed as sources
# ------------------------------------------------
# (This section used to say the opposite. It was right before whole-archive
# linkage existed and is now actively wrong -- see below.)
#
# The static registrars (ZClassScriptBindingRegistrar) live in
# <build>/Generated/ZHT/ZRuntime/Src/<Name>.gen.cpp and hook themselves into
# ScriptBindingRegistry's intrusive list during static init. Nothing references
# them, so a plain archive link would drop the whole object and the class table
# would come out empty.
#
# That is exactly what Runtime/CMakeLists.txt solves: ZRuntime carries
#     /WHOLEARCHIVE:ZRuntime.lib       (MSVC)
#     -force_load <ZRuntime.a>         (Apple)
#     --whole-archive ... ZRuntime.a   (GNU/Clang)
# as an INTERFACE link option, so EVERY consumer of the static runtime -- this
# test included -- gets all 58 generated objects forced into the image and all
# the registrars run. `target_link_libraries(... ZRuntime)` is therefore
# sufficient; the binding table is full on its own.
#
# Compiling the same .gen.cpp into the executable on top of that is a hard
# error, not a redundancy: a .gen.cpp carries TWO kinds of symbol --
#
#   * the anonymous-namespace script registrar (internal linkage, harmless), and
#   * IMPLEMENT_REGISTER_CLASS / IMPLEMENT_OBJECT_SERIALIZE / the
#     VirtualRedirectTransfer overrides -- all STRONG external definitions.
#
# Under whole-archive the archive's copy is always present, so the strong
# symbols collide:
#   ZRuntime.lib(ScriptTime.gen.obj): error LNK2005:
#     "void __cdecl RegisterKlass<class Time>(void)" already defined in
#     ScriptTime.gen.obj
# So these targets must NOT compile any .gen.cpp themselves. Do not "solve" a
# duplicate-definition report by switching to ZRuntimeShared either: that gives
# the test its own copy of every reflected class and a second, divergent
# TypeContainer<T>::m_Type, which links cleanly and then disagrees at runtime.
#
# Backend
# -------
# QuickJS only: BackendQuickJS.h is empty unless PAPI_QUICKJS=1, and that
# definition is PRIVATE to ZRuntime, so the test has to set it itself. On a
# V8 build the whole target is skipped rather than half-configured.
# =====================================================================

if(PAPI_TYPE STREQUAL "quickjs")
    option(ZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST
           "Build the script VM end-to-end (pesapi binding chain) smoke test"
           OFF)

    if(ZENGINE_BUILD_SCRIPT_VM_SMOKE_TEST)
        add_executable(ScriptVMSmokeTest ScriptVMSmokeTest.cpp)

        set_target_properties(ScriptVMSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        # 被测的四个类（VectorMath/QuaternionMath/Mathf、Time、Input、KeyCodes）
        # 的绑定注册器由 ZRuntime 的 /WHOLEARCHIVE 带进来（见文件头），这里不重复编译。
        if(TARGET ZRuntime_ZHeaderTool)
            add_dependencies(ScriptVMSmokeTest ZRuntime_ZHeaderTool)
        endif()

        target_compile_definitions(ScriptVMSmokeTest PRIVATE PAPI_QUICKJS=1)

        target_link_libraries(ScriptVMSmokeTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(ScriptVMSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(ScriptVMSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(ScriptVMSmokeTest ZEnginePCH)
            target_precompile_headers(ScriptVMSmokeTest REUSE_FROM ZEnginePCH)
        endif()

        # macOS: libZRuntime.a references render-side symbols whose defining TUs
        # are not built for Apple; pulling their callers in would abort. This
        # test never touches the renderer, so stubs stand in (same as the other
        # smoke tests).
        if(APPLE)
            target_sources(ScriptVMSmokeTest PRIVATE
                ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
        endif()

        if(MSVC)
            target_compile_options(ScriptVMSmokeTest PRIVATE /W4)
        endif()

        add_test(NAME ScriptVMSmokeTest COMMAND ScriptVMSmokeTest)
        message(STATUS "Configured script VM end-to-end smoke test: ScriptVMSmokeTest")

        # ---------------------------------------------------------------------
        # P8 HMR（Hot Module Replacement）。
        #
        # 判定表本身是纯 ScriptEnv 的（accept / decline / dispose / cascade），
        # 不需要绑定表也不需要 GameObject，所以不喂 .gen.cpp —— 只链 ZRuntime。
        #
        # 它钉的是"接上了、不报错、只是没生效"这一整类：accept 有没有真的从 JS
        # 走到 C++、热路径有没有真的换掉 exports、decline 有没有真的回落全量
        # 重建、依赖方有没有跟着重载（只重载被改的那个会留下半新半旧的闭包）。
        # ---------------------------------------------------------------------
        add_executable(ScriptHmrSmokeTest ScriptHmrSmokeTest.cpp)

        set_target_properties(ScriptHmrSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        target_compile_definitions(ScriptHmrSmokeTest PRIVATE PAPI_QUICKJS=1)

        target_link_libraries(ScriptHmrSmokeTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(ScriptHmrSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(ScriptHmrSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(ScriptHmrSmokeTest ZEnginePCH)
            target_precompile_headers(ScriptHmrSmokeTest REUSE_FROM ZEnginePCH)
        endif()

        if(APPLE)
            target_sources(ScriptHmrSmokeTest PRIVATE
                ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
        endif()

        if(MSVC)
            target_compile_options(ScriptHmrSmokeTest PRIVATE /W4)
        endif()

        add_test(NAME ScriptHmrSmokeTest COMMAND ScriptHmrSmokeTest)
        message(STATUS "Configured P8 HMR smoke test: ScriptHmrSmokeTest")

        # ---------------------------------------------------------------------
        # TS -> tsc -> .js -> ScriptEnv -> C++ 全链路。
        #
        # 与上面那个测试的区别：它喂给 ScriptEnv 的不是手写 CommonJS，而是 **tsc
        # 按本目录 tsconfig.json 编译 Test/Scripts/*.ts 的产物**（TscOutput/，签进
        # 仓库，所以这里不需要 node）。这补的是用户真正走的那条路：
        #
        #   1. tsc 的 CommonJS 产物（Object.defineProperty(exports, "__esModule")
        #      + ES2020 原生 class）能不能被 LoadModuleInternal 的 IIFE 包装执行；
        #   2. `class Mover extends Behaviour` 的**原生 class 继承**能不能被
        #      __zNewInstance new 出来 —— instanceof Behaviour 必须为真；
        #   3. 生成的 zengine.d.ts 到底能不能被真正的类型检查器接受（交给
        #      scripts/check_ts_chain.py 用 tsc 验，那边需要 node）。
        #
        # 产物漂移由 scripts/check_ts_chain.py 管：它重新生成 d.ts、重新编译、与
        # TscOutput/ diff，再跑本测试（--js-root 指向刚编译出来的目录）。
        # ---------------------------------------------------------------------
        add_executable(ScriptTsChainSmokeTest ScriptTsChainSmokeTest.cpp)

        set_target_properties(ScriptTsChainSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        if(TARGET ZRuntime_ZHeaderTool)
            add_dependencies(ScriptTsChainSmokeTest ZRuntime_ZHeaderTool)
        endif()

        # js root 的默认位置：签进仓库的 tsc 产物目录。测试也接受 --js-root 覆盖
        # （scripts/check_ts_chain.py 用它指向刚编译出来的临时目录）。
        target_compile_definitions(ScriptTsChainSmokeTest PRIVATE
            ZENGINE_TS_CHAIN_TSCOUT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/TscOutput")

        target_compile_definitions(ScriptTsChainSmokeTest PRIVATE PAPI_QUICKJS=1)

        target_link_libraries(ScriptTsChainSmokeTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(ScriptTsChainSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(ScriptTsChainSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(ScriptTsChainSmokeTest ZEnginePCH)
            target_precompile_headers(ScriptTsChainSmokeTest REUSE_FROM ZEnginePCH)
        endif()

        if(APPLE)
            target_sources(ScriptTsChainSmokeTest PRIVATE
                ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
        endif()

        if(MSVC)
            target_compile_options(ScriptTsChainSmokeTest PRIVATE /W4)
        endif()

        add_test(NAME ScriptTsChainSmokeTest COMMAND ScriptTsChainSmokeTest)
        message(STATUS "Configured TS->tsc->VM chain smoke test: ScriptTsChainSmokeTest")

        # ---------------------------------------------------------------------
        # TypeScriptComponent 真身：把 tsc 产物挂到 GameObject 上跑完整生命周期。
        #
        # 前两个测试都是"自己 new 一个 ScriptEnv 手喂模块"。这个走的是组件侧：
        # ObjectManager::Produce(GameObject) -> AddComponentByTypeName("TypeScript
        # Component") -> SetScriptGuid -> PostLoadResource 里 ResolveScript /
        # LoadModule / CreateInstance / 注入 gameObject·transform·component ->
        # OnAwake / OnStart -> 每帧 Tick 转发 OnUpdate -> 热重载重建 -> 析构
        # OnDestroy。
        #
        # AddComponentByTypeName 和 Cast<Component> 都走 TypeManager 的类型表，
        # 而类型表正是 IMPLEMENT_REGISTER_CLASS 的静态注册器填的
        # （GameObject / Transform / Behaviour / TypeScriptComponent）。Behaviour
        # 必须在其列（TypeScriptComponent 的直接基类），否则 Builder 走不到
        # Component，Cast<Component> 的范围测试落空。这四张表连同脚本绑定表一起
        # 由 ZRuntime 的 /WHOLEARCHIVE 带进来（见文件头），这里不重复编译。
        # ---------------------------------------------------------------------
        add_executable(ScriptComponentSmokeTest ScriptComponentSmokeTest.cpp)

        set_target_properties(ScriptComponentSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        if(TARGET ZRuntime_ZHeaderTool)
            add_dependencies(ScriptComponentSmokeTest ZRuntime_ZHeaderTool)
        endif()

        # 测试要把它临时工程里的 Scripts/ 和 Intermediate/Scripts/ 铺满，需要知道
        # 本仓库里 .ts 和 tsc 产物在哪（也接受 --resource-dir 覆盖）。
        target_compile_definitions(ScriptComponentSmokeTest PRIVATE
            ZENGINE_SCRIPT_COMPONENT_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

        target_compile_definitions(ScriptComponentSmokeTest PRIVATE PAPI_QUICKJS=1)

        target_link_libraries(ScriptComponentSmokeTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(ScriptComponentSmokeTest PRIVATE ZEnginePCH)
            target_include_directories(ScriptComponentSmokeTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(ScriptComponentSmokeTest ZEnginePCH)
            target_precompile_headers(ScriptComponentSmokeTest REUSE_FROM ZEnginePCH)
        endif()

        if(APPLE)
            target_sources(ScriptComponentSmokeTest PRIVATE
                ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
        endif()

        if(MSVC)
            target_compile_options(ScriptComponentSmokeTest PRIVATE /W4)
        endif()

        add_test(NAME ScriptComponentSmokeTest COMMAND ScriptComponentSmokeTest)
        message(STATUS "Configured TypeScriptComponent smoke test: ScriptComponentSmokeTest")

        # ---------------------------------------------------------------------
        # TypeScriptComponent 的 prefab / zasset 反序列化。
        #
        # ScriptComponentSmokeTest 走的是**运行时挂载**（AddComponentByTypeName
        # -> SetScriptGuid）。用户真正拿到一个带脚本的 GameObject 绝大多数时候
        # 是从**资产文件**里反序列化出来的，那条路多出三件事：
        #
        #   1. m_ScriptGuid / m_ClassName / m_SerializedFields 穿过磁盘往返；
        #   2. 还原出来的 guid 能不能被 PostLoadResource 用来绑上 ——
        #      PrefabPostLoadDriver 沿 Transform.m_Children DFS 下发；
        #   3. Inspector override 能不能跟着还原并在 ApplySerializedFields 里
        #      生效（HostedMover 的类默认 speed 是 2，测试改成 7，看一帧走
        #      3.5 还是 1.0）。
        #
        # 资产两轨：editor 的 `.prefab`（YAML 对象图，扩展名即权威）、真机 /
        # legacy 的 `.zasset`（二进制 SerializedFile）。分流点是
        # AssetManager::IsYamlAuthoringAssetPath()。两轨各测一遍 + 一致性。
        #
        # 额外要用到的类型：PrefabAsset / PrefabRefComponent /
        # RigidbodyComponent —— PrefabUtility.cpp 里的 TypeOf<PrefabAsset> /
        # Cast<PrefabRefComponent> / Cast<RigidBodyComponent> 要它们的
        # TypeContainer<T>::m_Type。同样由 /WHOLEARCHIVE 带进来。
        # ---------------------------------------------------------------------
        add_executable(ScriptComponentPrefabTest ScriptComponentPrefabTest.cpp)

        set_target_properties(ScriptComponentPrefabTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        if(TARGET ZRuntime_ZHeaderTool)
            add_dependencies(ScriptComponentPrefabTest ZRuntime_ZHeaderTool)
        endif()

        target_compile_definitions(ScriptComponentPrefabTest PRIVATE
            ZENGINE_SCRIPT_COMPONENT_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

        target_compile_definitions(ScriptComponentPrefabTest PRIVATE PAPI_QUICKJS=1)

        target_link_libraries(ScriptComponentPrefabTest PRIVATE ZRuntime)

        if(TARGET ZEnginePCH)
            target_link_libraries(ScriptComponentPrefabTest PRIVATE ZEnginePCH)
            target_include_directories(ScriptComponentPrefabTest PRIVATE ${ENGINE_ROOT_DIR}/Source/CommonPCH)
            add_dependencies(ScriptComponentPrefabTest ZEnginePCH)
            target_precompile_headers(ScriptComponentPrefabTest REUSE_FROM ZEnginePCH)
        endif()

        if(APPLE)
            target_sources(ScriptComponentPrefabTest PRIVATE
                ${ENGINE_ROOT_DIR}/Source/Runtime/Core/Serialize/Test/SmokeTestAppleLinkStubs.cpp)
        endif()

        if(MSVC)
            target_compile_options(ScriptComponentPrefabTest PRIVATE /W4)
        endif()

        add_test(NAME ScriptComponentPrefabTest COMMAND ScriptComponentPrefabTest)
        message(STATUS "Configured TypeScriptComponent prefab/zasset smoke test: ScriptComponentPrefabTest")
    endif()
else()
    message(STATUS "ScriptVMSmokeTest skipped: requires -DPAPI_TYPE=quickjs (current: ${PAPI_TYPE})")
endif()
