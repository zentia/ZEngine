// =============================================================================
// CI 覆盖率门禁：一个 smoke test 只有在某个 workflow 里出现
// `-D<NAME>=ON` 才算真的在跑。
//
//   cmake -B build -DZENGINE_BUILD_CI_COVERAGE_SMOKE_TEST=ON
//   cmake --build build --target CiSmokeTestCoverageSmokeTest
//   ctest --test-dir build -C Debug -R CiSmokeTestCoverageSmokeTest
//
// 为什么需要它
// ------------
// 2026-10-30 实测：仓库里有 **90 个** `option(ZENGINE_BUILD_*SMOKE_TEST|_TESTS)`，
// 只有 **38 个** 被任何 workflow 以 `-D...=ON` 启用。另外 52 个从来没有在 CI 上
// 跑过一次 —— 它们能编译、本地有人跑过、但换台机器没人知道。
//
// 最典型的样本是 Lumen 那六组：smoke_tests.yml 用 `ZENGINE_USE_VULKAN=OFF`，
// 该分支把 Function/Render/Lumen/*.cpp 从 ZRuntime 里剔除，所以那边跑不了；
// 文件里的注释写着"它们由 Apple / Linux / Windows+Vulkan 宿主负责"——
// 而 Linux 宿主当时**一个测试都没接**（build_linux.yml 只 build 不 ctest）。
// 于是"注释里有、实际没有"，静默了很久。
//
// 判据必须严格
// ------------
// 只判断"名字在 workflow 文件里出现过"是不够的：上面那六组在 smoke_tests.yml
// 里**被注释提到**（"Not enabled: ZENGINE_BUILD_LUMEN_..."），按出现次数算会
// 误判成已启用。所以这里要求非注释行里出现字面量 `-D<NAME>=ON`。
//
// 双向棘轮
// --------
//   A3 已启用数 >= 39  —— 不许有人把已跑的测试从 CI 里删掉
//   A4 未启用数 <= 52  —— 不许新增一个"写了但没人跑"的测试
// 两个方向都要守：只守一个，另一个方向就能静默退化。52 是**欠账基线**，不是
// 目标值；目标是把它压到 0，每压下去一个就把两个常量一起改小。
// =============================================================================

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool ok, const char* id, const char* what)
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("[ ok ] %-4s %s\n", id, what);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s\n", id, what);
        }
    }

    std::string ReadFileText(const std::filesystem::path& in_path)
    {
        std::ifstream in(in_path, std::ios::binary);
        if (!in)
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool IsIdentifierChar(char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    /// Build directories and 3rdparty are not ours: scanning them produces
    /// hundreds of unrelated options and would drown the real signal.
    bool IsScanCandidate(const std::filesystem::path& in_path)
    {
        const std::string generic = in_path.generic_string();
        if (generic.find("/3rdparty/") != std::string::npos)
        {
            return false;
        }
        for (const auto& part : in_path)
        {
            const std::string p = part.generic_string();
            if (p == "build" || p.compare(0, 6, "build-") == 0)
            {
                return false;
            }
        }
        return true;
    }

    /// `option(ZENGINE_BUILD_FOO_SMOKE_TEST` -> ZENGINE_BUILD_FOO_SMOKE_TEST
    std::set<std::string> CollectTestOptions(const std::filesystem::path& in_engine_root)
    {
        std::set<std::string> found;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(in_engine_root, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (!it->is_regular_file(ec) || it->path().filename() != "CMakeLists.txt")
            {
                continue;
            }
            if (!IsScanCandidate(it->path()))
            {
                it.disable_recursion_pending();
                continue;
            }

            const std::string text = ReadFileText(it->path());
            size_t at = 0;
            while ((at = text.find("option(", at)) != std::string::npos)
            {
                size_t name_begin = at + 7;
                while (name_begin < text.size() && std::isspace(static_cast<unsigned char>(text[name_begin])))
                {
                    ++name_begin;
                }
                size_t name_end = name_begin;
                while (name_end < text.size() && IsIdentifierChar(text[name_end]))
                {
                    ++name_end;
                }
                const std::string name = text.substr(name_begin, name_end - name_begin);
                at = name_end;

                const bool is_test = name.size() > 14 &&
                                     (name.compare(name.size() - 10, 10, "SMOKE_TEST") == 0 ||
                                      name.compare(name.size() - 6, 6, "_TESTS") == 0);
                if (is_test && name.compare(0, 14, "ZENGINE_BUILD_") == 0)
                {
                    found.insert(name);
                }
            }
        }
        return found;
    }

    /// Flags really passed to CMake: `-D<NAME>=ON` on a non-comment line.
    std::set<std::string> CollectEnabledFlags(const std::filesystem::path& in_workflow_dir)
    {
        std::set<std::string> enabled;
        std::error_code ec;
        if (!std::filesystem::is_directory(in_workflow_dir, ec))
        {
            return enabled;
        }
        for (std::filesystem::directory_iterator it(in_workflow_dir, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            const std::string ext = it->path().extension().generic_string();
            if (ext != ".yml" && ext != ".yaml")
            {
                continue;
            }
            const std::string text = ReadFileText(it->path());
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
            {
                // CRLF: getline leaves the '\r' behind, and comparing "=ON\r"
                // against "=ON" silently fails -- a whole workflow then reads as
                // zero flags and the ratchet loses its lower bound. Found by
                // mutation: rewriting a .yml with Python (CRLF on Windows)
                // dropped the Linux job's 37 flags to 0.
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                const size_t first = line.find_first_not_of(" \t\r");
                if (first == std::string::npos || line[first] == '#')
                {
                    continue;
                }
                size_t at = 0;
                while ((at = line.find("-DZENGINE_BUILD_", at)) != std::string::npos)
                {
                    const size_t name_begin = at + 2;
                    size_t name_end = name_begin;
                    while (name_end < line.size() && IsIdentifierChar(line[name_end]))
                    {
                        ++name_end;
                    }
                    if (name_end < line.size() && line.compare(name_end, 4, "=ON") == 0)
                    {
                        enabled.insert(line.substr(name_begin, name_end - name_begin));
                    }
                    at = name_end;
                }
            }
        }
        return enabled;
    }
}  // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("\n=== CiSmokeTestCoverage smoke test ===\n\n");

    const std::filesystem::path repo_root = ZENGINE_REPO_ROOT_DIR;
    const std::filesystem::path engine_root = ZENGINE_ENGINE_ROOT_DIR;
    const std::filesystem::path workflow_dir = repo_root / ".github" / "workflows";

    // ---- A. 扫描覆盖率：扫不到东西的话后面全是空跑 ----
    std::printf("-- A. scan coverage --\n");
    std::error_code ec;
    Check(std::filesystem::is_directory(engine_root, ec), "A1", "工程根存在");
    Check(std::filesystem::is_directory(workflow_dir, ec), "A2", ".github/workflows 存在");

    const std::set<std::string> options = CollectTestOptions(engine_root);
    Check(options.size() >= 50u, "A3", "扫到的测试开关数量（防空扫描）");

    const std::set<std::string> enabled = CollectEnabledFlags(workflow_dir);
    Check(!enabled.empty(), "A4", "扫到至少一条 -D...=ON");

    // ---- B. 双向棘轮 ----
    std::printf("\n-- B. ratchet --\n");

    size_t enabled_known = 0;
    std::vector<std::string> phantom;
    for (const auto& name : enabled)
    {
        if (options.count(name) != 0)
        {
            ++enabled_known;
        }
        else
        {
            phantom.push_back(name);
        }
    }

    // 一个拼错的 -D 不会让 CMake 报错，只会让那个测试静默地不跑。
    Check(phantom.empty(), "B1", "workflow 里没有指向不存在开关的 -D...=ON");
    for (const auto& name : phantom)
    {
        std::printf("       phantom flag: %s\n", name.c_str());
    }

    Check(enabled_known >= 39u, "B2", "被 CI 真正启用的测试开关数（只许增）");

    std::vector<std::string> unenabled;
    for (const auto& name : options)
    {
        if (enabled.count(name) == 0)
        {
            unenabled.push_back(name);
        }
    }

    Check(unenabled.size() <= 52u, "B3", "从未在任何 CI 上跑过的测试开关数（只许减）");

    std::printf("\n   CI 启用 %zu / 仓库共 %zu；欠账 %zu：\n", enabled_known, options.size(), unenabled.size());
    for (const auto& name : unenabled)
    {
        std::printf("       %s\n", name.c_str());
    }

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
