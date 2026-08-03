// __chkstk_darwin — stack probing stub for arm64 iOS
//
// Clang on Apple arm64 inserts calls to ___chkstk_darwin for functions whose
// stack frame exceeds one page (16 KB). On macOS this symbol lives in
// libSystem.B.dylib, but on iOS the symbol is NOT re-exported in the SDK
// stubs (.tbd), so dyld fails with:
//   Symbol not found: ___chkstk_darwin
//   Expected in: /usr/lib/libSystem.B.dylib
//
// NOTE: Apple platforms prepend an extra '_' to C symbol names. The linker
// references "___chkstk_darwin" (3 underscores), so the C source must use
// "__chkstk_darwin" (2 underscores). The compiler will emit the third '_'.
//
// On iOS the kernel uses guard pages to detect stack overflow, so explicit
// probing is unnecessary — a simple return is sufficient.

extern "C" {

__attribute__((naked)) void __chkstk_darwin(void) {
    __asm__ volatile("ret");
}

} // extern "C"
