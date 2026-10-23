#include "Editor/ToolSuite/SpineCook/SpineCookSuite.h"

#include "Runtime/Function/Render/Spine/SpineGpuAssetFile.h"
#include "Runtime/Function/Render/Spine/SpineGpuFormat.h"
#include "Runtime/Function/Render/Spine/SpineGpuImporter.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <commdlg.h>  // GetOpenFileNameW（交互分支的"浏览骨架"）
    #include <windows.h>

    #include "ZSlate/App/SlateStandaloneApp.h"
    #include "ZSlate/Widgets/Input/SButton.h"
    #include "ZSlate/Widgets/Input/SEditableTextBox.h"
    #include "ZSlate/Widgets/Layout/SBoxPanel.h"
    #include "ZSlate/Widgets/Layout/SSpacer.h"
    #include "ZSlate/Widgets/Text/STextBlock.h"
#endif

namespace ZEditor
{
namespace
{
    // -------------------------------------------------------------------------
    // 命令行
    // -------------------------------------------------------------------------
    const char* const kUsage =
        "用法：ZEditor -batchmode -suite=SpineCook <输入> [选项]\n"
        "\n"
        "  输入            一个 .json / .skel 骨架，或一个目录\n"
        "                  Unity 的 `<名>.skel.bytes` 同样认（图集找 `<名>.atlas.txt`）\n"
        "\n"
        "  -o <路径>       输出 .zspg（或目录）；省略 ⇒ 与骨架同目录同名\n"
        "  -atlas <路径>   图集；省略则取与骨架同名的 .atlas / .atlas.txt\n"
        "  --recursive     输入是目录时连子目录一起走\n"
        "\n"
        "例：\n"
        "  ZEditor -batchmode -suite=SpineCook hero.skel -o hero.zspg\n"
        "  ZEditor -batchmode -suite=SpineCook hero.skel.bytes -atlas hero.atlas.txt -o hero.zspg\n"
        "  ZEditor -batchmode -suite=SpineCook assets/spine -o out/ --recursive\n"
        "\n"
        "退出码：0 = 全部成功，1 = 有失败（失败数会打出来）。\n";

    bool ReadWholeFile(const std::filesystem::path& p, std::vector<uint8_t>& out, std::string& err)
    {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec) || ec)
        {
            err = "文件不存在：" + PathToUtf8(p);
            return false;
        }

        FILE* f = nullptr;
#ifdef _WIN32
        // ⚠️ 用宽字符版本：CRT 的 fopen 走 ANSI 代码页，非 ASCII 路径直接 mangled。
        f = _wfopen(p.c_str(), L"rb");
#else
        f = std::fopen(p.c_str(), "rb");
#endif
        if (f == nullptr)
        {
            err = "打不开：" + PathToUtf8(p);
            return false;
        }
        if (std::fseek(f, 0, SEEK_END) != 0)
        {
            std::fclose(f);
            err = "读不出大小：" + PathToUtf8(p);
            return false;
        }
        const long size = std::ftell(f);
        if (size < 0)
        {
            std::fclose(f);
            err = "读不出大小：" + PathToUtf8(p);
            return false;
        }
        std::rewind(f);
        out.resize(static_cast<size_t>(size));
        const size_t got = size == 0 ? 0 : std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        if (got != out.size())
        {
            err = "读取被截断：" + PathToUtf8(p);
            return false;
        }
        return true;
    }

    bool WriteWholeFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes, std::string& err)
    {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec)
        {
            err = "建输出目录失败 " + PathToUtf8(p.parent_path()) + "：" + ec.message();
            return false;
        }
        FILE* f = nullptr;
#ifdef _WIN32
        f = _wfopen(p.c_str(), L"wb");
#else
        f = std::fopen(p.c_str(), "wb");
