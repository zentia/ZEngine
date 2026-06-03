#pragma once
#include "Log.h"
#include "Runtime/ExportRuntime.h"
#include "pesapi.h"

EXPORT_RUNTIME int GetPapiVersion();
EXPORT_RUNTIME void* GetRegisterApi();
EXPORT_RUNTIME void SetLogCallback(LogCallback Log, LogCallback LogWarning, LogCallback LogError);

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