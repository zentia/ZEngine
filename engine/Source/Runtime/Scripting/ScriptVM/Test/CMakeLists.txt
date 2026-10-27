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
# Why the .gen.cpp files are listed as sources
# --------------------------------------------
# The static registrars (ZClassScriptBindingRegistrar) live in
# <build>/Generated/ZHT/ZRuntime/Src/<Name>.gen.cpp and hook themselves into
# ScriptBindingRegistry's intrusive list during static init. A linker only pulls
# an archive member in to satisfy an undefined symbol, and nothing references
# these registrars, so linking ZRuntime alone yields an EMPTY class table --
# InstallGlobals then publishes nothing and every global is `undefined`.
# The fix is to compile the handful of .gen.cpp files this test cares about
# directly into the executable.
#
# They live in ZRuntime too. That is normally fine: the archive member is only
# pulled when something still needs a symbol from it, and what these files
# contribute to the outside world is an anonymous-namespace registrar (internal
# linkage). If your linker nevertheless reports duplicate definitions, link
# ZRuntimeShared instead of ZRuntime for this target.
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

        # 被测的四个类。加进来的是"要在脚本里用到的类"，不是全部 58 个 —— 全加
        # 只会把链接面拉大，而这里要证明的是链路通不通，不是覆盖面。
        set(ZHT_GEN_SRC "${CMAKE_BINARY_DIR}/Generated/ZHT/ZRuntime/Src")
        set(_zht_bind_sources
            "${ZHT_GEN_SRC}/ScriptMath.gen.cpp"
            "${ZHT_GEN_SRC}/ScriptTime.gen.cpp"
            "${ZHT_GEN_SRC}/ScriptInput.gen.cpp"
            "${ZHT_GEN_SRC}/KeyCodes.gen.cpp"
        )
        set_source_files_properties(${_zht_bind_sources} PROPERTIES GENERATED TRUE)
        target_sources(ScriptVMSmokeTest PRIVATE ${_zht_bind_sources})
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

        set_source_files_properties(${_zht_bind_sources} PROPERTIES GENERATED TRUE)
        target_sources(ScriptTsChainSmokeTest PRIVATE ${_zht_bind_sources})
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
        # 除了脚本绑定用的 4 个 .gen.cpp，这里还要把 GameObject / Transform /
        # Behaviour / TypeScriptComponent 的 .gen.cpp 编进来：AddComponentBy
        # TypeName 和 Cast<Component> 都走 TypeManager 的类型表，而类型表正是
        # IMPLEMENT_REGISTER_CLASS 的静态注册器填的。Behaviour 必须带（TypeScript
        # Component 的直接基类），否则 Builder 走不到 Component，Cast<Component>
        # 的范围测试落空。
        # ---------------------------------------------------------------------
        add_executable(ScriptComponentSmokeTest ScriptComponentSmokeTest.cpp)

        set_target_properties(ScriptComponentSmokeTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        set(_zht_component_gen_sources
            "${ZHT_GEN_SRC}/GameObject.gen.cpp"
            "${ZHT_GEN_SRC}/Transform.gen.cpp"
            "${ZHT_GEN_SRC}/Behaviour.gen.cpp"
            "${ZHT_GEN_SRC}/TypeScriptComponent.gen.cpp"
        )
        set_source_files_properties(${_zht_bind_sources} ${_zht_component_gen_sources}
                                    PROPERTIES GENERATED TRUE)
        target_sources(ScriptComponentSmokeTest PRIVATE
            ${_zht_bind_sources} ${_zht_component_gen_sources})
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
        # 额外要编进来的 .gen.cpp：PrefabAsset / PrefabRefComponent /
        # RigidbodyComponent —— PrefabUtility.cpp 里的 TypeOf<PrefabAsset> /
        # Cast<PrefabRefComponent> / Cast<RigidBodyComponent> 要它们的
        # TypeContainer<T>::m_Type。
        # ---------------------------------------------------------------------
        add_executable(ScriptComponentPrefabTest ScriptComponentPrefabTest.cpp)

        set_target_properties(ScriptComponentPrefabTest PROPERTIES
            CXX_STANDARD 23
            FOLDER "Engine/Tests"
        )

        set(_zht_prefab_gen_sources
            "${ZHT_GEN_SRC}/GameObject.gen.cpp"
            "${ZHT_GEN_SRC}/Transform.gen.cpp"
            "${ZHT_GEN_SRC}/Behaviour.gen.cpp"
            "${ZHT_GEN_SRC}/TypeScriptComponent.gen.cpp"
            "${ZHT_GEN_SRC}/PrefabAsset.gen.cpp"
            "${ZHT_GEN_SRC}/PrefabRefComponent.gen.cpp"
            "${ZHT_GEN_SRC}/RigidbodyComponent.gen.cpp"
        )
        set_source_files_properties(${_zht_bind_sources} ${_zht_prefab_gen_sources}
                                    PROPERTIES GENERATED TRUE)
        target_sources(ScriptComponentPrefabTest PRIVATE
            ${_zht_bind_sources} ${_zht_prefab_gen_sources})
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
