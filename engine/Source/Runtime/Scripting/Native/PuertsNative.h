// PuertsNative.h -- C entrypoint surface for ZRuntimeShared.framework.
//
// This header is part of the *public* framework API: external code (Unity
// plugin code, sample apps, etc.) does
//
//     #include <ZRuntimeShared/PuertsNative.h>
//
// and must compile without any of the engine's internal include paths
// (Runtime/, 3rdparty/puerts/unity/...) being visible. Therefore this header
// MUST NOT include `Log.h`, `pesapi.h`, or anything else from inside the
// engine source tree -- all transitive includes the previous revision pulled
// in (which forced external code to ship matching copies of those headers,
// and produced thousands of "file not found" errors when those copies were
// absent) have been replaced with self-contained forward declarations.
//
// Only the opaque pointer typedefs and function-pointer typedefs that the
// exported function signatures actually mention are needed here. All pesapi_*
// types are forward-declared as `struct foo__*` -- identical to how pesapi.h
// itself defines them -- so callers only need the pointer identity to make
// calls, not the full struct layouts.

#pragma once

#include <cstddef>
#include <cstdint>

#if __has_include("Runtime/ExportRuntime.h")
#include "Runtime/ExportRuntime.h"
#elif __has_include("ExportRuntime.h")
#include "ExportRuntime.h"
#else
// Keep this public header usable in isolation. Normal engine and framework
// builds include ExportRuntime.h through one of the branches above.
#define ZRUNTIME_PUERTS_LOCAL_EXPORT_RUNTIME
#if defined(_WIN32) && defined(ZRUNTIME_SHARED_IMPORTS)
#define EXPORT_RUNTIME extern "C" __declspec(dllimport)
#elif defined(_WIN32) && defined(ZRUNTIME_SHARED_EXPORTS)
#define EXPORT_RUNTIME extern "C" __declspec(dllexport)
#elif defined(ZRUNTIME_SHARED_EXPORTS)
#define EXPORT_RUNTIME extern "C" __attribute__((visibility("default")))
#else
#define EXPORT_RUNTIME extern "C"
#endif
#endif

// Public pesapi types used by the exported signatures. These declarations
// intentionally match pesapi.h. Repeating an identical typedef is legal, so
// this remains valid regardless of whether pesapi.h is included before or
// after this header. Only pesapi_scope_memory is forward-declared because the
// API below receives it by pointer; defining it here would conflict with the
// complete definition in pesapi.h.
typedef void (*LogCallback)(const char* value);
typedef struct pesapi_env__* pesapi_env;
typedef struct pesapi_env_ref__* pesapi_env_ref;
typedef struct pesapi_value__* pesapi_value;
typedef struct pesapi_value_ref__* pesapi_value_ref;
typedef struct pesapi_callback_info__* pesapi_callback_info;
typedef struct pesapi_scope__* pesapi_scope;
typedef struct pesapi_registry__* pesapi_registry;

struct pesapi_ffi;
struct pesapi_scope_memory;

typedef void (*pesapi_callback)(struct pesapi_ffi* apis, pesapi_callback_info info);
typedef void (*pesapi_function_finalize)(struct pesapi_ffi* apis, void* data, void* env_private);

// ---------------------------------------------------------------------------
// Backend activation / version query.
// ---------------------------------------------------------------------------
EXPORT_RUNTIME int GetPapiVersion();
EXPORT_RUNTIME void* GetRegisterApi();

// Routes the scripting backend's internal log stream to caller-supplied
// sinks. LogCallback is declared above.
EXPORT_RUNTIME void SetLogCallback(LogCallback Log, LogCallback LogWarning, LogCallback LogError);

