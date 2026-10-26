// Standalone harness prelude for compiling Nanite cluster-builder code under
// emcc WITHOUT linking ZRuntime.
//
// The full CMake build generates the serialization codegen types (JSONRead,
// YAMLWrite, ...) and the INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL macro via
// ObjectDefines.h. A bare standalone TU doesn't run that codegen, so the math
// type headers (Vector3/Vector4 -> SerializeUtility.h) leave the INSTANTIATE
// macro undefined and the out-of-line Transfer bodies reference undefined
// transfer types.
//
// The Nanite smoke test is PURE CPU (build hierarchy, check geometry bytes) --
// it never serializes. So this prelude satisfies the headers with:
//   1. the DECLARE_SERIALIZE macro (mirrors Core/Serialize/SerializeUtility.h)
//   2. an empty INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL (no-op; Transfer is
//      never instantiated by the smoke test)
//   3. stub transfer-function types so the out-of-line VectorN::Transfer bodies
//      compile against (they are never called).
//
// This header is NOT linked into the engine; it lives only in the throwaway
// verification harness under build-web/nanite_standalone/.

#pragma once

// (1) DECLARE_SERIALIZE -- mirrors Core/Serialize/SerializeUtility.h
#ifndef NANITE_STANDALONE_DECLARE_SERIALIZE
#define NANITE_STANDALONE_DECLARE_SERIALIZE(x)              \
    inline static const char* GetTypeString()               \
    {                                                       \
        return #x;                                          \
    }                                                       \
    inline static bool AllowTransferOptimization()          \
    {                                                       \
        return false;                                       \
    }                                                       \
    template <class TransferFunction>                       \
    void Transfer(TransferFunction& transfer);
#endif

// (2) no-op INSTANTIATE macros (ObjectDefines.h is not in the standalone chain)
#ifndef INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL
#define INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(FULL_TYPENAME_, DECL_, FUNCTION_NAME_, FUNCTION_RETURN_TYPE_)
#endif
#ifndef INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED
#define INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(FULL_TYPENAME_)
#endif

// (3) stub transfer-function types for the out-of-line Transfer bodies.
namespace nanite_stub {
    struct TransferFnBase {
        template <class T>
        void Transfer(T&, const char*) {}
        template <class T>
        void Transfer(T&, const char*, const char*) {}
    };
}  // namespace nanite_stub
