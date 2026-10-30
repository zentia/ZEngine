#pragma once

// ============================================================================
// CrashHandler
// ----------------------------------------------------------------------------
// Windows SEH crash handler that writes a structured crash snapshot.
//
// Two artifacts are produced next to the exe:
//   * crash_stack.txt  - human-readable symbolized callstack
//   * crash_snap.json  - machine-readable snapshot (consumed by agentkit)
//
// Design:
//   * Process-wide: installed once via SetUnhandledExceptionFilter.
//   * Runs on the faulting thread (the filter callback is SEH, not Vectored).
//   * Looks for PDBs next to the exe (RelWithDebInfo / Debug build).
//   * Address formatting is locale-immune (see FormatHex).
//
// Usage:
//   #include "Runtime/Core/Base/CrashHandler.h"
//   InstallCrashHandler();
// ============================================================================

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Locale-immune hex formatting.
//
// The engine installs a global std::locale (".UTF-8" / "en_US.UTF-8").
// Any `out << std::hex << addr` then inherits that locale's numpunct and emits
// thousands separators: 0x7ff,9bb,33c,52f. That string is unparseable and
// silently broke every downstream consumer. We therefore never stream an
// integer in hex; we format it by hand.
// ---------------------------------------------------------------------------
namespace zcrash
{

inline std::string FormatHex(std::uint64_t v)
{
    static const char* kDigits = "0123456789abcdef";
    char buf[32];
    int pos = 31;
    buf[pos--] = '\0';
    if (v == 0)
    {
        buf[pos--] = '0';
    }
    while (v != 0 && pos >= 0)
    {
        buf[pos--] = kDigits[v & 0xF];
        v >>= 4;
    }
    return std::string(buf + pos + 1);
}

// 十进制的 locale 免疫格式化。
//
// FormatHex 只解决了十六进制，但十进制一样会被 numpunct 插逗号：
// 真实工程里 thread_id 被打成 `4,321`、行号被打成 `2,393`。
// 凡是流式输出的整数都得走这里 —— 只修 hex 是不够的。
inline std::string FormatDec(std::uint64_t v)
{
    char buf[32];
    int pos = 30;
    buf[31] = '\0';
    if (v == 0)
    {
        buf[pos--] = '0';
    }
    while (v != 0 && pos >= 0)
    {
        buf[pos--] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    return std::string(buf + pos + 1);
}

// platform 字段运行时派生（顺带关闭 #10 的硬编码 "windows"）。
// 过去 JSON 里写死 "windows"，agentkit 无法解析非 Windows 快照；即便在 Windows
// 上也应由平台宏派生，保证与 POSIX 分支同一套取值逻辑、同一份 schema 契约。
inline const char* PlatformString()
{
#if defined(_WIN32) || defined(_WIN64)
    return "windows";
#elif defined(__OHOS__) || (defined(OHOS) && OHOS)
    return "ohos";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

// JSON string escaping. Paths contain backslashes, and C++ symbols routinely
// contain <, >, " and &. A naive dump would produce invalid JSON.
inline std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

struct Frame
{
    std::uint64_t pc = 0;
    std::uint64_t module_base = 0;
    std::uint64_t offset = 0;
    std::string module;
    std::string function;
    std::string file;
    int line = 0;
    bool has_symbol = false;
    bool has_line = false;
};
struct Snapshot
{
    std::uint32_t exception_code = 0;
    std::uint64_t exception_address = 0;
    std::uint32_t thread_id = 0;
    std::vector<Frame> frames;

    // ---- 构建身份 ---------------------------------------------------
    // 版本对齐门禁的输入。三个字段各有分工，缺一不可：
    //
    //   product_version  人读版本号（如 0.1.0）。可能为空，且人会写错。
    //   build_id         PDB GUID + Age，形如 "a1b2c3d4...1"。
    //                    这是最可靠的锚点：编译器为每次链接生成 GUID，
    //                    Age 是 PDB 被增量更新的次数。两者组合在同一份
    //                    源码 + 同一份编译配置下才唯一。Release 没有行号
    //                    时，它是把崩溃帧对回源码的唯一硬证据。
    //   build_timestamp  PE 头里的链接时间戳（UTC 秒）。
    //
    // 为什么不能只靠 product_version：版本字符串是人维护的，改代码忘记
    // bump 是常态；而 build_id 是链接器生成的，想撒谎都撒不了。
    std::string build_id;
    std::string product_version;
    std::uint32_t build_timestamp = 0;
    // build_id 为空时说明为什么；空串表示拿到了
    std::string build_id_reason;
};

// Module base for an address. Release builds ship without PDBs: an absolute
// address is dead there, but (base, rva) still lets an offline symbolizer
// resolve the frame later.
// ---------------------------------------------------------------------------
// 构建身份提取：从主模块的 PE 头里读 Debug Directory，取出 PDB GUID + Age。
//
// 为什么不用 GetFileVersionInfo 读版本资源：那玩意儿要写 .rc 文件，
// 而且改代码忘了 bump 是常态。CodeView 记录是链接器自己写的，
// 拿的是事实而不是人声明的东西。
// ---------------------------------------------------------------------------
struct BuildIdentity
{
    std::string build_id;          // "<GUID><Age>"，空串表示拿不到
    std::uint32_t timestamp = 0;   // PE 链接时间戳（UTC 秒）
    bool ok = false;

    // 拿不到 build_id 时的原因。不能只留空串：
    // 「这个构建没生成 PDB」和「我解析失败了」是完全不同的两回事，
    // 前者是构建配置问题，后者是代码 bug。留空串会被当成前者掩盖掉后者。
    std::string reason;
};

inline std::string GuidToString(const GUID& g)
{
    char buf[64];
    std::snprintf(
        buf, sizeof(buf),
        "%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        g.Data1,
        static_cast<unsigned>(g.Data2),
        static_cast<unsigned>(g.Data3),
        static_cast<unsigned>(g.Data4[0]), static_cast<unsigned>(g.Data4[1]),
        static_cast<unsigned>(g.Data4[2]), static_cast<unsigned>(g.Data4[3]),
        static_cast<unsigned>(g.Data4[4]), static_cast<unsigned>(g.Data4[5]),
        static_cast<unsigned>(g.Data4[6]), static_cast<unsigned>(g.Data4[7]));
    return std::string(buf);
}

inline BuildIdentity ReadBuildIdentity()
{
    BuildIdentity id;

    const HMODULE main_module = GetModuleHandleA(nullptr);
    if (main_module == nullptr)
    {
        id.reason = "no_main_module";
        return id;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(main_module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        id.reason = "not_mz";
        return id;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const std::uint8_t*>(main_module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        id.reason = "not_pe";
        return id;
    }

    // PE 时间戳：链接时刻，UTC 秒。人能看懂，便于和 CI 记录对账。
    id.timestamp = static_cast<std::uint32_t>(nt->FileHeader.TimeDateStamp);

    // Debug Directory 可能在任一 section 里，得走 RVA->文件偏移转换。
    const DWORD debug_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
    const DWORD debug_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size;
    if (debug_rva == 0 || debug_size < sizeof(IMAGE_DEBUG_DIRECTORY))
    {
        // 连 Debug Directory 都没有 —— 常见于 /DEBUG 关掉的 Release 构建。
        id.reason = "no_debug_directory";
        return id;
    }

    const WORD section_count = nt->FileHeader.NumberOfSections;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);

    // 找 Debug Directory 落在哪个 section。
    // 注意用 max(VirtualSize, SizeOfRawData) 做上界：有些链接器写的
    // VirtualSize 比实际原始数据小，只按 VirtualSize 判会整个漏掉。
    DWORD debug_file_offset = 0;
    bool found_section = false;
    for (WORD i = 0; i < section_count; ++i)
    {
        const IMAGE_SECTION_HEADER& s = sections[i];
        // std::max, not max(): windows.h defines max as a macro unless the
        // translation unit sets NOMINMAX, and several do (the Editor does),
        // which leaves the bare name undefined -> C3861.
        const DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
        if (debug_rva >= s.VirtualAddress && debug_rva < s.VirtualAddress + span)
        {
            debug_file_offset = s.PointerToRawData + (debug_rva - s.VirtualAddress);
            found_section = true;
            break;
        }
    }

    if (!found_section)
    {
        id.reason = "debug_rva_outside_sections";
        return id;
    }

    // 遍历所有 IMAGE_DEBUG_DIRECTORY 条目，而不是只看第一个。
    // Debug Directory 里通常还塞着 POGO / VC_FEATURE / GCTL 等条目，
    // CodeView 不一定排在第一个 —— 只看第一个会静默漏掉。
    const DWORD entry_count = debug_size / sizeof(IMAGE_DEBUG_DIRECTORY);
    bool saw_any_entry = false;

    for (DWORD i = 0; i < entry_count; ++i)
    {
        const auto* dir = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
            reinterpret_cast<const std::uint8_t*>(main_module) + debug_file_offset
            + i * sizeof(IMAGE_DEBUG_DIRECTORY));

        if (dir->Type == 0 && dir->SizeOfData == 0)
        {
            continue;  // 空条目
        }
        saw_any_entry = true;

        // 只认 CodeView（RSDS）——那是 PDB 的 GUID/Age。
        // NB10 是老格式，没有 GUID，直接跳过不猜。
        if (dir->Type != IMAGE_DEBUG_TYPE_CODEVIEW)
        {
            continue;
        }

        if (dir->PointerToRawData == 0)
        {
            continue;
        }

        const auto* cv = reinterpret_cast<const std::uint8_t*>(main_module) + dir->PointerToRawData;
        if (std::memcmp(cv, "RSDS", 4) != 0)
        {
            continue;
        }

        // RSDS: char[4] sig, GUID guid, DWORD age, char[] pdb_path
        GUID guid{};
        std::memcpy(&guid, cv + 4, sizeof(GUID));
        DWORD age = 0;
        std::memcpy(&age, cv + 4 + sizeof(GUID), sizeof(DWORD));

        id.build_id = GuidToString(guid);
        // Age 拼进去：同一个 PDB 被增量更新后 GUID 不变、Age 递增，
        // 只留 GUID 会把两次不同的构建认成同一次。
        char age_buf[16];
        std::snprintf(age_buf, sizeof(age_buf), "%x", age);
        id.build_id += age_buf;
        id.ok = true;
        return id;
    }

    // 走到这里说明：有 Debug Directory、也扫了所有条目，但没有 CodeView/PDB。
    // 这就是 Release 无 PDB 的真实处境 —— 必须说出来，不能留空串让人
    // 以为「崩溃没抓到」。
    id.reason = saw_any_entry ? "debug_directory_without_codeview" : "empty_debug_directory";
    return id;
}

inline std::uint64_t ModuleBaseOf(HANDLE process, std::uint64_t addr)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
    {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(mbi.AllocationBase);
}

inline std::string ModuleNameOf(HANDLE process, std::uint64_t addr)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
    {
        return {};
    }
    const HMODULE mod = reinterpret_cast<HMODULE>(mbi.AllocationBase);
    if (mod == nullptr)
    {
        return {};
    }
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(mod, path, MAX_PATH) == 0)
    {
        return {};
    }
    const std::string full(path);
    const std::size_t slash = full.find_last_of("\\/");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

inline Snapshot Capture(EXCEPTION_POINTERS* ep,
                        const std::string& product_version = std::string())
{
    Snapshot snap;
    if (ep == nullptr || ep->ExceptionRecord == nullptr)
    {
        return snap;
    }

    snap.exception_code = static_cast<std::uint32_t>(ep->ExceptionRecord->ExceptionCode);
    snap.exception_address = reinterpret_cast<std::uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    snap.thread_id = GetCurrentThreadId();

    // 构建身份：优先用链接器写的事实（build_id），版本串只是人读标签。
    const BuildIdentity bid = ReadBuildIdentity();
    snap.build_id = bid.build_id;
    snap.build_timestamp = bid.timestamp;
    snap.build_id_reason = bid.reason;
    snap.product_version = product_version;

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT* ctx = ep->ContextRecord;
    if (ctx == nullptr)
    {
        SymCleanup(process);
        return snap;
    }

    STACKFRAME64 frame{};
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;

    for (int i = 0; i < 64; ++i)
    {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                         &frame, ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        {
            break;
        }
        if (frame.AddrPC.Offset == 0)
        {
            break;
        }

        const DWORD64 addr = frame.AddrPC.Offset;
        Frame f;
        f.pc = addr;
        f.module_base = ModuleBaseOf(process, addr);
        f.module = ModuleNameOf(process, addr);

        char symbol_buffer[sizeof(SYMBOL_INFO) + 512] = {};
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen   = 511;
        DWORD64 displacement = 0;

        if (SymFromAddr(process, addr, &displacement, symbol))
        {
            f.has_symbol = true;
            f.function = symbol->Name;
            f.offset = displacement;
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, addr, &line_displacement, &line) && line.FileName != nullptr)
        {
            f.has_line = true;
            f.file = line.FileName;
            f.line = static_cast<int>(line.LineNumber);
        }

        snap.frames.push_back(f);
    }

    SymCleanup(process);
    return snap;
}
inline void WriteText(const Snapshot& snap, const std::string& path)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open())
    {
        return;
    }

    out << "ZEngine crash. ExceptionCode=0x" << FormatHex(snap.exception_code)
        << " at 0x" << FormatHex(snap.exception_address)
        << " tid=" << FormatDec(snap.thread_id) << "\n";

    // 构建身份。build_id 是硬证据（链接器写死的 GUID+Age），
    // product_version 只是人读标签——可能为空，也可能没 bump。
    out << "build_id=" << (snap.build_id.empty() ? std::string("<unknown>") : snap.build_id)
        << " timestamp=" << FormatDec(snap.build_timestamp);
    if (!snap.build_id_reason.empty())
    {
        out << " (" << snap.build_id_reason << ")";
    }
    if (!snap.product_version.empty())
    {
        out << " version=" << snap.product_version;
    }
    out << "\n\n";

    for (std::size_t i = 0; i < snap.frames.size(); ++i)
    {
        const Frame& f = snap.frames[i];
        out << "[" << i << "] 0x" << FormatHex(f.pc) << "  ";
        if (f.has_symbol)
        {
            out << f.function << " +0x" << FormatHex(f.offset);
        }
        else
        {
            out << "<no symbol>";
        }
        if (!f.module.empty())
        {
            out << "  [" << f.module << " base=0x" << FormatHex(f.module_base) << "]";
        }
        if (f.has_line)
        {
            out << "  (" << f.file << ":" << FormatDec(static_cast<std::uint64_t>(f.line)) << ")";
        }
        out << "\n";
    }
    out.flush();
}