// ---------------------------------------------------------------------------
// Per-backend FFI / env-ref entry points.
//
// Only the entries for the active PAPI backend are implemented at link time
// (the iOS build script filters the exported-symbols list accordingly via
// `PAPI_KEEP_BACKEND`). The other declarations remain so that the header is
// usable from external code that wants to be backend-agnostic at compile
// time and only resolves the symbols it needs at runtime.
// ---------------------------------------------------------------------------
EXPORT_RUNTIME int GetLuaPapiVersion();
EXPORT_RUNTIME pesapi_ffi* GetLuaFFIApi();
EXPORT_RUNTIME pesapi_env_ref CreateLuaPapiEnvRef();
EXPORT_RUNTIME void DestroyLuaPapiEnvRef(pesapi_env_ref env_ref);

EXPORT_RUNTIME int GetQjsPapiVersion();
EXPORT_RUNTIME pesapi_ffi* GetQjsFFIApi();
EXPORT_RUNTIME pesapi_env_ref CreateQjsPapiEnvRef();
EXPORT_RUNTIME void DestroyQjsPapiEnvRef(pesapi_env_ref env_ref);

// V8 entry points (mirror of the QuickJS quartet above). Implemented by
// puerts' papi-v8 static library (see engine/3rdparty/puerts/unity/native/
// papi-v8/source/PapiExport.cpp). Only resolved when ZEngine is configured
// with -DPAPI_TYPE=v8, which links the PapiV8 target. On other backends
// these symbols are simply not referenced -- BackendV8 is gated by the
// PAPI_V8 macro the build system sets when v8 is selected.
EXPORT_RUNTIME int GetV8PapiVersion();
EXPORT_RUNTIME pesapi_ffi* GetV8FFIApi();
EXPORT_RUNTIME pesapi_env_ref CreateV8PapiEnvRef();
EXPORT_RUNTIME void DestroyV8PapiEnvRef(pesapi_env_ref env_ref);

