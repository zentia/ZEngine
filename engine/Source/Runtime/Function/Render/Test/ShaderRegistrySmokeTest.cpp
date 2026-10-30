// =============================================================================
// ShaderRegistry 冒烟测试：路径「存储形式 / 查找 key」拆分。
//
//   ShaderRegistrySmokeTest  -DZENGINE_BUILD_SHADER_REGISTRY_SMOKE_TEST=ON
//
//   cmake -B build -DZENGINE_BUILD_SHADER_REGISTRY_SMOKE_TEST=ON
//   cmake --build build --target ShaderRegistrySmokeTest
//   ctest --test-dir build -C Debug -R ShaderRegistrySmokeTest
//
// 为什么单独写这个测试
// --------------------
// ShaderRegistry 此前**零测试覆盖**。它管的是 shader 的身份：
// `m_Guid` 被 Material 存进资产里，用来回查 `Shaders/*.shader` 的源和它烘焙出来的
// `Assets/_Generated/Shaders/*.zasset`（Material.cpp:44-50）。所以这个注册表出问题的
// 形态不是崩溃，是**引用静默断链** —— 编译得过、跑得起来、材质变粉。
//
// 这里钉的是三件事：
//
//   A. m_SourceRelPath / m_ZassetRelPath 保留磁盘上的原大小写。
//      它们会被拼到工程根上去开文件（Material.cpp / InspectorShaderInspector.cpp），
//      在区分大小写的文件系统上折叠大小写 = 文件打不开。
//
//   B. m_Guid 仍以**查找 key**（Windows 上小写）为种子，逐字节不变。
//      这是整个改动唯一真正危险的地方：一旦顺势改用保留大小写的存储形式做种子，
//      链接不报错、只断言 size()==32 也照样绿，等发现时所有材质的 shader 引用早就断了。
//      所以这里用的是**独立复算出来的字面量**，不是"再调一次同一个函数"。
//
//   C. 旧版注册表（小写 path）能在下次 rescan 时自愈回磁盘大小写，且 guid 不动。
//
// 顺带钉住的弱断言
// ----------------
// ⚠️ m_ZassetRelPath 的大小写在 Windows 上会被 std::filesystem::relative() 内部的
// weakly_canonical 还原回来，所以单看它抓不到回归。真正守门的是 A 组的源路径断言。
// =============================================================================

#include "Runtime/Function/Render/ShaderRegistry.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Command/CommandSystem.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/RegisterRuntime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int g_Failures = 0;
    int g_Checks = 0;

    void Check(bool ok, const char* id, const char* what, const std::string& got = {},
               const std::string& want = {})
    {
        ++g_Checks;
        if (ok)
        {
            std::printf("  [ ok ] %-6s %s", id, what);
            if (!got.empty())
            {
                std::printf("  got='%s'", got.c_str());
            }
            std::printf("\n");
        }
        else
        {
            ++g_Failures;
            std::printf("  [FAIL] %-6s %s  got='%s' want='%s'\n",
                        id, what, got.c_str(), want.c_str());
        }
    }

    void CheckEq(const char* id, const char* what, const std::string& got, const std::string& want)
    {
        Check(got == want, id, what, got, want);
    }

    bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return out.good();
    }

    bool ReadTextFile(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return false;
        }
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    std::string Lower(const std::string& in)
    {
        std::string s = in;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // 混合大小写的目录名 + 文件名：任何一处被折叠都会被 A 组抓到。
    constexpr const char* kShaderRel = "Shaders/PBR/StandardPBR.shader";
    constexpr const char* kShaderName = "Custom/StandardPBR";

    // 期望的 guid：**独立复算出来的字面量**（Python 抄一遍 FNV-1a 双种子算法），
    // 不是"再调一次 DeterministicGuidFromPath" —— 后者在被测代码被改错时也会跟着错。
    //
    // 两个值是因为 key 的定义本身平台相关：Windows 折叠大小写，其它平台不折叠。
    // 无论哪个平台，guid 都必须等于**改动之前**那一版算出来的值。
#if defined(_WIN32)
    constexpr const char* kExpectedGuid = "fef5aeac4ea780091dcc18c0a552d164";
    constexpr const char* kLegacyStoredPath = "shaders/pbr/standardpbr.shader";
#else
    constexpr const char* kExpectedGuid = "ac4fe0458074a0899c94762734cdcd24";
    constexpr const char* kLegacyStoredPath = kShaderRel;
#endif

    // 第二个 shader：**旧注册表里没有**，所以它走的才是真正"新扫描 + 派生 guid"
    // 那条路。没有它，B 组断言只在读回 JSON 里那个 guid —— 牙齿检查证明过：把
    // 派生种子改成存储形式，测试照样全绿（因为那条路根本没跑）。
    constexpr const char* kShaderRel2 = "Shaders/Utility/UnlitColor.shader";
    constexpr const char* kShaderName2 = "Custom/UnlitColor";
#if defined(_WIN32)
    constexpr const char* kExpectedGuid2 = "0daaf6e2505d712bf5c6f2869e240222";
#else
    constexpr const char* kExpectedGuid2 = "f7b7fb980b038a2bb8b74e7dc480cba2";
#endif
}  // namespace