inline void WriteJson(const Snapshot& snap, const std::string& path)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open())
    {
        return;
    }

    out << "{\n";
    out << "  \"schema\": \"zengine-crash/1\",\n";
    out << "  \"platform\": \"" << PlatformString() << "\",\n";
    out << "  \"exception_type\": \"" << FormatHex(snap.exception_code) << "\",\n";
    out << "  \"exception_address\": \"" << FormatHex(snap.exception_address) << "\",\n";
    out << "  \"thread_id\": " << FormatDec(snap.thread_id) << ",\n";
    out << "  \"product_version\": \"" << JsonEscape(snap.product_version) << "\",\n";
    out << "  \"build_id\": \"" << JsonEscape(snap.build_id) << "\",\n";
    out << "  \"build_timestamp\": " << FormatDec(snap.build_timestamp) << ",\n";
    out << "  \"build_id_reason\": \"" << JsonEscape(snap.build_id_reason) << "\",\n";
    out << "  \"frames\": [\n";

    for (std::size_t i = 0; i < snap.frames.size(); ++i)
    {
        const Frame& f = snap.frames[i];
        out << "    {\"index\": " << i
            << ", \"module\": \"" << JsonEscape(f.module) << "\""
            << ", \"address\": \"" << FormatHex(f.pc) << "\""
            << ", \"module_base\": \"" << FormatHex(f.module_base) << "\""
            << ", \"function\": \"" << JsonEscape(f.function) << "\""
            << ", \"offset\": \"" << FormatHex(f.offset) << "\""
            << ", \"file\": \"" << JsonEscape(f.file) << "\""
            << ", \"line\": " << FormatDec(static_cast<std::uint64_t>(f.line))
            << "}";
        if (i + 1 < snap.frames.size())
        {
            out << ",";
        }
        out << "\n";
    }

    out << "  ]\n}\n";
    out.flush();
}

}  // namespace zcrash

