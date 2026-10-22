// =====================================================================
// ProjectInfo 路径字段冒烟测试。
// ---------------------------------------------------------------------
// 为什么需要它：`.zproject` 里的目录字段以前**从来没有真的被读进来过**。
//
//   ProjectInfo 里 content_dir / config_dir / saved_dir / source_dir 是裸
//   `std::string` 且没有 `ZPROPERTY()` ⇒ 生成的 Transfer 里根本没有它们
//   ⇒ `LoadAssetByYaml` 把 .zproject 的 `content_dir: asset` 当陌生 key 丢掉
//   ⇒ `GetProjectContent()` 永远返回 `<project>/Assets`。
//
// 后果是一条完全无声的链（macOS 上文件系统区分大小写才暴露出来）：
//   扫描/导入/cook 全走空目录 → 9 个 .tga 一个都没被导入成 Texture2D .zasset
//   → Material 的 legacy 贴图路径解析不到 → 采样占位白图。
//   日志里一条"我找错目录了"都没有，只有 1.5 万条
//   "legacy texture path ... could not be resolved"。
//
// 修法是给四个字段加 `ZPROPERTY()` 并把类型换成 `eastl::string`
// （SerializeTraits 只对 eastl::string 有特化）。这个测试就是那根钉子：
// 谁再把宏摘掉，第 1 组断言立刻红。
//
// 顺带钉 `LoadFromFile` 的路径规范化：`-p ZGameSmoke.zproject` 这种裸文件名
// 的 `parent_path()` 是空串，不规范化就让 `project_path.empty()` 把
// ImportProjectTextures / CookProjectTextures / ImportProjectShaders 全 bail 掉。
//
//   cmake -B build -DZENGINE_BUILD_PROJECT_INFO_PATHS_SMOKE_TEST=ON
//   cmake --build build --target ProjectInfoPathsSmokeTest
// =====================================================================

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/BaseClasses/Type.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/RegisterRuntime.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

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

    // 和 engine/ZGameSmoke.zproject 同格式（block YAML）。content_dir 故意写成
    // 小写的 "asset" —— 引擎默认值是 "Assets"，两者不同，所以"读进来了"和
    // "用了默认值"在这一条断言上就分得开。
    const char* kProjectText =
        "name: PathsSmoke\n"
        "description: scratch project for the ProjectInfo path smoke test\n"
        "version: 1.0.0\n"
        "engine_version: 0.1.0\n"
        "startup_level: asset/level/1-1.scene\n"
        "content_dir: asset\n"
        "config_dir: config\n"
        "saved_dir: saved\n"
        "source_dir: source\n"
        "scripts_dir: Scripts\n"
        "intermediate_dir: Intermediate\n";

    const char* kProjectTextNoDirs =
        "name: PathsSmokeLegacy\n"
        "version: 1.0.0\n";

    void WriteText(const std::filesystem::path& path, const char* text)
    {
        std::FILE* file = std::fopen(path.string().c_str(), "wb");
        if (file == nullptr)
        {
            return;
        }
        std::fwrite(text, 1, std::strlen(text), file);
        std::fclose(file);
    }

    ProjectInfo* MakeProjectInfo()
    {
        ObjectManager* object_manager = GET_SYSTEM(ObjectManager);
        if (object_manager == nullptr)
        {
            return nullptr;
        }
        // 必须走 ObjectManager::Produce —— ObjectManager 负责 InstanceID 和
        // 生命周期记账，直接 new 是不合法的。
        Object* produced = object_manager->Produce(TypeOf<ProjectInfo>(), /*instanceID=*/0);
        return static_cast<ProjectInfo*>(produced);
    }
}  // namespace

