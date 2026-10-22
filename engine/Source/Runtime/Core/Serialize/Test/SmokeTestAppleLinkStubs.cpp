// =====================================================================
// Apple-only link stubs for the Core/Serialize smoke tests.
// ---------------------------------------------------------------------
// libZRuntime.a on macOS currently references 15 render-side symbols
// whose defining TUs are not built for Apple (RenderResource / UIPass /
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
ZHT_APPLE_LINK_STUB(1, "__ZN14RenderResource13GetEntityMeshE12RenderEntity")
ZHT_APPLE_LINK_STUB(2, "__ZN14RenderResource17GetEntityMaterialE12RenderEntity")
// 3 曾是 RenderResource::UpdatePerFrameBuffer —— 已移除：它以前只在 Vulkan /
// DX12 两个后端文件里有实现（都编译不到 Apple），现在合并成一份平台无关的
// `Function/Render/RenderResourcePerFrame.cpp`，Apple 上也有真定义了。再留桩会
// 和真符号撞重复定义。
// ZHT_APPLE_LINK_STUB(3, "__ZN14RenderResource20UpdatePerFrameBufferE...")
ZHT_APPLE_LINK_STUB(4, "__ZN14RenderResource25CreateAndMapStorageBufferEP3RHI")
ZHT_APPLE_LINK_STUB(5, "__ZN14RenderResource26UploadGlobalRenderResourceEP3RHI17LevelResourceDesc")
ZHT_APPLE_LINK_STUB(6, "__ZN14RenderResource30UploadGameObjectRenderResourceEP3RHI12RenderEntity14RenderMeshData")
ZHT_APPLE_LINK_STUB(7, "__ZN14RenderResource30UploadGameObjectRenderResourceEP3RHI12RenderEntity14RenderMeshData18RenderMaterialData")
ZHT_APPLE_LINK_STUB(8, "__ZN14RenderResource30UploadGameObjectRenderResourceEP3RHI12RenderEntity18RenderMaterialData")
ZHT_APPLE_LINK_STUB(9, "__ZN14ShaderCompiler17CompileFromSourceERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE11ShaderStageS8_RKNS0_6vectorIS6_NS4_IS6_EEEERKNS0_3mapIS6_S6_NS0_4lessIS6_EENS4_INS0_4pairIS7_S6_EEEEEE")
ZHT_APPLE_LINK_STUB(10, "__ZN14ShaderCompilerC1Ev")
ZHT_APPLE_LINK_STUB(11, "__ZN14ShaderCompilerD1Ev")
// 12/13/14 已移除：MainCameraPassShaderCommon::IsSkyMaterial、UIPass::Draw、
// UIPass::InitializeUIRenderBackend。它们所在的 TU 以前被 Apple 构建整目录排除，
// 现在 Passes/ 改成精确排除后这三个都有真实现了，再留桩会撞重复定义。
// （15 及以后若再有，同理：Apple 上能真编的就不要再桩。）

    #undef ZHT_APPLE_LINK_STUB

#endif  // __APPLE__