inline LONG WINAPI ZEngineCrashHandler(EXCEPTION_POINTERS* ep);


// 进程级产品版本。由 InstallCrashHandler(version) 写入，崩溃时取用。
//
// 为什么是个函数内的 static 而不是全局变量：这是个 header-only 的 inline
// 头文件，放全局会撞 ODR。static 局部在 C++11 起是线程安全初始化的，
// 崩溃路径上只读不写，没有竞争。
inline std::string& MutableProductVersion()
{
    static std::string s_version;
    return s_version;
}

inline const std::string& ProductVersion()
{
    return MutableProductVersion();
}

inline void InstallCrashHandler(const std::string& product_version = std::string())
{
    MutableProductVersion() = product_version;
    static bool installed = false;
    if (installed) return;
    installed = true;
    SetUnhandledExceptionFilter(ZEngineCrashHandler);
}

// ---- The actual SEH filter (plain function, NOT a lambda) -----------

inline LONG WINAPI ZEngineCrashHandler(EXCEPTION_POINTERS* ep)
{
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string dir(exe_path);
    const std::size_t last_slash = dir.find_last_of("\\/");
    dir = last_slash == std::string::npos ? std::string() : dir.substr(0, last_slash + 1);

    // Declared in LogSystem.h / defined in LogSystem.cpp; routed through a free
    // function so this header stays dependency-light (no EASTL / <format>).
    void ZEngineForceFlushLogs();
    uint32_t ZEngineDumpCrashLogTail(const std::string& dir);

    // Flush the log before writing the crash dump: the logger buffers, so without
    // this every diagnostic line (including DRED breadcrumbs and debug-layer
    // errors) is lost exactly when it matters most -- on a crash.
    ZEngineForceFlushLogs();

    // 兜底（#11）：force_flush_all_logs() 是尽力而为，异步日志尾仍可能丢。把进程级
    // 环形缓冲里最近 N 条**预渲染**日志落盘为 crash_log_tail.txt——flush 失败时的
    // 最后防线，也是崩溃前最后、最该看到的诊断记录。写在 crash_stack.txt 之前，
    // 二者同目录。返回写出的记录条数（忽略返回值，落盘失败不影响崩溃快照）。
    ZEngineDumpCrashLogTail(dir);

    const zcrash::Snapshot snap = zcrash::Capture(ep, ProductVersion());
    zcrash::WriteText(snap, dir + "crash_stack.txt");
    zcrash::WriteJson(snap, dir + "crash_snap.json");

    return EXCEPTION_EXECUTE_HANDLER;
}

