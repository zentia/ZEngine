// =============================================================================
// BqLog Web stubs.
// -----------------------------------------------------------------------------
// On Emscripten/Web we treat BqLog as an INTERFACE-only target (see
// engine/3rdparty/CMakeLists.txt). The library is not compiled because it
// uses POSIX backtrace, platform-specific inline assembly, and other native
// host APIs that are not available in the browser sandbox.
//
// However, the engine's logging facade (LogSystem -> bq::engine_log inherits
// from bq::category_log -> bq::log) inlines calls to a handful of BqLog C
// API entry points through the headers we still expose.
//
// Two distinct groups of symbols need stubs:
//
// A) Per-log-call inlines (referenced by every TU that uses LOG_*):
//        bq::api::__api_log_write_begin
//        bq::api::__api_log_write_finish
//        bq::api::__api_get_stack_trace
//        bq::api::__api_get_stack_trace_utf16
//
// B) Per-log-construction inlines (referenced by log_system.cpp because
//    bq::engine_log::create_log -> bq::log::get_log_by_id calls these):
//        bq::api::__api_create_log
//        bq::api::__api_get_log_name_by_id
//        bq::api::__api_get_log_categories_count
//        bq::api::__api_get_log_category_name_by_index
//        bq::api::__api_get_log_merged_log_level_bitmap_by_log_id
//        bq::api::__api_get_log_print_stack_level_bitmap_by_log_id
//        bq::api::__api_get_log_category_masks_array_by_log_id
//
// Without group (B) the wasm-ld step fails with "undefined symbol: __api_*".
//
// Runtime safety strategy
// -----------------------
// bq::log::is_enable_for(category, level) does:
//     ((*merged_log_level_bitmap_ & (1U << level)) != 0)
//      && categories_mask_array_[category]
// before any other API call. To avoid null-deref crashes we hand back
// pointers to static, all-zero bitmaps. With every level bit cleared,
// is_enable_for() always returns false and do_log() short-circuits before
// touching __api_log_write_begin/finish at all. Stack-trace bitmap is
// likewise zeroed so should_print_stack stays false.
//
// Net effect: logging is silently disabled on Web. This is acceptable for
// initial bring-up; a follow-up can route BqLog output to
// emscripten_console_* without changing this file's contract.
//
// IMPORTANT: This file MUST be in the Platform/Web auto-glob (see
// engine/Source/Runtime/CMakeLists.txt::PLATFORM_SUBDIR) so it always lands
// inside libZRuntime.a on Emscripten builds and can satisfy the symbol
// references coming from any executable that links to ZRuntime.
// =============================================================================

#if defined(__EMSCRIPTEN__)

    #include "bq_common/BqCommonPublicInclude.h"
    #include "bq_log/Misc/BqLogDef.h"

    #include <cstdint>
    #include <cstdio>
    #include <cstring>

namespace bq
{
    namespace api
    {

        // ---------------------------------------------------------------------------
        // Group A: per-log-call symbols.
        // ---------------------------------------------------------------------------

        // Returns a "successful" handle that points at a static throw-away buffer.
        // In practice this is never reached because is_enable_for() returns false
        // (see Group B bitmaps), but the linker still needs the symbol.
        BQ_API _api_log_write_handle __api_log_write_begin(uint64_t /*log_id*/,
                                                           uint8_t /*log_level*/,
                                                           uint32_t /*category_index*/,
                                                           uint8_t /*format_string_type*/,
                                                           uint32_t /*format_str_bytes_len*/,
                                                           const void* /*format_str_data*/,
                                                           uint32_t /*args_data_bytes_len*/)
        {
            static thread_local uint8_t scratch[64 * 1024];

            _api_log_write_handle handle {};
            handle.format_data_addr = scratch;
            handle.result = enum_buffer_result_code::success;
            return handle;
        }

        BQ_API void __api_log_write_finish(uint64_t /*log_id*/, _api_log_write_handle /*write_handle*/) {}

