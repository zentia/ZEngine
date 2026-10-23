// =============================================================================
// 分配器配对冒烟测试（§3.26）
//
// 盯的是一类**只在 Apple 上致命、其余平台全程隐形**的 bug：
//
//   ObjectManager::Produce()          -> ProduceHelper 里的 `new T()`
//   MemoryManager::CreateObject<T>()  -> mi_malloc + placement new
//
// 非 Apple 上 mimalloc-new-delete.h 接管了全局 operator new/delete，`new` 就是
// mimalloc ⇒ 两条路互相喂也碰巧能跑。Apple 上**故意不接管**（否则 dylib 与 host
// 进程会交叉释放，见 MemoryManager.cpp 顶部那段），于是把 Produce 出来的指针喂给
// mi_free 会在 mi_usable_size -> mi_checked_ptr_segment 里 EXC_BAD_ACCESS /
// SIGBUS —— 而且崩在释放路径上，栈里完全看不出是谁配错的。
//
// 本测试两条腿：
//   1. 运行时：真的 Produce 一个出来，验证 IsMimallocOwned() == false（Apple），
//      再验证 mimalloc 那条释放会被守卫拦下（计数 +1）而不是崩。
//   2. 源码文本：全库扫"new 出来的对象被 mimalloc 释放"，防回归。
//
//   cmake -B build -DZENGINE_BUILD_MEMORY_PAIRING_SMOKE_TEST=ON
//   cmake --build build --target MemoryPairingSmokeTest
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/BaseClasses/GameObject.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
int g_Checks   = 0;
int g_Failures = 0;

void Check(bool condition, const char* name)
{
    ++g_Checks;
    if (!condition)
    {
        ++g_Failures;
        std::printf("[FAIL] %s\n", name);
    }
    else
    {
        std::printf("[ ok ] %s\n", name);
    }
}

// ⚠️ 契约要搜的串一律**拼出来**，不在注释里逐字写出来 —— 否则这条注释自己就把
// 契约蒙过去了（"文本契约被自己的注释蒙过去"，已踩 6 次）。
const std::string MimallocFreeName()
{
    return std::string("MemoryManager::") + "DestroyObject";
}