#else  // !_WIN32

// ============================================================================
// POSIX crash capture (audit #2).
// ----------------------------------------------------------------------------
// Windows 有 SEH + Minidump，非 Windows 过去是彻底的 no-op：sigaction 都没装，
// 进程踩到 SIGSEGV 直接按默认动作死掉，什么都不留。这里补上对等的兜底——
// 在崩溃信号处理器里把调用栈写到 crash_stack.txt / crash_snap.json，与
// Windows 产物同构，交给同一套离线符号化（addr2line / 自研 agentkit）。
//
// 信号安全（async-signal-safe）是这条路径的生死线：
//   崩溃时进程状态已损坏（堆可能被踩烂、锁可能被持有、分配器不可重入）。
//   处理器里**任何** malloc / std::string / std::ofstream / snprintf /
//   backtrace_symbols（内部走 malloc + 动态链接器锁）都是自杀——它们不是
//   async-signal-safe，可能死锁或二次踩内存，比不装处理器更糟。
//   因此本分支只用：write/open/close（POSIX 保证信号安全）、栈上定长缓冲、
//   手写 hex/dec、backtrace()（纯栈扫描，无分配）、dladdr()（查 maps，无分配）。
//   刻意**不**用 backtrace_symbols_fd：它在 bionic（Android）/ musl（OHOS）上
//   不保证存在，链接期缺符号会让处理器在这些平台上直接失效——而那正是最需要
//   它的目标。dladdr 是 POSIX 标准、各 libc 都有，给出 (object, 基址, 符号名)，
//   配 addr2line 离线解析，是跨 POSIX 最稳的基线。
// ============================================================================

