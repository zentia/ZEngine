// =====================================================================
// Apple-only link stubs for the Core/Serialize smoke tests.
// ---------------------------------------------------------------------
// libZRuntime.a on macOS currently references render-side symbols whose
// defining TUs are not built for Apple (RenderSystem::GetFrameDrawList /
// Vulkan ShaderCompiler / ...). Every consumer links ZRuntime with
// -force_load, so those references must resolve even though the dump
// tests never touch the renderer. `-undefined dynamic_lookup` is not
// enough: some of them sit in vtables and are bound eagerly by dyld.
//
// Each stub is bound to the exact mangled name via an asm label and
// aborts if ever reached. Only compiled into the smoke-test targets on
// APPLE; carved out of ZRuntime in Runtime/CMakeLists.txt.
// =====================================================================

#if defined(__APPLE__)

    #include <cstdio>
    #include <cstdlib>

    #define ZHT_APPLE_LINK_STUB(ID, MANGLED)                                                   \
        extern "C" [[noreturn]] void ZhtAppleLinkStub_##ID() __asm__(MANGLED);                 \
        extern "C" [[noreturn]] void ZhtAppleLinkStub_##ID()                                   \
        {                                                                                      \
            std::fprintf(stderr, "smoke test: render stub reached: %s\n", MANGLED); \
            std::abort();                                                                      \
        }

ZHT_APPLE_LINK_STUB(0, "__ZN12RenderSystem16GetFrameDrawListEj")
// 1–8 已移除：RenderResource 的 GetEntityMesh / GetEntityMaterial /
// CreateAndMapStorageBuffer / UploadGlobalRenderResource /
// UploadGameObjectRenderResource ×3 —— 它们以前住在 `RenderResource.cpp` 那个
// `#if defined(Z_HAS_VULKAN)` 块里，Apple 上编译不到所以只能桩。
//
// 现在那个块的守卫是 `Z_HAS_VULKAN || __APPLE__`（Vulkan 专属的 VMA 分配换成
// 了 CreateAssetBuffer helper，Metal 走通用 CreateBuffer），Apple 上这些都有真
// 实现了。再留桩会和 libZRuntime.a 里的真符号撞重复定义。
// 3 更早一步就已经移除（UpdatePerFrameBuffer 合并成 RenderResourcePerFrame.cpp）。
ZHT_APPLE_LINK_STUB(9, "__ZN14ShaderCompiler17CompileFromSourceERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE11ShaderStageS8_RKNS0_6vectorIS6_NS4_IS6_EEEERKNS0_3mapIS6_S6_NS0_4lessIS6_EENS4_INS0_4pairIS7_S6_EEEEEE")
ZHT_APPLE_LINK_STUB(10, "__ZN14ShaderCompilerC1Ev")
ZHT_APPLE_LINK_STUB(11, "__ZN14ShaderCompilerD1Ev")
// 12/13/14 已移除：MainCameraPassShaderCommon::IsSkyMaterial、UIPass::Draw、
// UIPass::InitializeUIRenderBackend。它们所在的 TU 以前被 Apple 构建整目录排除，
// 现在 Passes/ 改成精确排除后这三个都有真实现了，再留桩会撞重复定义。
// （15 及以后若再有，同理：Apple 上能真编的就不要再桩。）

    #undef ZHT_APPLE_LINK_STUB

#endif  // __APPLE__
