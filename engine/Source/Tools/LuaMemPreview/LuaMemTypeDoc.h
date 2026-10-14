#pragma once

// ============================================================================
// LuaMemTypeDoc - what each type bucket actually counts
// ============================================================================
//
// Source of truth is lua_track_type_t in GameCore's
//   Client_proj/GameCore/External/lua-5.4.7/src/ltracker.h
// mirrored 1:1 by LuaMemoryTypeName() in
//   Source/Runtime/Profiler/Memory/Lua/LuaMemoryProfiler.h
//
// The wording below is deliberately about *what is included in the number*,
// not about what the Lua type is. Reading a bucket wrong is the main way to
// misread a snapshot, and two cases cause nearly all of it:
//
//   * `string` vs `string_table` - the first is string contents, the second is
//     only the intern hash table's bucket array. A big `string_table` with a
//     small `string` means many short-lived distinct strings, not large ones.
//   * `thread` holds global_State. It shares the main thread's malloc block
//     (struct LG, ltracker.h:28-29), so VM-global memory is *not* in
//     `vm_other` - a natural but wrong place to look for it.
//
// Since the 8 GC buckets were changed to report each object's FULL footprint
// (owned dynamic arrays included), `proto` covers bytecode + constants +
// debug info. That is what moved it from under 1 MB to the largest consumer
// in the reference snapshot, so the text says so explicitly.

#include <string>

namespace LuaMemPreview
{
// One-line summary shown next to the bar; always non-empty.
const char* TypeSummary(const std::string& type_name);

// Longer explanation for the hover card. May contain '\n'. Empty when the tag
// is unknown, in which case the caller should fall back to TypeSummary().
const char* TypeDetail(const std::string& type_name);

// True for buckets that are not GC objects (string_table, vm_other). The bar
// marks these, because "0 B" means something different for them: for a GC
// bucket it means no such objects, for vm_other it means the reconciliation
// found nothing unaccounted for - which is the healthy case.
bool IsNonGcBucket(const std::string& type_name);

}  // namespace LuaMemPreview