std::string ReadFile(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
    {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// 剥掉 `//` 之后的内容。留下来的才是"代码"。
std::string StripLineComment(const std::string& line)
{
    const size_t at = line.find("//");
    return (at == std::string::npos) ? line : line.substr(0, at);
}

/// 逐行扫**代码行**（跳过整行注释），返回 needle 首次命中的绝对偏移。
/// 钉"调用点"的断言一律走这个，不走全文 find —— 把一行代码注释掉是最普通的回归，
/// 全文 find 照样命中。
size_t CodeLineFind(const std::string& src, const std::string& needle)
{
    size_t at = 0;
    while (at < src.size())
    {
        size_t eol = src.find('\n', at);
        if (eol == std::string::npos)
        {
            eol = src.size();
        }
        const std::string line  = src.substr(at, eol - at);
        const std::string code  = StripLineComment(line);
        const size_t      hit   = code.find(needle);
        if (hit != std::string::npos)
        {
            return at + hit;
        }
        at = eol + 1;
    }
    return std::string::npos;
}

bool CodeLineMentions(const std::string& src, const std::string& needle)
{
    return CodeLineFind(src, needle) != std::string::npos;
}

/// 从 `Name(` 里抠出实参标识符。
bool ExtractCallArg(const std::string& code, const std::string& call_prefix, std::string& out)
{
    const size_t at = code.find(call_prefix);
    if (at == std::string::npos)
    {
        return false;
    }
    const size_t open = at + call_prefix.size();
    size_t       i    = open;
    while (i < code.size() && (std::isspace(static_cast<unsigned char>(code[i])) || code[i] == '*'))
    {
        ++i;
    }
    const size_t begin = i;
    while (i < code.size() && (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_'))
    {
        ++i;
    }
    if (i == begin)
    {
        return false;
    }
    out = code.substr(begin, i - begin);
    return true;
}

/// var 的分配来源。只区分"new 系"和"mimalloc 系"两类。
/// 返回 "new" / "mimalloc" / ""（认不出）。
std::string ClassifySourceRhs(const std::string& rhs)
{
    if (rhs.find("Produce") != std::string::npos)
    {
        return "new";
    }
    // AssetManager::ReadObject 底层就是 Produce，跨函数最容易看漏。
    if (rhs.find("ReadObject") != std::string::npos && rhs.find("CreateObject") == std::string::npos)
    {
        return "new";
    }
    if (rhs.find("NewObject") != std::string::npos || rhs.find("CreateObject") != std::string::npos)
    {
        return "mimalloc";
    }
    return "";
}

/// 取 `=` 左边紧挨着的那个标识符。
/// ⚠️ 必须先跳过 `=` 左边的**尾部空白**：`PrefabAsset* prefab = ...` 里 `prefab` 和
/// `=` 之间有一个空格，不跳就取到空串 —— 那样整张来源表是空的、扫描恒绿，
/// 变异测试里"改回错配"照样全绿（这次是真踩到了）。
bool ExtractAssignedVar(const std::string& code, size_t eq, std::string& out)
{
    size_t i = eq;
    while (i > 0 && std::isspace(static_cast<unsigned char>(code[i - 1])))
    {
        --i;
    }
    size_t j = i;
    while (j > 0 && (std::isalnum(static_cast<unsigned char>(code[j - 1])) || code[j - 1] == '_'))
    {
        --j;
    }
    if (j == i)
    {
        return false;
    }
    out = code.substr(j, i - j);
    return true;
}

/// 扫一份源码，返回所有"new 系出来的对象被 mimalloc 释放"的实参变量名。
std::vector<std::string> FindOffenders(const std::string& src, const std::string& free_name)
{
    std::vector<std::string>                 offenders;
    std::unordered_map<std::string, std::string> origin;

    // 第一遍：建 var -> 分配来源 的表
    size_t at = 0;
    while (at < src.size())
    {
        size_t eol = src.find('\n', at);
        if (eol == std::string::npos)
        {
            eol = src.size();
        }
        const std::string code = StripLineComment(src.substr(at, eol - at));
        const size_t      eq   = code.find('=');
        if (eq != std::string::npos && eq + 1 < code.size() && code[eq + 1] != '=')
        {
            std::string var;
            if (ExtractAssignedVar(code, eq, var))
            {
                const std::string kind = ClassifySourceRhs(code.substr(eq + 1));
                if (!kind.empty())
                {
                    origin[var] = kind;
                }
            }
        }
        at = eol + 1;
    }

    // 第二遍：每个 mimalloc 释放点，看它的实参是不是 new 系来的
    at = 0;
    while (at < src.size())
    {
        size_t eol = src.find('\n', at);
        if (eol == std::string::npos)
        {
            eol = src.size();
        }
        const std::string code = StripLineComment(src.substr(at, eol - at));
        std::string       var;
        if (ExtractCallArg(code, free_name + "(", var))
        {
            const auto it = origin.find(var);
            if (it != origin.end() && it->second == "new")
            {
                offenders.push_back(var);
            }
        }
        at = eol + 1;
    }
    return offenders;
}

std::vector<std::filesystem::path> CollectSources(const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> out;
    std::error_code                    ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        const std::filesystem::path& p = it->path();
        const std::string            s = p.generic_string();
        if (s.find("/3rdparty/") != std::string::npos || s.find("/build/") != std::string::npos)
        {
            continue;
        }
        const std::string ext = p.extension().generic_string();
        if (ext == ".cpp" || ext == ".h" || ext == ".mm" || ext == ".inl")
        {
            out.push_back(p);
        }
    }
    return out;
}
}  // namespace

// =============================================================================
// 1) 运行时契约
// =============================================================================
static void CheckRuntimePairing()
{
    auto* object_manager = GET_SYSTEM(ObjectManager);
    Check(object_manager != nullptr, "ObjectManager is available");

    // ---- CreateObject 那一侧：必须是 mimalloc 的 ----
    struct Probe
    {
        int payload;
    };
    Probe* from_mimalloc = MemoryManager::CreateObject<Probe>();
    Check(from_mimalloc != nullptr, "MemoryManager::CreateObject allocates");
    Check(MemoryManager::IsMimallocOwned(from_mimalloc),
          "a CreateObject block is mimalloc-owned (so mimalloc release is the right pair)");
    MemoryManager::DestroyObject(from_mimalloc);

    // ---- Produce 那一侧 ----
    Object* produced = object_manager->Produce(TypeOf<GameObject>(), 0);
    Check(produced != nullptr, "ObjectManager::Produce allocates");

#if defined(__APPLE__)
    // 这条是**整件事的根**：Apple 上 Produce 出来的块不在 mimalloc 堆区，
    // 所以喂给 mi_free 必崩。非 Apple 上 new 就是 mimalloc，这里是 true，
    // 也就检测不出来 —— 这正是这类 bug 只在 Apple 上暴露的原因。
    Check(!MemoryManager::IsMimallocOwned(produced),
          "on Apple a Produce block is NOT mimalloc-owned (that is why mi_free faults)");

    // 守卫：真去喂一次，期望被拦下（计数 +1）而不是崩。
    const uint32_t before = MemoryManager::GetForeignFreeCount();
    MemoryManager::DestroyObject(produced);
    const uint32_t after = MemoryManager::GetForeignFreeCount();
    Check(after == before + 1,
          "feeding a Produce block to the mimalloc release is caught by the guard (not a crash)");
#else
    Check(MemoryManager::IsMimallocOwned(produced),
          "off Apple `new` IS mimalloc, so the mismatch is invisible here (documented, not a bug)");
    object_manager->Destroy(produced);
#endif

    // ---- 正常配对不该触发守卫 ----
    const uint32_t before_ok = MemoryManager::GetForeignFreeCount();
    Object*        again     = object_manager->Produce(TypeOf<GameObject>(), 0);
    object_manager->Destroy(again);
    Check(MemoryManager::GetForeignFreeCount() == before_ok,
          "the correct pair (Produce -> ObjectManager::Destroy) never trips the guard");
}

// =============================================================================
// 2) 源码文本契约（全库扫描，防回归）
// =============================================================================
static void CheckSourcePairing()
{
#ifdef ZENGINE_ENGINE_ROOT_DIR
    const std::filesystem::path root   = std::filesystem::path(ZENGINE_ENGINE_ROOT_DIR) / "Source";
    const std::vector<std::filesystem::path> files = CollectSources(root);
    Check(!files.empty(), "the source tree is scannable");

    const std::string free_name = MimallocFreeName();
    int               bad       = 0;
    std::string       first_bad;

    for (const std::filesystem::path& path : files)
    {
        // 本测试自己会在运行时故意做一次错配（验证守卫），必须跳过，否则自己红自己。
        if (path.filename().generic_string() == "MemoryPairingSmokeTest.cpp")
        {
            continue;
        }
        const std::string src = ReadFile(path);
        if (src.empty())
        {
            continue;
        }

        for (const std::string& var : FindOffenders(src, free_name))
        {
            ++bad;
            if (first_bad.empty())
            {
                first_bad = path.generic_string() + " -> " + var;
            }
        }
    }

    Check(bad == 0, "no Produce/ReadObject result anywhere is released through the mimalloc path");
    if (bad != 0)
    {
        std::printf("       first offender: %s (%d total)\n", first_bad.c_str(), bad);
    }

    // ---- 扫描器自检（负向对照）----
    //
    // 光有"bad == 0"是不够的：扫描器自己写错（取不到变量名、认不出来源）时它也
    // 恒为 0，于是"改回错配"这种最普通的回归照样全绿。所以拿一段**已知有错配**
    // 的样例喂进去，必须检出；再拿一段已知正确的喂进去，必须不报。
    const std::string fixture_bad = std::string("Object* a = om->") + "Produce(TypeOf<Foo>(), 0);\n" +
                                    std::string("MemoryManager::") + "DestroyObject(a);\n" +
                                    std::string("Object* b = am->") + "ReadObject(p, t);\n" +
                                    std::string("MemoryManager::") + "DestroyObject(b);\n";
    const std::string fixture_ok = std::string("Bar* c = MemoryManager::") + "CreateObject<Bar>();\n" +
                                   std::string("MemoryManager::") + "DestroyObject(c);\n" +
                                   std::string("Baz* d = om->") + "NewObject<Baz>();\n" +
                                   std::string("MemoryManager::") + "DestroyObject(d);\n" +
                                   std::string("Object* e = om->") + "Produce(TypeOf<Foo>(), 0);\n" +
                                   std::string("om->") + "Destroy(e);\n";

    Check(FindOffenders(fixture_bad, free_name).size() == 2,
          "the scanner catches both mismatch shapes in a known-bad sample (Produce + ReadObject)");
    Check(FindOffenders(fixture_ok, free_name).empty(),
          "the scanner stays silent on a known-good sample (correct pairs only)");

    // ---- 钉住 6 个修好的点本身：必须是 ObjectManager::Destroy ----
    struct Site
    {
        const char* file;
        const char* var;
        const char* what;
    };
    const std::filesystem::path src_root = std::filesystem::path(ZENGINE_ENGINE_ROOT_DIR) / "Source";
    const Site sites[] = {
        {"Runtime/Resource/Prefab/PrefabUtility.cpp", "prefab", "SaveAsPrefabAsset 的临时 PrefabAsset 头"},
        {"Runtime/Function/Framework/Level/Level.cpp", "header", "Level::Save 的临时 LevelRes 头"},
        {"Runtime/Resource/Asset/AssetManager.cpp", "obj", "AssetManager 读失败分支里的对象"},
        {"Editor/EditorWindow/ZSlateInspectorWindow/ZSlateInspectorWindow.cpp", "produced",
         "Inspector 里 ReadObject 出来的 DataTable"},
    };
    for (const Site& s : sites)
    {
        const std::string text = ReadFile(src_root / s.file);
        Check(!text.empty(), s.file);
        Check(CodeLineMentions(text, std::string("->Destroy(") + s.var + ")"),
              (std::string(s.what) + " 用 ObjectManager::Destroy 释放").c_str());
        Check(!CodeLineMentions(text, free_name + "(" + s.var + ")"),
              (std::string(s.what) + " 不再走 mimalloc 释放").c_str());
    }
#endif
}

int main()
{
    RegisterCore();
    RegisterPlatform();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: system bring-up failed\n");
        return 77;
    }

    CheckRuntimePairing();
    CheckSourcePairing();

    std::printf("\nMemory pairing smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