#include <csignal>
#include <cstring>
#include <cstdint>

#if defined(__linux__) || defined(__ANDROID__) || defined(__OHOS__)
    #include <sys/utsname.h>
    #include <unistd.h>       // readlink / write / open / close
    #include <fcntl.h>
    #include <execinfo.h>     // backtrace
    #include <dlfcn.h>        // dladdr
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>  // _NSGetExecutablePath
    #include <execinfo.h>
    #include <dlfcn.h>
    #include <unistd.h>
    #include <fcntl.h>
#elif defined(__EMSCRIPTEN__)
    // WebAssembly / Emscripten：sysroot 里**没有** <execinfo.h>，因此无
    // backtrace()；但 <unistd.h>/<fcntl.h>/<dlfcn.h> 都在，open/write/close/
    // dladdr/Dl_info 全部可用（WASM 是单进程沙箱，这些只是内存/虚拟 FS 操作，
    // 且 write/open/close 是 POSIX 保证的信号安全原语）。backtrace() 由本文件
    // 下方命名空间内的 inline 兜底提供（返回 0 帧，快照仍保留 signal/platform/
    // 日志尾，帧列表优雅降级——比 Windows/POSIX 少帧，但绝不二次踩内存）。
    #include <unistd.h>              // open / write / close
    #include <fcntl.h>               // O_* 标志
    #include <dlfcn.h>               // dladdr / Dl_info
    #include <emscripten/stack.h>    // 栈边界内建（backtrace 兜底预留）
#endif

// 这两个自由函数定义在 LogSystem.cpp 的**全局**作用域（声明见 LogSystem.h）。
//
// 必须在全局作用域声明，不能像原来那样写在 WriteSnapshot() 的函数体里：块作用域
// 的函数声名会把名字放进**最近的 enclosing namespace**（这里是 zcrash_posix），
// 于是链接找的是 zcrash_posix::ZEngineForceFlushLogs —— 它没有任何定义，而全局
// 那个 ::ZEngineForceFlushLogs 明明就在旁边。Windows 分支不受影响（它的 handler
// 在全局作用域，块作用域声明正好落到全局），只有 POSIX 分支会中招；iOS 是第一个
// 真正把这条路径链起来的平台（2026-10-29 交叉编译时暴露）。
void          ZEngineForceFlushLogs();
std::uint32_t ZEngineDumpCrashLogTail(const std::string& dir);

