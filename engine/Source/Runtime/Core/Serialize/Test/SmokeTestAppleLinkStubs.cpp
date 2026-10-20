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
ZHT_APPLE_LINK_STUB(3, "__ZN14RenderResource20UpdatePerFrameBufferENSt3__110shared_ptrI11RenderSceneEENS1_I12RenderCameraEE")
ZHT_APPLE_LINK_STUB(4, "__ZN14RenderResource25CreateAndMapStorageBufferEP3RHI")
ZHT_APPLE_LINK_STUB(5, "__ZN14RenderResource26UploadGlobalRenderResourceEP3RHI17LevelResourceDesc")
ZHT_APPLE_LINK_STUB(6, "__ZN14RenderResource30UploadGameObjectRenderResourceEP3RHI12RenderEntity14RenderMeshData")
ZHT_APPLE_LINK_STUB(7, "__ZN14RenderResource30UploadGameObjectRenderResourceEP3RHI12RenderEntity14RenderMeshData18RenderMaterialData")
ZHT_APPLE_LINK_STUB(8, "__ZN14RenderResource30UploadGameObjectRenderResourceEP3RHI12RenderEntity18RenderMaterialData")
ZHT_APPLE_LINK_STUB(9, "__ZN14ShaderCompiler17CompileFromSourceERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE11ShaderStageS8_RKNS0_6vectorIS6_NS4_IS6_EEEERKNS0_3mapIS6_S6_NS0_4lessIS6_EENS4_INS0_4pairIS7_S6_EEEEEE")
ZHT_APPLE_LINK_STUB(10, "__ZN14ShaderCompilerC1Ev")
ZHT_APPLE_LINK_STUB(11, "__ZN14ShaderCompilerD1Ev")
ZHT_APPLE_LINK_STUB(12, "__ZN26MainCameraPassShaderCommon13IsSkyMaterialERK14GpuPBRMaterial")
ZHT_APPLE_LINK_STUB(13, "__ZN6UIPass25InitializeUIRenderBackendEP8WindowUI")
ZHT_APPLE_LINK_STUB(14, "__ZN6UIPass4DrawEv")

    #undef ZHT_APPLE_LINK_STUB

#endif  // __APPLE__