#endif
        if (f == nullptr)
        {
            err = "写不了：" + PathToUtf8(p);
            return false;
        }
        const size_t wrote = std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (wrote != bytes.size())
        {
            err = "写入被截断：" + PathToUtf8(p);
            return false;
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // 文件名 → 类型
    // -------------------------------------------------------------------------
    // Unity 的 spine-unity 把二进制骨架命名成 `<名>.skel.bytes`、图集 `<名>.atlas.txt`
    // （`.bytes`/`.txt` 才会被 Unity 当文本资源收）。本工程 OSGame 的 782 个资产
    // **全是这个命名**，只认 `.skel` 会一个都跑不了。
    enum class SkeletonKind
    {
        kNone,
        kJson,
        kBinary,
    };

    bool EndsWith(const std::string& s, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    SkeletonKind KindOfSkeleton(const std::filesystem::path& p)
    {
        const std::string name = PathToUtf8(p.filename());
        // ⚠️ 先判 `.skel.bytes`：`.skel` 是它的前缀，顺序反了会把二进制当二进制……
        // 实际上两者都走二进制解析器，真正的坑在 StripSkeletonSuffix 那边。
        if (EndsWith(name, ".skel") || EndsWith(name, ".skel.bytes")) return SkeletonKind::kBinary;
        if (EndsWith(name, ".json")) return SkeletonKind::kJson;
        return SkeletonKind::kNone;
    }

    // `<名>.skel.bytes` → `<名>`（去掉**整个**骨架后缀）。
    // ⚠️ 不能用 `replace_extension()`：它只换最后一个扩展名，
    //    `X.skel.bytes` 会变成 `X.skel.zspg`，一个目录批量导出下来全是这种怪名。
    std::filesystem::path StripSkeletonSuffix(const std::filesystem::path& p)
    {
        const std::string name = PathToUtf8(p.filename());
        for (const char* suf : {".skel.bytes", ".skel", ".json"})
        {
            if (EndsWith(name, suf))
            {
                return p.parent_path() / name.substr(0, name.size() - std::strlen(suf));
            }
        }
        return p;
    }

    // -------------------------------------------------------------------------
    // 单个骨架 → 容器
    // -------------------------------------------------------------------------
    // 失败时把原因写进 error（调用方负责打出来并计失败数）。
    // 每一步单独检查：导入器已经把"静默调整"全挡成报错，这里再吞掉错误信息
    // 就等于把那层防护白做。
    bool CookOne(const std::filesystem::path& skeleton_path,
                 const std::filesystem::path& atlas_path,
                 const std::filesystem::path& out_path,
                 std::string&                 error)
    {
        // 解析 → 烘焙 → 序列化与渲染器 / 编辑器自动 cook 是同一个函数（SpineGpuAssetFile.h）
        std::vector<uint8_t> container;
        SpineGpuAsset        asset;
        const SpineImportResult r = SpineGpuCookSkeleton(skeleton_path, atlas_path, container, &asset);
        if (!r.ok)
        {
            error = r.error;
            return false;
        }

        if (!WriteWholeFile(out_path, container, error)) return false;

        // 图集页：运行时按页名相对 .zspg 所在目录找（SpineGpuRenderer）。输出不在图集旁边时把页拷过去，
        // 否则 cook 到 Assets/ 下的容器一加载就"读不到图集页"。同一个文件（没给 -o）不拷。
        const std::filesystem::path src_dir = atlas_path.parent_path();
        const std::filesystem::path dst_dir = out_path.parent_path();
        for (const std::string& page : asset.atlas_page_names)
        {
            const std::filesystem::path page_src = src_dir / PathFromUtf8(page);
            const std::filesystem::path page_dst = dst_dir / PathFromUtf8(page);
            std::error_code ec;
            if (std::filesystem::exists(page_dst, ec) && std::filesystem::equivalent(page_src, page_dst, ec))
            {
                continue;
            }
            ec.clear();
            std::filesystem::create_directories(page_dst.parent_path(), ec);
            ec.clear();
            std::filesystem::copy_file(page_src, page_dst, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                error = "拷图集页 " + PathToUtf8(page_src) + " → " + PathToUtf8(page_dst) + " 失败：" + ec.message();
                return false;
            }
        }
        return true;
    }

    // 没给 -atlas 时找同名图集。找不到就报错，不"尽力而为" ——
    // 用错图集算出来的 UV 是静默错的（图集带裁剪时偏一点，不报任何错）。
    //
    // 两种命名都要找：spine 编辑器导出的是 `<名>.atlas`；Unity 的 spine-unity 是
    // `<名>.atlas.txt`（.txt 才被 Unity 当文本资源收）。本工程 OSGame 782 个资产
    // 全是后者。
    bool ResolveAtlas(const std::filesystem::path& skeleton_path,
                      const std::filesystem::path& explicit_atlas,
                      std::filesystem::path&       out,
                      std::string&                 error)
    {
        if (!explicit_atlas.empty())
        {
            out = explicit_atlas;
            return true;
        }
        const std::filesystem::path stem = StripSkeletonSuffix(skeleton_path);
        std::error_code             ec;
        for (const char* ext : {".atlas", ".atlas.txt"})
        {
            std::filesystem::path candidate = stem;
            candidate += ext;
            if (std::filesystem::exists(candidate, ec) && !ec)
            {
                out = candidate;
                return true;
            }
        }
        error = "找不到同名图集（试过 " + PathToUtf8(stem) + ".atlas 与 " + PathToUtf8(stem)
                + ".atlas.txt；用 -atlas 显式指定）";
        return false;
    }

    bool IsSkeleton(const std::filesystem::path& p)
    {
        return KindOfSkeleton(p) != SkeletonKind::kNone;
    }

    // 交互分支里把路径塞给 Slate（UTF-8 文本）和把 UTF-8 文本变回路径，都用
    // ToolSuite.h 里那两个 —— 一份实现，批处理与 GUI 不会走岔。
}   // namespace

const ToolSuiteDesc& SpineCookSuite::Desc() const
{
    static const ToolSuiteDesc kDesc {
        "SpineCook",
        "Spine 容器导出",
        "Spine 骨架 + 图集 → .zspg 容器（GPU 蒙皮中间格式）",
        /* needs_project  = */ false,
        /* supports_batch = */ true,
    };
    return kDesc;
}

int SpineCookSuite::RunBatch(const std::vector<std::string>& args)
{
    // args 是 `-batchmode` 之后的部分；`-suite=SpineCook` 只用于选套件，这里跳过。
    std::string              input_arg;
    std::filesystem::path    out_arg;
    bool                     out_given  = false;
    std::filesystem::path    atlas_arg;
    bool                     recursive  = false;

    for (size_t i = 0; i < args.size(); ++i)
    {
        const std::string& a = args[i];
        if (a.rfind("-suite=", 0) == 0)
        {
            continue;
        }
        if ((a == "-atlas" || a == "--atlas") && i + 1 < args.size())
        {
            atlas_arg = PathFromUtf8(args[++i]);
            continue;
        }
        if (a.rfind("--atlas=", 0) == 0)
        {
            atlas_arg = PathFromUtf8(a.substr(std::strlen("--atlas=")));
            continue;
        }
        if (a == "-o" || a == "-out" || a == "--out")
        {
            if (i + 1 >= args.size())
            {
                std::fprintf(stderr, "%s 需要一个路径参数。\n\n%s", a.c_str(), kUsage);
                return 1;
            }
            out_arg   = PathFromUtf8(args[++i]);
            out_given = true;
            continue;
        }
        if (a == "--recursive" || a == "-r")
        {
            recursive = true;
            continue;
        }
        if (a == "-h" || a == "--help")
        {
            std::printf("%s", kUsage);
            return 0;
        }
        if (a.rfind("-", 0) == 0)
        {
            // 不认识的开关直接拒绝。吞掉它 = 用户以为生效了，实际没生效 ——
            // 批处理里这种错最贵（退出码还是 0）。
            std::fprintf(stderr, "不认识的开关：%s\n\n%s", a.c_str(), kUsage);
            return 1;
        }
        if (!input_arg.empty())
        {
            std::fprintf(stderr, "给了多个输入（'%s' 之后又有 '%s'）。\n\n%s",
                         input_arg.c_str(), a.c_str(), kUsage);
            return 1;
        }
        input_arg = a;
    }

    if (input_arg.empty())
    {
        std::fprintf(stderr, "缺少输入。\n\n%s", kUsage);
        return 1;
    }

    const std::filesystem::path input = PathFromUtf8(input_arg);
    std::error_code             ec;
    const bool                  input_is_dir = std::filesystem::is_directory(input, ec);

    // ---- 收集要处理的文件 ----
    std::vector<std::filesystem::path> inputs;
    if (input_is_dir)
    {
        if (recursive)
        {
            for (std::filesystem::recursive_directory_iterator it(input, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                if (it->is_regular_file() && IsSkeleton(it->path())) inputs.push_back(it->path());
            }
        }
        else
        {
            for (const auto& entry : std::filesystem::directory_iterator(input, ec))
            {
                if (entry.is_regular_file() && IsSkeleton(entry.path())) inputs.push_back(entry.path());
            }
        }
    }
    else
    {
        inputs.push_back(input);
    }

    if (inputs.empty())
    {
        std::fprintf(stderr, "输入里没有 .json / .skel / .skel.bytes：%s\n", PathToUtf8(input).c_str());
        return 1;
    }

    // ⚠️ 不能只靠 is_directory(out_arg)：输出目录通常**还不存在**（第一次跑），
    //    is_directory 返回 false ⇒ 每个骨架都写到同一个路径上，互相覆盖，
    //    而覆盖在批处理里退出码还是 0 —— 最难发现的那类事故。
    //   输入是目录 ⇒ 输出一定是目录（照相对结构铺开）；输入是单文件时才看
    //   out_arg 本身是不是一个已存在的目录。
    const bool out_is_dir = out_given && (input_is_dir || std::filesystem::is_directory(out_arg, ec));

    int ok_count   = 0;
    int fail_count = 0;

    // 最后一道防线：多个输入落到同一个输出路径 = 静默覆盖。宁可报错退出，
    // 也不给它一个 0 退出码（上面 out_is_dir 的判定已经挡掉绝大多数情况，
    // 这里是"判定漏了"时的兜底）。
    // （没给 -o 时每个骨架写在自己旁边，不会撞，不在防线范围内。）
    if (inputs.size() > 1 && out_given && !out_is_dir)
    {
        std::fprintf(stderr,
                     "%zu 个输入会写到同一个输出 %s；批量导出请给一个输出目录（-o <目录>）。\n",
                     inputs.size(),
                     PathToUtf8(out_arg).c_str());
        return 1;
    }

    for (const std::filesystem::path& skeleton : inputs)
    {
        std::filesystem::path out_path;
        if (!out_given)
        {
            // ⚠️ 走 StripSkeletonSuffix 而不是 replace_extension：
            // `X.skel.bytes` 要出 `X.zspg`，不是 `X.skel.zspg`。
            out_path = StripSkeletonSuffix(skeleton);
            out_path += ".zspg";
        }
        else if (out_is_dir)
        {
            // 目录 → 目录时保持相对结构：两个子目录里都叫 hero.json 是常态，
            // 铺平就只剩一个。
            std::error_code rel_ec;
            const std::filesystem::path rel = std::filesystem::relative(skeleton, input, rel_ec);
            // relative() 失败时退回"只用文件名"，至少不会把绝对路径拼到 out_arg 上
            // （`out_arg / 绝对路径` 在 Windows 上直接变成那个绝对路径 ⇒ 写到源目录里）。
            const std::filesystem::path base = rel_ec ? std::filesystem::path(skeleton.filename()) : rel;
            out_path = out_arg / StripSkeletonSuffix(base);
            out_path += ".zspg";
        }
        else
        {
            out_path = out_arg;
        }

        std::filesystem::path atlas_path;
        std::string           error;
        if (!ResolveAtlas(skeleton, atlas_arg, atlas_path, error) ||
            !CookOne(skeleton, atlas_path, out_path, error))
        {
            std::fprintf(stderr, "  [FAIL] %s\n", error.c_str());
            ++fail_count;
            continue;
        }

        std::printf("  [OK] %s -> %s\n", PathToUtf8(skeleton).c_str(), PathToUtf8(out_path).c_str());
        ++ok_count;
    }

    std::printf("SpineCook: %d 成功，%d 失败。\n", ok_count, fail_count);
    // ⚠️ 有失败就非 0 —— 批处理里"退出码 0 但少导了两个"是最难查的事故。
    return fail_count == 0 ? 0 : 1;
}

int SpineCookSuite::RunInteractive(const std::filesystem::path& /*project_file*/,
                                   const std::vector<std::string>& /*args*/)
{
#ifdef _WIN32
    using namespace ZSlate;

    auto in_box = std::make_shared<SEditableTextBox>();
    in_box->HintText = "骨架（.json / .skel）";
    in_box->MinWidth = 520.0f;

    auto out_box = std::make_shared<SEditableTextBox>();
    out_box->HintText = "输出 .zspg（留空 ⇒ 与骨架同目录同名）";
    out_box->MinWidth = 520.0f;

    auto status = std::make_shared<STextBlock>();
    status->FontSize = 13.0f;
    status->Color    = UIColor(0.62f, 0.78f, 0.62f, 1.0f);

    FStandaloneAppDesc desc;
    desc.Title  = "ZEditor — Spine 容器导出";
    desc.Width  = 760.0f;
    desc.Height = 320.0f;
    desc.Font   = Win32::ESystemFont::Sans;

    FSlateStandaloneApp app;
    if (!app.Initialize(desc))
    {
        std::fprintf(stderr, "SpineCook: 建不了窗口。命令行请用 -batchmode。\n");
        return 1;
    }

    auto make_text = [](const std::string& text, float size, const UIColor& colour)
    {
        auto t = std::make_shared<STextBlock>();
        t->Text     = text;
        t->FontSize = size;
        t->Color    = colour;
        return t;
    };

    auto make_button = [](const std::string& text, std::function<void()> on_clicked)
    {
        auto b = std::make_shared<SButton>();
        auto l = std::make_shared<STextBlock>();
        l->Text     = text;
        l->FontSize = 14.0f;
        l->Color    = Colors::White;
        b->SetContent(l);
        b->OnClicked = std::move(on_clicked);
        return b;
    };

    auto root = std::make_shared<SVerticalBox>();
    root->AddSlot(make_text("Spine 容器导出", 22.0f, Colors::White));
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 6)));
    root->AddSlot(make_text("骨架 + 同名图集 → .zspg。命令行批量导出用 -batchmode。",
                            12.0f, UIColor(0.60f, 0.63f, 0.70f, 1.0f)));
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 16)));
    root->AddSlot(in_box);
    root->AddSlot(make_button("浏览骨架…",
                              [&app, in_box]()
                              {
                                  wchar_t buffer[4096] = {};
                                  OPENFILENAMEW ofn {};
                                  ofn.lStructSize  = sizeof(ofn);
                                  ofn.hwndOwner    = app.GetHwnd();
                                  ofn.lpstrFile    = buffer;
                                  ofn.nMaxFile     = 4096;
                                  ofn.lpstrFilter  = L"Spine 骨架 (*.json;*.skel)\0*.json;*.skel\0"
                                                     L"所有文件 (*.*)\0*.*\0";
                                  ofn.nFilterIndex = 1;
                                  ofn.lpstrTitle   = L"打开 Spine 骨架";
                                  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
                                  if (!GetOpenFileNameW(&ofn)) return;
                                  in_box->SetText(PathToUtf8(std::filesystem::path(buffer)));
                              }));
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 10)));
    root->AddSlot(out_box);
    root->AddSlot(std::make_shared<SSpacer>(Vector2(0, 16)));
    root->AddSlot(make_button("导出",
                              [in_box, out_box, status]()
                              {
                                  const std::string in_text = in_box->GetText();
                                  if (in_text.empty())
                                  {
                                      status->Color = UIColor(0.92f, 0.52f, 0.48f, 1.0f);
                                      status->Text  = "先选一个骨架";
                                      return;
                                  }
                                  const std::filesystem::path in = PathFromUtf8(in_text);
                                  const std::string           out_text = out_box->GetText();
                                  // `X.skel.bytes` → `X.zspg`（replace_extension 会得到 `X.skel.zspg`）
                                  std::filesystem::path out =
                                      out_text.empty() ? SpineGpuCookedPathFor(in) : PathFromUtf8(out_text);

                                  std::filesystem::path atlas;
                                  std::string           error;
                                  if (!ResolveAtlas(in, std::filesystem::path(), atlas, error) ||
                                      !CookOne(in, atlas, out, error))
                                  {
                                      status->Color = UIColor(0.92f, 0.52f, 0.48f, 1.0f);
                                      status->Text  = error;
                                      return;
                                  }
                                  status->Color = UIColor(0.62f, 0.78f, 0.62f, 1.0f);
                                  status->Text  = "已导出 " + PathToUtf8(out);
                              }));
    root->AddSlot(status);

    app.SetRootContent(root);
    app.Run();
    app.Shutdown();
    return 0;
#else
    std::fprintf(stderr, "SpineCook: 交互模式目前只在 Windows 上可用；其它平台请用 -batchmode。\n");
    return 1;
#endif
}

namespace
{
    // 自注册。静态对象在 main() 之前构造；注册表容器是函数内静态，
    // 第一次访问时一定已经构造好了（见 ToolSuiteRegistry.cpp）。
    struct SpineCookRegistrar
    {
        SpineCookRegistrar() { RegisterToolSuite(std::make_unique<SpineCookSuite>()); }
    };
    const SpineCookRegistrar g_spine_cook_registrar;
}   // namespace
}   // namespace ZEditor