namespace zcrash_posix
{

// ---- backtrace() 兜底（Emscripten / WebAssembly）-------------------------
//
// glibc 的 <execinfo.h> 在 Emscripten sysroot 里不存在，故本文件上方的 include
// 分支拿不到 backtrace()。这里提供一个**信号安全**的同名 inline 定义：返回 0
// 帧（buffer 保持全 null），让 WriteSnapshot 的既有循环自然跳过帧输出。
//
// 为什么是 0 帧而不是手撸栈扫描：崩溃处理器里做栈扫描（读 [sp] 找返回地址）在
// 堆/栈可能已损坏时并不比 backtrace() 本身更安全，且 WASM 是线性内存、没有可
// 可靠区分代码指针与数据的边界。宁可少给帧，也绝不冒二次踩内存的风险——崩溃
// 快照的必达产物是 signal/platform/日志尾，帧只是锦上添花。等 Emscripten 上游
// 补齐 <execinfo.h> 后可整段移除本兜底。
#if defined(__EMSCRIPTEN__)
inline int backtrace(void** buffer, int buffer_size)
{
    if (buffer != nullptr && buffer_size > 0)
    {
        // 全 null：告知调用方"无可用帧"，与 glibc 返回值的空帧语义一致。
        for (int i = 0; i < buffer_size; ++i)
        {
            buffer[i] = nullptr;
        }
    }
    return 0;
}
#endif

// ---- 信号安全的定长整数格式化（栈上，无分配）----------------------------
//
// 与 Windows 分支的 FormatHex/FormatDec 同旨：locale 免疫、不碰 iostream。
// 区别是这里直接写进调用方给的 fd，因为信号处理器里不能攒 std::string。

inline void WriteAll(int fd, const char* s, unsigned n)
{
    // write() 可能短写或被信号打断；循环补写直到全部落地或彻底失败。
    unsigned off = 0;
    while (off < n)
    {
        const ssize_t done = write(fd, s + off, n - off);
        if (done <= 0)
        {
            return;  // 落盘失败：崩溃路径上不打断、不重试，静默作罢
        }
        off += static_cast<unsigned>(done);
    }
}

inline void WriteStr(int fd, const char* s)
{
    if (s != nullptr)
    {
        WriteAll(fd, s, static_cast<unsigned>(std::strlen(s)));
    }
}

inline void WriteHex(int fd, std::uint64_t v)
{
    // 只写裸十六进制数字，**不带 "0x" 前缀**：全部 7 个调用点已在前方各自写了
    // 字面 "0x"（如 "si_addr=0x" / "\"address\": \"0x" / " +0x" / " base=0x"）。
    // 若此处再加前缀，产物会得到 "0x0x1234" 这样的双前缀。
    char buf[24];  // 16 hex + '\0'
    char* p = buf + sizeof(buf) - 1;
    *(--p) = '\0';
    if (v == 0)
    {
        *(--p) = '0';
    }
    while (v != 0 && p > buf)
    {
        *(--p) = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    }
    // 写到 null 终止符为止（不含 '\0'）。此前 buf+sizeof(buf)-1-p **多算 1 字节**，
    // 把 buf 里的 '\0' 也 write() 进文件——产物嵌入 NUL，JSON 畸形、txt 被
    // cat/fgets 提前截断（看似"丢逗号"）。改用 strlen 与 WriteStr 一致。
    WriteAll(fd, p, static_cast<unsigned>(std::strlen(p)));
}

inline void WriteDec(int fd, std::uint64_t v)
{
    char buf[24];
    char* p = buf + sizeof(buf) - 1;
    *(--p) = '\0';
    if (v == 0)
    {
        *(--p) = '0';
    }
    while (v != 0 && p > buf)
    {
        *(--p) = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    // 同 WriteHex：用 strlen 只写真实字符数，不把 '\0' 落盘。
    WriteAll(fd, p, static_cast<unsigned>(std::strlen(p)));
}

// 取可执行文件所在目录（末尾带 '/'），失败返回空串。
// Linux/OHOS/Android 走 /proc/self/exe；macOS 走 _NSGetExecutablePath。
inline void AppendExeDir(char* out, unsigned out_sz)
{
    out[0] = '\0';
    if (out_sz == 0)
    {
        return;
    }
#if defined(__APPLE__)
    // _NSGetExecutablePath 要的是 uint32_t*，不是 size_t*。64 位 macOS/iOS 上
    // unsigned long 是 8 字节，传它的地址根本编译不过（这里过去是 unsigned long，
    // 于是 macOS 上任何包含本头的 TU 都失败）。
    uint32_t cap = static_cast<uint32_t>(out_sz - 1);
    if (cap == 0)
    {
        return;
    }
    if (_NSGetExecutablePath(out, &cap) == 0)
    {
        // 截到最后一个分隔符
        char* slash = nullptr;
        for (char* s = out; *s != '\0'; ++s)
        {
            if (*s == '/' || *s == '\\')
            {
                slash = s;
            }
        }
        if (slash != nullptr)
        {
            slash[1] = '\0';
            return;
        }
    }
    out[0] = '\0';
#else
    char path[512];
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0)
    {
        path[n] = '\0';
        char* slash = nullptr;
        for (char* s = path; *s != '\0'; ++s)
        {
            if (*s == '/' || *s == '\\')
            {
                slash = s;
            }
        }
        if (slash != nullptr)
        {
            slash[1] = '\0';
            std::strncpy(out, path, out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }
    out[0] = '\0';
#endif
}

// JSON 字符串转义后直接写 fd（信号安全：不攒 std::string）。
// 只处理会破坏 JSON 的少数字符；其余原样输出。
inline void WriteJsonEscaped(int fd, const char* s)
{
    if (s == nullptr)
    {
        return;
    }
    char buf[64];
    unsigned bi = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p)
    {
        const char* rep = nullptr;
        char mb[8];
        switch (*p)
        {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (*p < 0x20)
                {
                    // 控制字符 → \uXXXX
                    mb[0] = '\\';
                    mb[1] = 'u';
                    mb[2] = '0';
                    mb[3] = '0';
                    mb[4] = "0123456789abcdef"[(*p >> 4) & 0xF];
                    mb[5] = "0123456789abcdef"[*p & 0xF];
                    mb[6] = '\0';
                    rep = mb;
                }
                else
                {
                    // 普通字节：先冲掉缓冲里的转义序列，再原样写这一个字节
                    if (bi > 0)
                    {
                        buf[bi++] = '\0';
                        WriteAll(fd, buf, bi);
                        bi = 0;
                    }
                    char one[2] = { static_cast<char>(*p), '\0' };
                    WriteAll(fd, one, 1);
                    continue;
                }
                break;
        }
        for (const char* q = rep; *q != '\0'; ++q)
        {
            if (bi < sizeof(buf) - 1)
            {
                buf[bi++] = *q;
            }
            else
            {
                buf[bi] = '\0';
                WriteAll(fd, buf, bi);
                bi = 0;
            }
        }
    }
    if (bi > 0)
    {
        buf[bi] = '\0';
        WriteAll(fd, buf, bi);
    }
}

// platform 字段运行时派生（顺带关闭 #10 的硬编码 "windows"）。
// OHOS/Android 的 uname 都报 "Linux"，必须先用编译期宏区分，否则 agentkit
// 会把鸿蒙/安卓快照误当桌面 Linux 解析。
inline const char* PlatformString()
{
#if defined(__OHOS__) || (defined(OHOS) && OHOS)
    return "ohos";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__)
    return "macos";  // iOS 走独立构建；此处桌面 Apple = macOS
#elif defined(__linux__)
    return "linux";
#else
    return "posix";
#endif
}

// 写一个文件（open/write/close），返回 fd 供调用方继续写。
inline int OpenTrunc(const char* path)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

// ---- 崩溃快照：写 crash_stack.txt（人读）与 crash_snap.json（机读）--------
//
// si_signo/si_errno 与 faulting 地址从 siginfo_t 取；回溯用 backtrace()。
// 栈本身全程信号安全（无 malloc / 无 std::string / 无 iostream）；唯一的分配
// 是末尾落盘日志尾那一次 std::string 构造——刻意排在栈之后，详见函数内注释。

inline void WriteSnapshot(int sig, const siginfo_t* info)
{
    char dir[512];
    AppendExeDir(dir, sizeof(dir));

    // 顺序很重要：**先**写崩溃栈（下面，全程 alloc-free，是必达的保底产物），
    // **再**落盘日志尾。日志尾走 ZEngineDumpCrashLogTail(const std::string&)，
    // 内部会构造一个 std::string（= 一次 malloc）——它不是 async-signal-safe，
    // 若放在栈之前，一旦在堆已损坏时死锁/二次踩内存，我们就连栈都丢了。把
    // 必达的栈写在前面，即使日志尾这一步炸了，crash_stack.txt 也已经落地。
    // 两个自由函数（ZEngineForceFlushLogs / ZEngineDumpCrashLogTail）定义在
    // LogSystem.cpp 的全局作用域，声明见本文件上方 zcrash_posix 之前 —— 刻意不在这里
    // 重复声明：块作用域声明会把名字挂到 zcrash_posix 上，链接就对不上了。

    // 拼产物路径到栈缓冲（信号安全：不调用 std::string / 不分配）。dir 为空
    // 时退化成裸文件名，写到进程当前工作目录。
    char txt_path[768];
    if (dir[0] != '\0')
    {
        std::strncpy(txt_path, dir, sizeof(txt_path) - 1);
        txt_path[sizeof(txt_path) - 1] = '\0';
        std::strncat(txt_path, "crash_stack.txt", sizeof(txt_path) - std::strlen(txt_path) - 1);
    }
    else
    {
        std::strncpy(txt_path, "crash_stack.txt", sizeof(txt_path) - 1);
        txt_path[sizeof(txt_path) - 1] = '\0';
    }

    // ---- crash_stack.txt -------------------------------------------------
    const int txt_fd = OpenTrunc(txt_path);
    if (txt_fd >= 0)
    {
        WriteStr(txt_fd, "ZEngine crash (POSIX). signal=");
        WriteDec(txt_fd, static_cast<std::uint64_t>(sig));
        WriteStr(txt_fd, " si_code=");
        WriteDec(txt_fd, static_cast<std::uint64_t>(info != nullptr ? info->si_code : -1));
        WriteStr(txt_fd, " si_addr=0x");
        WriteHex(txt_fd, reinterpret_cast<std::uint64_t>(info != nullptr ? info->si_addr : nullptr));
        WriteStr(txt_fd, "\n\n");

        void* frames[64];
        const int n = backtrace(frames, 64);
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t addr = reinterpret_cast<std::uint64_t>(frames[i]);
            WriteStr(txt_fd, "[");
            WriteDec(txt_fd, static_cast<std::uint64_t>(i));
            WriteStr(txt_fd, "] 0x");
            WriteHex(txt_fd, addr);

            Dl_info dl;
            if (dladdr(frames[i], &dl) != 0)
            {
                if (dl.dli_sname != nullptr)
                {
                    WriteStr(txt_fd, "  ");
                    WriteStr(txt_fd, dl.dli_sname);
                    const std::uint64_t sym = reinterpret_cast<std::uint64_t>(dl.dli_saddr);
                    if (sym != 0 && addr >= sym)
                    {
                        WriteStr(txt_fd, " +0x");
                        WriteHex(txt_fd, addr - sym);
                    }
                }
                if (dl.dli_fname != nullptr)
                {
                    // 只取文件名部分（路径可能被截断，留基址更重要）
                    const char* base = dl.dli_fname;
                    for (const char* p = dl.dli_fname; *p != '\0'; ++p)
                    {
                        if (*p == '/' || *p == '\\')
                        {
                            base = p + 1;
                        }
                    }
                    WriteStr(txt_fd, "  [");
                    WriteStr(txt_fd, base);
                    WriteStr(txt_fd, " base=0x");
                    WriteHex(txt_fd, reinterpret_cast<std::uint64_t>(dl.dli_fbase));
                    WriteStr(txt_fd, "]");
                }
            }
            WriteStr(txt_fd, "\n");
        }
        WriteStr(txt_fd, "\n-- backtrace only; resolve (module base + offset) with addr2line -e <obj> <offset> --\n");
        close(txt_fd);
    }