// ---------------------------------------------------------------------------
// Re-exported pesapi C entry points (vtable-style flat C API).
//
// These mirror the function-pointer fields of `struct pesapi_ffi` so that
// callers that link against ZRuntimeShared directly -- without first
// calling GetRegisterApi() to obtain an FFI table -- can still reach the
// full pesapi surface. Each one takes a `pesapi_ffi*` that the caller has
// obtained from one of the Get*FFIApi() entry points above.
// ---------------------------------------------------------------------------
EXPORT_RUNTIME pesapi_value pesapi_create_null(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME pesapi_value pesapi_create_undefined(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME pesapi_value pesapi_create_boolean(struct pesapi_ffi* apis, pesapi_env env, int value);
EXPORT_RUNTIME pesapi_value pesapi_create_int32(struct pesapi_ffi* apis, pesapi_env env, int32_t value);
EXPORT_RUNTIME pesapi_value pesapi_create_uint32(struct pesapi_ffi* apis, pesapi_env env, uint32_t value);
EXPORT_RUNTIME pesapi_value pesapi_create_int64(struct pesapi_ffi* apis, pesapi_env env, int64_t value);
EXPORT_RUNTIME pesapi_value pesapi_create_uint64(struct pesapi_ffi* apis, pesapi_env env, uint64_t value);
EXPORT_RUNTIME pesapi_value pesapi_create_double(struct pesapi_ffi* apis, pesapi_env env, double value);
EXPORT_RUNTIME pesapi_value pesapi_create_string_utf8(struct pesapi_ffi* apis, pesapi_env env, const char* str, size_t length);
EXPORT_RUNTIME pesapi_value pesapi_create_string_utf16(struct pesapi_ffi* apis, pesapi_env env, const uint16_t* str, size_t length);
EXPORT_RUNTIME pesapi_value pesapi_create_binary(struct pesapi_ffi* apis, pesapi_env env, void* data, size_t length);
EXPORT_RUNTIME pesapi_value pesapi_create_binary_by_value(struct pesapi_ffi* apis, pesapi_env env, void* data, size_t length);
EXPORT_RUNTIME pesapi_value pesapi_create_array(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME pesapi_value pesapi_create_object(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME pesapi_value pesapi_create_function(struct pesapi_ffi* apis, pesapi_env env, pesapi_callback native_impl, void* data, pesapi_function_finalize finalize);
EXPORT_RUNTIME pesapi_value pesapi_create_class(struct pesapi_ffi* apis, pesapi_env env, const void* type_id);
EXPORT_RUNTIME int pesapi_get_value_bool(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int32_t pesapi_get_value_int32(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME uint32_t pesapi_get_value_uint32(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int64_t pesapi_get_value_int64(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME uint64_t pesapi_get_value_uint64(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME double pesapi_get_value_double(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME const char* pesapi_get_value_string_utf8(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value, char* buf, size_t* bufsize);
EXPORT_RUNTIME const uint16_t* pesapi_get_value_string_utf16(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value, uint16_t* buf, size_t* bufsize);
EXPORT_RUNTIME void* pesapi_get_value_binary(struct pesapi_ffi* apis, pesapi_env env, pesapi_value pvalue, size_t* bufsize);
EXPORT_RUNTIME uint32_t pesapi_get_array_length(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_null(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_undefined(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_boolean(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_int32(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_uint32(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_int64(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_uint64(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_double(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_string(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_object(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_function(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_binary(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_array(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME pesapi_value pesapi_native_object_to_value(struct pesapi_ffi* apis, pesapi_env env, const void* type_id, void* object_ptr, int call_finalize);
EXPORT_RUNTIME void* pesapi_get_native_object_ptr(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME const void* pesapi_get_native_object_typeid(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_instance_of(struct pesapi_ffi* apis, pesapi_env env, const void* type_id, pesapi_value value);
EXPORT_RUNTIME pesapi_value pesapi_boxing(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME pesapi_value pesapi_unboxing(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME void pesapi_update_boxed_value(struct pesapi_ffi* apis, pesapi_env env, pesapi_value boxed_value, pesapi_value value);
EXPORT_RUNTIME int pesapi_is_boxed_value(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value);
EXPORT_RUNTIME int pesapi_get_args_len(struct pesapi_ffi* apis, pesapi_callback_info info);
EXPORT_RUNTIME pesapi_value pesapi_get_arg(struct pesapi_ffi* apis, pesapi_callback_info info, int index);
EXPORT_RUNTIME pesapi_env pesapi_get_env(struct pesapi_ffi* apis, pesapi_callback_info info);
EXPORT_RUNTIME void* pesapi_get_native_holder_ptr(struct pesapi_ffi* apis, pesapi_callback_info info);
EXPORT_RUNTIME const void* pesapi_get_native_holder_typeid(struct pesapi_ffi* apis, pesapi_callback_info info);
EXPORT_RUNTIME void* pesapi_get_userdata(struct pesapi_ffi* apis, pesapi_callback_info info);
EXPORT_RUNTIME void pesapi_add_return(struct pesapi_ffi* apis, pesapi_callback_info info, pesapi_value value);
EXPORT_RUNTIME void pesapi_throw_by_string(struct pesapi_ffi* apis, pesapi_callback_info pinfo, const char* msg);
EXPORT_RUNTIME pesapi_env_ref pesapi_create_env_ref(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME int pesapi_env_ref_is_valid(struct pesapi_ffi* apis, pesapi_env_ref env);
EXPORT_RUNTIME pesapi_env pesapi_get_env_from_ref(struct pesapi_ffi* apis, pesapi_env_ref env_ref);
EXPORT_RUNTIME pesapi_env_ref pesapi_duplicate_env_ref(struct pesapi_ffi* apis, pesapi_env_ref env_ref);
EXPORT_RUNTIME void pesapi_release_env_ref(struct pesapi_ffi* apis, pesapi_env_ref env_ref);
EXPORT_RUNTIME pesapi_scope pesapi_open_scope(struct pesapi_ffi* apis, pesapi_env_ref env_ref);
EXPORT_RUNTIME pesapi_scope pesapi_open_scope_placement(struct pesapi_ffi* apis, pesapi_env_ref env_ref, struct pesapi_scope_memory* memory);
EXPORT_RUNTIME int pesapi_has_caught(struct pesapi_ffi* apis, pesapi_scope scope);
EXPORT_RUNTIME const char* pesapi_get_exception_as_string(struct pesapi_ffi* apis, pesapi_scope scope, int with_stack);
EXPORT_RUNTIME void pesapi_close_scope(struct pesapi_ffi* apis, pesapi_scope scope);
EXPORT_RUNTIME void pesapi_close_scope_placement(struct pesapi_ffi* apis, pesapi_scope scope);
EXPORT_RUNTIME pesapi_value_ref pesapi_create_value_ref(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value, uint32_t internal_field_count);
EXPORT_RUNTIME pesapi_value_ref pesapi_duplicate_value_ref(struct pesapi_ffi* apis, pesapi_value_ref value_ref);
EXPORT_RUNTIME void pesapi_release_value_ref(struct pesapi_ffi* apis, pesapi_value_ref value_ref);
EXPORT_RUNTIME pesapi_value pesapi_get_value_from_ref(struct pesapi_ffi* apis, pesapi_env env, pesapi_value_ref value_ref);
EXPORT_RUNTIME void pesapi_set_ref_weak(struct pesapi_ffi* apis, pesapi_env env, pesapi_value_ref value_ref);
EXPORT_RUNTIME int pesapi_set_owner(struct pesapi_ffi* apis, pesapi_env env, pesapi_value value, pesapi_value owner);
EXPORT_RUNTIME pesapi_env_ref pesapi_get_ref_associated_env(struct pesapi_ffi* apis, pesapi_value_ref value_ref);
EXPORT_RUNTIME void** pesapi_get_ref_internal_fields(struct pesapi_ffi* apis, pesapi_value_ref value_ref, uint32_t* pinternal_field_count);
EXPORT_RUNTIME pesapi_value pesapi_get_property(struct pesapi_ffi* apis, pesapi_env env, pesapi_value object, const char* key);
EXPORT_RUNTIME int pesapi_set_property(struct pesapi_ffi* apis, pesapi_env env, pesapi_value object, const char* key, pesapi_value value);
EXPORT_RUNTIME int pesapi_get_private(struct pesapi_ffi* apis, pesapi_env env, pesapi_value object, void** out_ptr);
EXPORT_RUNTIME int pesapi_set_private(struct pesapi_ffi* apis, pesapi_env env, pesapi_value object, void* ptr);
EXPORT_RUNTIME pesapi_value pesapi_get_property_uint32(struct pesapi_ffi* apis, pesapi_env env, pesapi_value object, uint32_t key);
EXPORT_RUNTIME int pesapi_set_property_uint32(struct pesapi_ffi* apis, pesapi_env env, pesapi_value object, uint32_t key, pesapi_value value);
EXPORT_RUNTIME pesapi_value pesapi_call_function(struct pesapi_ffi* apis, pesapi_env env, pesapi_value func, pesapi_value this_object, int argc, const pesapi_value argv[]);
EXPORT_RUNTIME pesapi_value pesapi_eval(struct pesapi_ffi* apis, pesapi_env env, const uint8_t* code, size_t code_size, const char* path);
EXPORT_RUNTIME pesapi_value pesapi_global(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME const void* pesapi_get_env_private(struct pesapi_ffi* apis, pesapi_env env);
EXPORT_RUNTIME void pesapi_set_env_private(struct pesapi_ffi* apis, pesapi_env env, const void* ptr);
EXPORT_RUNTIME void pesapi_set_registry(struct pesapi_ffi* apis, pesapi_env env, pesapi_registry registry);

#ifdef ZRUNTIME_PUERTS_LOCAL_EXPORT_RUNTIME
#undef EXPORT_RUNTIME
#undef ZRUNTIME_PUERTS_LOCAL_EXPORT_RUNTIME
#endif
