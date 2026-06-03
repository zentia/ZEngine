// =============================================================================
// iOS_LibCxxPolyfill.cpp
//
// PURPOSE
//   Provide local strong-symbol definitions for two libc++ entry points whose
//   on-device implementation is missing on early iOS 16 minor versions
//   (16.0 / 16.1 / 16.2 / 16.3). These symbols are declared "available since
//   iOS 16.0" by the SDK 16.4 TBD file, but Apple shipped iOS 16.0 with a
//   `/usr/lib/libc++.1.dylib` that does NOT actually export them. dyld then
//   fails dlopen() of any .framework that references them:
//
//       Symbol not found:
//           __ZNSt3__113basic_filebufIcNS_11char_traitsIcEEE4openEPKcj
//           ( std::__1::basic_filebuf<char>::open(const char*, openmode) )
//       Referenced from: ZRuntimeShared
//       Expected in:     /usr/lib/libc++.1.dylib
//
//   The same risk exists for `basic_stringbuf<char>::str() const`, which
//   ZRuntimeShared.framework also references and which has identical
//   availability issues on early iOS 16.
//
// SOLUTION
//   Force an explicit instantiation of the full `basic_filebuf<char>` and
//   `basic_stringbuf<char>` class templates inside this translation unit. That
//   instructs the compiler to emit *strong* (T) definitions of every
//   non-inline member function for these specializations into our own
//   framework. At link time, the framework satisfies its own out-of-line
//   references locally, so dyld no longer queries the system libc++ for
//   them at load time.
//
//   The implementation bodies come straight out of the iOS 16.4 SDK headers
//   (<fstream>, <sstream>) — they are templated `out-of-line` definitions
//   that are perfectly valid to instantiate from any client TU and have no
//   private (libc++ internal) dependencies beyond what the headers expose.
//
// SCOPE
//   Compiled only for iOS (engine/Source/Platform/IOS/ is auto-collected by
//   engine/Source/Runtime/CMakeLists.txt — see PLATFORM_SUBDIR logic).
//   Other platforms never see this file and never carry the extra ~290
//   instantiated symbols. Symbol size impact on the final framework is on
//   the order of a few kilobytes.
//
// SAFETY
//   - The instantiated bodies are byte-identical to what libc++ would have
//     emitted, so there is no ABI divergence vs. the system implementation.
//   - On iOS versions where the system libc++ DOES export these symbols,
//     ld64 still binds the framework's references to *our* local strong
//     definitions (default visibility lookup is per-image, and same-image
//     definitions win), so no clash and no behavioral difference.
//   - Default symbol visibility is preserved (NOT hidden) intentionally:
//     the goal is to satisfy the framework's own undefined references, not
//     to re-export to consumers. Re-export is harmless because the
//     mangled names are universal libc++ entry points.
//
// MAINTAINER NOTE
//   If a future iOS deployment-target raise drops support for iOS 16.0–16.3,
//   this polyfill becomes redundant and the file can be deleted. Until then
//   keep it here; removing it will reintroduce the dlopen failure on those
//   devices.
// =============================================================================

#include <fstream>
#include <sstream>

// Explicit instantiation definitions. These force every non-inline member of
// the listed specializations to be emitted as a strong symbol in this TU.
// `std::__1` is libc++'s inline namespace; the same mangling reaches outside
// users via the standard `std::` aliases.
template class std::__1::basic_filebuf<char, std::__1::char_traits<char>>;
template class std::__1::basic_stringbuf<char,
                                         std::__1::char_traits<char>,
                                         std::__1::allocator<char>>;