    // ---- crash_snap.json -------------------------------------------------
    char json_path[768];
    if (dir[0] != '\0')
    {
        std::strncpy(json_path, dir, sizeof(json_path) - 1);
        json_path[sizeof(json_path) - 1] = '\0';
        std::strncat(json_path, "crash_snap.json", sizeof(json_path) - std::strlen(json_path) - 1);
    }
    else
    {
        std::strncpy(json_path, "crash_snap.json", sizeof(json_path) - 1);
        json_path[sizeof(json_path) - 1] = '\0';
    }
    const int json_fd = OpenTrunc(json_path);
    if (json_fd >= 0)
    {
        WriteStr(json_fd, "{\n");
        WriteStr(json_fd, "  \"schema\": \"zengine-crash/1\",\n");
        WriteStr(json_fd, "  \"platform\": \"");
        WriteStr(json_fd, PlatformString());
        WriteStr(json_fd, "\",\n");
        WriteStr(json_fd, "  \"signal\": ");
        WriteDec(json_fd, static_cast<std::uint64_t>(sig));
        WriteStr(json_fd, ",\n  \"si_code\": ");
        WriteDec(json_fd, static_cast<std::uint64_t>(info != nullptr ? info->si_code : -1));
        WriteStr(json_fd, ",\n  \"si_addr\": \"0x");
        WriteHex(json_fd, reinterpret_cast<std::uint64_t>(info != nullptr ? info->si_addr : nullptr));
        WriteStr(json_fd, "\",\n  \"frames\": [\n");

        void* frames[64];
        const int n = backtrace(frames, 64);
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t addr = reinterpret_cast<std::uint64_t>(frames[i]);
            WriteStr(json_fd, "    {\"index\": ");
            WriteDec(json_fd, static_cast<std::uint64_t>(i));
            WriteStr(json_fd, ", \"address\": \"0x");
            WriteHex(json_fd, addr);
            WriteStr(json_fd, "\"");

            Dl_info dl;
            if (dladdr(frames[i], &dl) != 0)
            {
                WriteStr(json_fd, ", \"symbol\": \"");
                WriteJsonEscaped(json_fd, dl.dli_sname != nullptr ? dl.dli_sname : "");
                WriteStr(json_fd, "\", \"module\": \"");
                const char* base = (dl.dli_fname != nullptr) ? dl.dli_fname : "";
                for (const char* p = base; *p != '\0'; ++p)
                {
                    if (*p == '/' || *p == '\\')
                    {
                        base = p + 1;
                    }
                }
                WriteJsonEscaped(json_fd, base);
                WriteStr(json_fd, "\", \"module_base\": \"0x");
                WriteHex(json_fd, reinterpret_cast<std::uint64_t>(dl.dli_fbase));
                WriteStr(json_fd, "\"");
            }
            WriteStr(json_fd, " }");
            if (i + 1 < n)
            {
                WriteStr(json_fd, ",");
            }
            WriteStr(json_fd, "\n");
        }
        WriteStr(json_fd, "  ]\n}\n");
        close(json_fd);
    }

    // 最后再落盘日志尾（#11 兜底）。放在栈之后：这一步会构造一个 std::string
    // （一次 malloc，非 async-signal-safe），但此刻 crash_stack.txt /
    // crash_snap.json 已落地，即便这里在堆已损坏时失败，保底产物也在。
    // force_flush 是尽力而为，环形缓冲里的预渲染日志是崩溃前最后、最该看到的记录。
    ZEngineForceFlushLogs();
    ZEngineDumpCrashLogTail(std::string(dir));  // 显式构造：本路径唯一一次分配
}

