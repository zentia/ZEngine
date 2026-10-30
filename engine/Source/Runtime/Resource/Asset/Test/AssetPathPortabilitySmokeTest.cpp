// =============================================================================
// 资产可移植性冒烟测试：资产文件里不许出现机器相关的绝对路径。
//
//   cmake -B build -DZENGINE_BUILD_ASSET_PATH_PORTABILITY_SMOKE_TEST=ON
//   cmake --build build --target AssetPathPortabilitySmokeTest
//   ctest --test-dir build -C Debug -R AssetPathPortabilitySmokeTest
//
// 为什么需要它
// ------------
// 2026-10-30 之前，8 个 prefab 的 externals 表里写的是
//
//     path: "E:/Engine/ZEngine/engine/asset/objects/.../player.object_controller_config.zasset"
//
// 也就是**作者机器的绝对路径**。后果不是崩溃，是**换台机器/换盘就静默断链**：
// `AssetManager::ResolveExternalAssetPath` 拿到绝对路径时先按原样 exists() 试，
// 试不到才退回"工程根 + 相对路径"；于是开发机上一切正常，别人拉下来 PPtr 全部
// 解析成 null，表现是"角色没有控制器参数、墙没有碰撞体" —— 而没有任何一条日志。
//
// 这类问题只靠人肉 code review 抓不住（diff 里那行路径看着很正常），必须机器守。
//
// 三层断言，缺一不可
// ------------------
//   A. 扫描覆盖率：必须真的扫到文件。根路径写错 => 0 个文件 => 全绿，这是
//      最典型的"空测试"陷阱，所以先钉"扫到了多少"。
//   B. 文本资产里不许出现绝对路径（Windows 盘符 / POSIX home）。
//   C. externals 表里写出来的相对路径必须**真的能解析到文件**。
//      B 抓不到"路径是相对的相对错了"——那种同样断链，同样静默。
//
// C 组也是本次修复的验证：8 个 prefab 的 27 处绝对路径改成工程相对形式之后，
// 它们必须都能在工程根下找到。
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    void CheckCount(size_t got, size_t want_at_least, const char* id, const char* what)
    {
        ++g_Checks;
        if (got >= want_at_least)
        {
            std::printf("[ ok ] %-4s %s (%zu)\n", id, what, got);
        }
        else
        {
            ++g_Failures;
            std::printf("[FAIL] %-4s %s got=%zu want>=%zu\n", id, what, got, want_at_least);
        }
    }

    /// 只扫文本资产。.zasset 是二进制（里面也有路径字符串，但那是另一套
    /// 序列化，不是作者手写的引用，扫它会有一堆无关命中）。
    bool IsTextAsset(const std::filesystem::path& p)
    {
        const std::string ext = p.extension().generic_string();
        return ext == ".prefab" || ext == ".scene" || ext == ".mat" ||
               ext == ".meta"  || ext == ".json"  || ext == ".yaml" ||
               ext == ".yml"   || ext == ".txt";
    }

    /// 一行里是否含 Windows 盘符绝对路径（`E:/` 或 `E:\`）。
    bool HasWindowsAbsolute(const std::string& line)
    {
        for (size_t i = 1; i + 1 < line.size(); ++i)
        {
            if (line[i] != ':')
            {
                continue;
            }
            const unsigned char prev = static_cast<unsigned char>(line[i - 1]);
            if (!std::isalpha(prev))
            {
                continue;
            }
            const char next = line[i + 1];
            if (next == '/' || next == '\\')
            {
                return true;
            }
        }
        return false;
    }

    /// POSIX 的家目录绝对路径（`/Users/xxx/` / `/home/xxx/`）。
    bool HasPosixHomeAbsolute(const std::string& line)
    {
        return line.find("/Users/") != std::string::npos ||
               line.find("/home/") != std::string::npos;
    }

    std::string ReadFileText(const std::filesystem::path& p)
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
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("\n=== AssetPathPortability smoke test ===\n\n");

    const std::filesystem::path engine_root = ZENGINE_ENGINE_ROOT_DIR;
    const std::filesystem::path asset_root = engine_root / "asset";

    // -------------------------------------------------------------------------
    // A. 覆盖率：根路径对不对，决定了后面两组是不是空跑
    // -------------------------------------------------------------------------
    std::error_code ec;
    Check(std::filesystem::is_directory(asset_root, ec), "A1", "能定位 <engine>/asset 目录");
    if (!std::filesystem::is_directory(asset_root, ec))
    {
        std::printf("       looked for: %s\n", asset_root.generic_string().c_str());
        std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
        return 1;
    }

    std::vector<std::filesystem::path> text_assets;
    for (std::filesystem::recursive_directory_iterator it(asset_root, ec), end;
         it != end; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (it->is_regular_file(ec) && IsTextAsset(it->path()))
        {
            text_assets.push_back(it->path());
        }
    }

    // 空测试陷阱：如果下面这个数是 0，B/C 两组就是"零个样本、零个失败"。
    CheckCount(text_assets.size(), 10, "A2", "扫到的文本资产数量");

    size_t prefab_count = 0;
    for (const auto& p : text_assets)
    {
        if (p.extension() == ".prefab")
        {
            ++prefab_count;
        }
    }
    // 出问题的正是 prefab 的 externals 表，所以必须确认这类样本真的在扫描范围内。
    CheckCount(prefab_count, 5, "A3", "扫到的 .prefab 数量");

    // -------------------------------------------------------------------------
    // B. 文本资产里不许出现机器相关的绝对路径
    // -------------------------------------------------------------------------
    {
        size_t offenders = 0;
        for (const auto& path : text_assets)
        {
            std::ifstream in(path);
            if (!in)
            {
                continue;
            }
            std::string line;
            size_t line_no = 0;
            while (std::getline(in, line))
            {
                ++line_no;
                if (!HasWindowsAbsolute(line) && !HasPosixHomeAbsolute(line))
                {
                    continue;
                }
                ++offenders;
                if (offenders <= 10)
                {
                    const std::string rel =
                        std::filesystem::relative(path, engine_root, ec).generic_string();
                    std::printf("       %s:%zu: %s\n", rel.c_str(), line_no, line.c_str());
                }
            }
        }
        Check(offenders == 0, "B1", "文本资产里没有 Windows 盘符 / POSIX home 绝对路径");
    }

    // -------------------------------------------------------------------------
    // C. externals 表里的相对路径必须真的能解析到文件
    //
    //    ResolveExternalAssetPath 把相对路径拼到 ProjectInfo::GetProjectRoot()
    //    上，而工程根就是 <engine>（ZGameSmoke.zproject 所在目录）。所以这里
    //    直接按 <engine>/<path> 校验存在性。
    // -------------------------------------------------------------------------
    {
        size_t refs = 0;
        size_t missing = 0;

        for (const auto& path : text_assets)
        {
            if (path.extension() != ".prefab" && path.extension() != ".scene")
            {
                continue;
            }

            std::ifstream in(path);
            if (!in)
            {
                continue;
            }
            std::string line;
            while (std::getline(in, line))
            {
                // externals 表里的条目形如：    path: "asset/objects/.../x.zasset"
                const size_t colon = line.find("path:");
                if (colon == std::string::npos)
                {
                    continue;
                }
                const size_t open = line.find('"', colon);
                const size_t close = line.find('"', open == std::string::npos ? colon : open + 1);
                if (open == std::string::npos || close == std::string::npos)
                {
                    continue;
                }
                const std::string ref = line.substr(open + 1, close - open - 1);
                if (ref.empty())
                {
                    continue;
                }

                ++refs;
                const std::filesystem::path resolved = engine_root / ref;
                if (!std::filesystem::exists(resolved, ec))
                {
                    ++missing;
                    if (missing <= 10)
                    {
                        const std::string rel =
                            std::filesystem::relative(path, engine_root, ec).generic_string();
                        std::printf("       断链 %s -> %s\n", rel.c_str(), ref.c_str());
                    }
                }
            }
        }

        CheckCount(refs, 10, "C1", "prefab/scene externals 里引用的路径条数");
        Check(missing == 0, "C2", "externals 里每条相对路径都能在工程根下解析到文件");
    }

    std::printf("\n=== %d checks, %d failures ===\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