int main(int argc, char** argv)
{
    // 这个进程的 stdout 会被 ctest 抓走，行缓冲会让输出顺序和日志混在一起。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string resource_dir;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--resource-dir") == 0)
        {
            resource_dir = argv[i + 1];
        }
    }

    std::printf("=== ShaderRegistry 路径大小写冒烟测试 ===\n");

    // ---- 1. 造一个临时工程 ------------------------------------------------
    std::error_code ec;
    const std::filesystem::path scratch =
        std::filesystem::weakly_canonical(std::filesystem::temp_directory_path() /
                                              "zengine_shader_registry_smoke",
                                          ec);
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    if (!WriteTextFile(scratch / "Shaders" / "PBR" / "StandardPBR.shader",
                       "Shader \"" + std::string(kShaderName) + "\"\n"
                       "{\n"
                       "    Properties { _Color(\"Color\", Color) = (1,1,1,1) }\n"
                       "    SubShader { Pass { } }\n"
                       "}\n"))
    {
        std::printf("FATAL: 写不出 shader 源文件。\n");
        return 77;
    }
    if (!WriteTextFile(scratch / "Shaders" / "Utility" / "UnlitColor.shader",
                       "Shader \"" + std::string(kShaderName2) + "\"\n"
                       "{\n"
                       "    Properties { _Color(\"Color\", Color) = (1,1,1,1) }\n"
                       "    SubShader { Pass { } }\n"
                       "}\n"))
    {
        std::printf("FATAL: 写不出第二个 shader 源文件。\n");
        return 77;
    }

    // kZassetWant 全小写 —— 旧版写在磁盘上的样子（Material.cpp 就是拿它去开文件的）。
    const std::string kLegacyZasset =
        "assets/_generated/shaders/pbr/standardpbr.shader.zasset";

    // 一份**旧版**注册表：Windows 上 path 是小写（改动之前的写法），guid 由当时的
    // key 派生。见文件头 C 组。
    const std::filesystem::path registry_file = scratch / "AssetRegistry" / "shader_registry.json";
    if (!WriteTextFile(registry_file,
                       "{\n  \"version\": 1,\n  \"entries\": [\n"
                       "    { \"guid\": \"" + std::string(kExpectedGuid) + "\",\n"
                       "      \"path\": \"" + std::string(kLegacyStoredPath) + "\",\n"
                       "      \"zasset\": \"" + kLegacyZasset + "\",\n"
                       "      \"name\": \"" + kShaderName + "\" }\n"
                       "  ]\n}\n"))
    {
        std::printf("FATAL: 写不出旧版注册表。\n");
        return 77;
    }

    // ---- 2. 起系统 --------------------------------------------------------
    // 与 ScriptComponentSmokeTest 同一套：只注册 CommandSystem / ProjectInfo /
    // ShaderRegistry + RegisterCore()。CommandSystem::Initialize() 会从 argv 解析
    // -p/--project，测试进程没这个参数，它会把 project_path 冲成空、整个
    // InitializeAll 回滚 —— 所以手工填好并标成"已初始化"让它跳过。
    WriteTextFile(scratch / "ShReg.zproject",
                  "name: ShReg\n"
                  "version: 1.0.0\n"
                  "shaders_dir: Shaders\n");

    RegisterCore();
    REGISTER_SYSTEM(CommandSystem);
    REGISTER_SYSTEM(ProjectInfo);
    REGISTER_SYSTEM(ShaderRegistry);

    GET_SYSTEM(CommandSystem)->GetProjectPath() = scratch / "ShReg.zproject";
    GET_SYSTEM(CommandSystem)->GetWorkingDir() = scratch;
    GET_SYSTEM(CommandSystem)->SetInitialized(true);

    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: SystemRegistry::InitializeAll(false) 失败。\n");
        return 77;
    }

    auto* registry = GET_SYSTEM(ShaderRegistry);
    if (registry == nullptr)
    {
        std::printf("FATAL: ShaderRegistry 没起来。\n");
        return 77;
    }
    if (GET_SYSTEM(ProjectInfo)->GetProjectRoot().empty())
    {
        std::printf("FATAL: ProjectInfo 的工程根是空的，ShaderRegistry 会走 idle 分支。\n");
        return 77;
    }

    // ---- A. 大小写保留 ----------------------------------------------------
    std::printf("\n--- A. 存储路径保留磁盘原大小写 ---\n");
    auto all = registry->GetAll();
    ShaderRegistryEntry* entry = nullptr;
    for (auto* e : all)
    {
        if (Lower(e->m_SourceRelPath.c_str()) == Lower(kShaderRel))
        {
            entry = e;
        }
    }
    Check(entry != nullptr, "A1", "shader 被扫进注册表");
    if (entry == nullptr)
    {
        return 1;
    }
    CheckEq("A2", "源路径保留磁盘原大小写（旧注册表已被自愈）",
            entry->m_SourceRelPath.c_str(), kShaderRel);
    Check(entry->m_SourceRelPath.c_str() != Lower(entry->m_SourceRelPath.c_str()),
          "A3", "源路径没有被折叠成小写");
    CheckEq("A4", "ShaderLab 名字被解析出来", entry->m_ShaderName.c_str(), kShaderName);

    // m_ZassetRelPath 会被 Material.cpp 拼到工程根上去开文件，大小写同样要保住。
    const std::string kZassetWant = "Assets/_Generated/Shaders/PBR/StandardPBR.shader.zasset";
    CheckEq("A5", "zasset 路径保留原大小写", entry->m_ZassetRelPath.c_str(), kZassetWant);
    Check(entry->m_ZassetRelPath.c_str() != Lower(entry->m_ZassetRelPath.c_str()),
          "A6", "zasset 路径没有被折叠成小写");

    // ---- B. GUID 以 key 为种子，逐字节不变 ---------------------------------
    std::printf("\n--- B. guid 与改动前逐字节一致 ---\n");
    CheckEq("B1", "guid 等于改动前那一版算出的值（独立复算的字面量）",
            entry->m_Guid.c_str(), kExpectedGuid);