// 处理器入口：SA_SIGINFO 三参签名。
inline void OnFatalSignal(int sig, siginfo_t* info, void* ucontext)
{
    (void)ucontext;
    WriteSnapshot(sig, info);

    // 复位默认动作再重抛：让 core dump / 外层调试器仍能收到原始信号，
    // 也避免我们装的处理器的任何残留状态干扰默认终止。
    signal(sig, SIG_DFL);
    raise(sig);
}

inline void InstallOne(int sig)
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = OnFatalSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;  // 独立信号栈：栈溢出时原栈已不可用
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

// 备用信号栈：栈溢出（SIGSEGV 打在栈 guard page）时，原栈指针已不可用，
// 处理器必须在一块预留栈上跑，否则二次踩内存。
inline void InstallAltStack()
{
    stack_t st;
    std::memset(&st, 0, sizeof(st));
    st.ss_flags = SIGSTKSZ;
    st.ss_size = 16384;  // 给处理器足够空间跑 backtrace + dladdr
    sigaltstack(&st, nullptr);
}

}  // namespace zcrash_posix

// 进程级产品版本（与 Windows 分支同构）。header-only inline，用函数内 static
// 避免 ODR 冲突；C++11 起线程安全初始化，崩溃路径只读不写。
inline std::string& MutableProductVersionPosix()
{
    static std::string s_version;
    return s_version;
}

inline void InstallCrashHandler(const std::string& product_version = std::string())
{
    MutableProductVersionPosix() = product_version;
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;

    zcrash_posix::InstallAltStack();
    zcrash_posix::InstallOne(SIGSEGV);
    zcrash_posix::InstallOne(SIGBUS);
    zcrash_posix::InstallOne(SIGFPE);
    zcrash_posix::InstallOne(SIGILL);
    zcrash_posix::InstallOne(SIGABRT);
    // SIGTRAP 不装：那是断点调试用的，交给调试器默认行为。
}

#endif  // !_WIN32