        BQ_API void __api_get_stack_trace(_api_string_def* out_name_ptr, uint32_t /*skip_frame_count*/)
        {
            if (out_name_ptr != nullptr)
            {
                out_name_ptr->str = "";
                out_name_ptr->len = 0;
            }
        }

        BQ_API void __api_get_stack_trace_utf16(_api_u16string_def* out_name_ptr, uint32_t /*skip_frame_count*/)
        {
            if (out_name_ptr != nullptr)
            {
                out_name_ptr->str = nullptr;
                out_name_ptr->len = 0;
            }
        }

        // ---------------------------------------------------------------------------
        // Group B: per-log-construction symbols.
        // ---------------------------------------------------------------------------

        // Singleton fake-log identifier returned by __api_create_log. Any non-zero
        // value is fine — bq::log treats 0 as "not found" and bails. We keep it
        // constant so subsequent get_log_*_by_id calls can be matched against it.
        namespace
        {
            constexpr uint64_t kStubLogId = 0xB91091CEB91091CEull;

            // All bits cleared: is_enable_for() never returns true → do_log() short
            // circuits before any side-effecting API call.
            constexpr uint32_t kZeroMergedLevelBitmap = 0;
            constexpr uint32_t kZeroPrintStackLevelBitmap = 0;

            // One byte per category. 1 byte is enough for the engine's single
            // "engine" category; the engine never indexes past it.
            constexpr uint8_t kZeroCategoryMaskArray[1] = {0};
        }  // namespace

        BQ_API uint64_t __api_create_log(const char* /*log_name_utf8*/,
                                         const char* /*config_content_utf8*/,
                                         uint32_t /*category_count*/,
                                         const char* const* /*category_names_array_utf8*/)
        {
            return kStubLogId;
        }

        BQ_API bool __api_get_log_name_by_id(uint64_t log_id, _api_string_def* name_ptr)
        {
            if (log_id != kStubLogId || name_ptr == nullptr)
            {
                return false;
            }
            static const char* kEngineLogName = "engine";
            name_ptr->str = kEngineLogName;
            name_ptr->len = static_cast<uint32_t>(std::strlen(kEngineLogName));
            return true;
        }

        BQ_API uint32_t __api_get_log_categories_count(uint64_t /*log_id*/)
        {
            // bq::engine_log inherits from bq::category_log<bq::engine_log_category>.
            // The engine config string passed to create_log declares no extra
            // category names, so the implicit single "default" category is enough.
            return 1u;
        }

        BQ_API bool __api_get_log_category_name_by_index(uint64_t log_id,
                                                         uint32_t category_index,
                                                         _api_string_def* category_name_ptr)
        {
            if (log_id != kStubLogId || category_name_ptr == nullptr || category_index != 0)
            {
                return false;
            }
            static const char* kDefaultCategory = "default";
            category_name_ptr->str = kDefaultCategory;
            category_name_ptr->len = static_cast<uint32_t>(std::strlen(kDefaultCategory));
            return true;
        }

        BQ_API const uint32_t* __api_get_log_merged_log_level_bitmap_by_log_id(uint64_t /*log_id*/)
        {
            // Returning a non-null pointer keeps is_enable_for from null-derefing.
            // The pointed-to bitmap is all-zero so every level evaluates as
            // disabled — see file header for why this is the intended behavior.
            return &kZeroMergedLevelBitmap;
        }

        BQ_API const uint32_t* __api_get_log_print_stack_level_bitmap_by_log_id(uint64_t /*log_id*/)
        {
            return &kZeroPrintStackLevelBitmap;
        }

        BQ_API const uint8_t* __api_get_log_category_masks_array_by_log_id(uint64_t /*log_id*/)
        {
            // is_enable_for indexes this with category_index. Returning a 1-byte
            // all-zero array is safe for the single "default" category and also
            // produces "disabled" for the masking AND-test.
            return kZeroCategoryMaskArray;
        }

    }  // namespace api
}  // namespace bq

#endif  // __EMSCRIPTEN__