int main()
{
    std::printf("=== ProjectInfo paths smoke test ===\n");

    RegisterCore();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: SystemRegistry::InitializeAll(false) failed.\n");
        return 77;
    }

    std::error_code ec;
    const std::filesystem::path scratch_raw =
        std::filesystem::temp_directory_path() / "zengine_project_info_paths_smoke";
    std::filesystem::remove_all(scratch_raw, ec);
    std::filesystem::create_directories(scratch_raw, ec);
    // 必须解析一遍：macOS 的 temp_directory_path() 是符号链接（/var -> /private/var），
    // 而 LoadFromFile 对工程路径做了 weakly_canonical ⇒ 两边不解析就永远比不上，
    // 断言红的其实是测试自己而不是被测代码。
    const std::filesystem::path scratch = std::filesystem::weakly_canonical(scratch_raw, ec);

    // ---- 1. 四个目录字段必须真的从 .zproject 读进来 -------------------
    {
        const std::filesystem::path project_file = scratch / "PathsSmoke.zproject";
        WriteText(project_file, kProjectText);

        ProjectInfo* info = MakeProjectInfo();
        Check(info != nullptr, "ObjectManager produced a ProjectInfo");
        if (info != nullptr)
        {
            Check(info->LoadFromFile(project_file), "LoadFromFile reads the block-YAML .zproject");

            // 这一组就是回归门禁：缺了 ZPROPERTY() 这里全变成 "Assets"/默认值。
            Check(info->content_dir == "asset", "content_dir is deserialized from the .zproject");
            Check(info->config_dir == "config", "config_dir is deserialized from the .zproject");
            Check(info->saved_dir == "saved", "saved_dir is deserialized from the .zproject");
            Check(info->source_dir == "source", "source_dir is deserialized from the .zproject");

            // 派生路径：导入/cook/扫描都从这里出发，指错目录就是整条链静默失效。
            Check(info->GetProjectContent() == scratch / "asset",
                  "GetProjectContent() honours content_dir (not the 'Assets' default)");
            Check(info->GetGeneratedAssetsRoot() == scratch / "asset" / "_Generated",
                  "GetGeneratedAssetsRoot() sits under content_dir");

            // 导入产物（派生数据）必须落进 _Generated/Textures/，镜像源图在
            // content 下的相对路径 —— 绝不能写在源图旁边（那等于把派生数据混进
            // 签进 VCS 的源树，而且 content 被整体重建时和源图一起没了）。
            Check(info->GetGeneratedTexturesRoot() == scratch / "asset" / "_Generated" / "Textures",
                  "GetGeneratedTexturesRoot() is <content>/_Generated/Textures");
            Check(info->GetGeneratedTexturePath(scratch / "asset" / "objects" / "gold.tga") ==
                      scratch / "asset" / "_Generated" / "Textures" / "objects" / "gold.zasset",
                  "GetGeneratedTexturePath() mirrors the source path under the generated bucket");
            Check(info->GetGeneratedTexturePath(scratch / "asset" / "a" / "b" / "c.png") ==
                      scratch / "asset" / "_Generated" / "Textures" / "a" / "b" / "c.zasset",
                  "GetGeneratedTexturePath() handles nested source paths");
            // 源图不在 content 下时退化成只镜像文件名，不能返回空（调用方靠空值 bail）。
            Check(info->GetGeneratedTexturePath(scratch / "outside" / "x.tga") ==
                      scratch / "asset" / "_Generated" / "Textures" / "x.zasset",
                  "a source outside content falls back to mirroring the file name");
        }
    }

    // ---- 2. 裸文件名不能让 project_path 变空 --------------------------
    {
        const std::filesystem::path absolute = scratch / "PathsSmoke.zproject";
        WriteText(absolute, kProjectText);

        ProjectInfo* info = MakeProjectInfo();
        Check(info != nullptr, "second ProjectInfo produced");
        if (info != nullptr)
        {
            // 用只带文件名的相对路径：cwd 是 build 目录，文件在 temp 下，
            // 所以"碰巧等于 cwd"这条路是走不通的 —— 只有规范化能救。
            const std::filesystem::path saved_cwd = std::filesystem::current_path(ec);
            std::filesystem::current_path(scratch, ec);
            const bool loaded = info->LoadFromFile(std::filesystem::path("PathsSmoke.zproject"));
            std::filesystem::current_path(saved_cwd, ec);

            Check(loaded, "LoadFromFile accepts a bare file name");
            Check(!info->project_path.empty(),
                  "a bare file name still yields a non-empty project_path (parent_path() would be empty)");
            Check(info->project_path.is_absolute(), "project_path is absolute");
            Check(std::filesystem::weakly_canonical(info->project_path) ==
                      std::filesystem::weakly_canonical(scratch),
                  "project_path points at the directory that actually holds the .zproject");
        }
    }

    // ---- 3. 老工程（没有这些 key）必须继续用默认值 --------------------
    {
        const std::filesystem::path project_file = scratch / "Legacy.zproject";
        WriteText(project_file, kProjectTextNoDirs);

        ProjectInfo* info = MakeProjectInfo();
        Check(info != nullptr, "third ProjectInfo produced");
        if (info != nullptr)
        {
            Check(info->LoadFromFile(project_file), "LoadFromFile reads a .zproject without the dir keys");
            Check(info->content_dir == "Assets",
                  "a .zproject without content_dir keeps the 'Assets' default");
            Check(info->config_dir == "config" && info->saved_dir == "saved",
                  "the other dir keys keep their defaults too");
        }
    }

    // ---- 4. SaveToFile 回环：目录字段要能写出去再读回来 ----------------
    {
        const std::filesystem::path project_file = scratch / "RoundTrip.zproject";
        WriteText(project_file, kProjectText);

        ProjectInfo* written = MakeProjectInfo();
        ProjectInfo* reread  = MakeProjectInfo();
        Check(written != nullptr && reread != nullptr, "round-trip ProjectInfo pair produced");
        if (written != nullptr && reread != nullptr)
        {
            Check(written->LoadFromFile(project_file), "round-trip: first load");
            Check(written->SaveToFile(), "SaveToFile writes the .zproject back");
            Check(reread->LoadFromFile(project_file), "round-trip: second load");
            Check(reread->content_dir == written->content_dir,
                  "content_dir survives a save/load round-trip");
            Check(reread->content_dir == "asset", "the round-tripped content_dir is still 'asset'");
        }
    }

    std::filesystem::remove_all(scratch, ec);

    SystemRegistry::GetInstance().ShutdownAll();

    std::printf("\n%d checks, %d failures\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