#if defined(_WIN32)
    // Windows 上 key 是折叠过的，所以存储形式做种子必然得到另一个值。
    // （其它平台 key == 存储形式，这条不成立，因此不跑。）
    CheckEq("B2", "guid 不等于用保留大小写路径做种子的值",
            entry->m_Guid.c_str() ==
                    ShaderRegistry::DeterministicGuidFromPath(eastl::string(kShaderRel))
                ? "same"
                : "different",
            "different");
#endif

    // ---- C. 旧注册表自愈并落盘 ---------------------------------------------
    std::printf("\n--- C. 自愈结果落盘 ---\n");
    std::string registry_text;
    ReadTextFile(registry_file, registry_text);
    Check(registry_text.find(kShaderRel) != std::string::npos,
          "C1", "注册表文件里已是磁盘真实大小写");
    Check(registry_text.find(Lower(kShaderRel)) == std::string::npos,
          "C2", "注册表文件里不再残留小写源路径（非 Windows 上本就无差别，恒真）");
    Check(registry_text.find(kZassetWant) != std::string::npos,
          "C3", "zasset 路径也自愈并落盘");
    Check(registry_text.find(kLegacyZasset) == std::string::npos,
          "C4", "注册表文件里不再残留小写 zasset 路径");

    // ---- D. 查找仍然大小写不敏感（Windows）---------------------------------
    std::printf("\n--- D. 查找不受拼写影响 ---\n");
    Check(registry->FindByPath(eastl::string(kShaderRel)) != nullptr,
          "D1", "按原大小写 FindByPath 命中");
    Check(registry->FindByPath(eastl::string(Lower(kShaderRel).c_str())) != nullptr,
          "D2", "按全小写 FindByPath 也命中");
    Check(registry->FindByGuid(entry->m_Guid) != nullptr, "D3", "FindByGuid 命中");
    Check(registry->FindByName(eastl::string(kShaderName)) != nullptr, "D4", "FindByName 命中");

    // ---- F. 新扫描条目的 guid 也必须是字面量（这条才真的走到派生代码）--------
    // 牙齿检查的教训：只有从旧注册表读回来的条目时，B 组只是在回读我种进去的值，
    // 派生代码压根没跑。第二个 shader 不在旧注册表里，才覆盖得到。
    std::printf("\n--- F. 新扫描条目的 guid 由 key 派生 ---\n");
    ShaderRegistryEntry* fresh = nullptr;
    for (auto* e : registry->GetAll())
    {
        if (Lower(e->m_SourceRelPath.c_str()) == Lower(kShaderRel2))
        {
            fresh = e;
        }
    }
    Check(fresh != nullptr, "F1", "新 shader 被扫进注册表");
    if (fresh == nullptr)
    {
        return 1;
    }
    CheckEq("F2", "新条目 guid 等于独立复算的字面量", fresh->m_Guid.c_str(), kExpectedGuid2);
    CheckEq("F3", "新条目源路径保留原大小写", fresh->m_SourceRelPath.c_str(), kShaderRel2);
    CheckEq("F4", "新条目 zasset 路径保留原大小写", fresh->m_ZassetRelPath.c_str(),
            std::string("Assets/_Generated/Shaders/Utility/UnlitColor.shader.zasset"));

    // ---- E. 重新 LoadFromDisk 后仍然稳定（不来回抖）--------------------------
    std::printf("\n--- E. 二次启动稳定 ---\n");
    registry->Shutdown();
    if (!registry->Initialize())
    {
        std::printf("FATAL: 二次 Initialize 失败。\n");
        return 1;
    }
    auto* again = registry->FindByPath(eastl::string(kShaderRel));
    Check(again != nullptr, "E1", "二次启动后仍查得到");
    if (again != nullptr)
    {
        CheckEq("E2", "二次启动后 guid 不变", again->m_Guid.c_str(), kExpectedGuid);
        CheckEq("E3", "二次启动后路径仍是磁盘大小写", again->m_SourceRelPath.c_str(), kShaderRel);
    }

    std::printf("\n========================================\n");
    std::printf("总计 %d 项检查，失败 %d 项\n", g_Checks, g_Failures);
    std::printf("========================================\n");
    return g_Failures == 0 ? 0 : 1;
}
