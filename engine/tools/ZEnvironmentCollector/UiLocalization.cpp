// ============================================================================
// UiLocalization - see UiLocalization.h
// ============================================================================
// Two sources for the same table, selected by ZENVCHECK_USE_ZRUNTIME:
//
//   * defined  -- the Simplified Chinese strings are DATA, not code: they are
//     authored in resource/ui_strings_zh-CN.csv, baked into
//     resource/ui_strings_zh-CN.zasset by DataTableCook at build time, and read
//     back here through the engine's C ABI. Editing a label is a CSV edit; no
//     .cpp changes, no recompilation of the table, and a translator never has
//     to touch C++ string literals.
//
//   * undefined -- the standalone configure has no ZRuntimeShared, so it keeps
//     the table compiled into this translation unit. Both builds therefore stay
//     buildable; only the shipped (full-tree) one is table-driven.
//
// Either way the ids here must match the ids passed to ZSlate::Loc() in
// EnvironmentCheckGui.cpp. A mismatch is not a compile error -- Loc() falls back
// to the English literal and reports the missing id once through ZSlate's sink,
// which is why the source CSV lists the ids in the same order as they appear in
// the UI.
//
// Report file content stays Chinese on purpose -- those files go to support,
// not to the player -- so this table covers the interface only.

#include "UiLocalization.h"

#include <windows.h>

#if defined(ZENVCHECK_USE_ZRUNTIME)
    #include "Runtime/ZEngineApi/ZEngineToolApi.h"

    #include "Collection.h"  // ExecutableDirectory()

    #include <filesystem>
    #include <string>
    #include <vector>
#endif

namespace zengine::envcheck::ui
{

namespace
{
    constexpr const wchar_t* kSettingsKey = L"Software\\ZEngine\\EnvironmentCollector";
    constexpr const wchar_t* kLanguageValue = L"Language";

#if defined(ZENVCHECK_USE_ZRUNTIME)
    // One table per language, named after ZSlate's own language id
    // (ZSlate::LanguageId(): "en", "zh-CN"). English has no table on purpose --
    // it is the literal written at the call site.
    constexpr const char* kTableFileName = "ui_strings_zh-CN.zasset";

    struct LoadedTable
    {
        std::vector<std::string> ids;
        std::vector<std::string> texts;
        std::vector<UiStringEntry> entries;
    };

    // Built once, on first use: the table has to outlive the caller (ZSlate keeps
    // the `const char*`s we hand it), so it cannot be a local.
    LoadedTable& Table()
    {
        static LoadedTable table;
        static bool loaded = false;
        if (loaded) return table;
        loaded = true;

        const std::filesystem::path path = collector::ExecutableDirectory() / "resource" / kTableFileName;
        if (ZRuntime_LoadLocalizationTable(path.string().c_str()) == 0)
        {
            // Not fatal: the UI degrades to English, and the engine log already
            // says why. A diagnostic tool must still run on a machine where its
            // own resource directory was damaged.
            return table;
        }

        const unsigned count = ZRuntime_LocalizationRowCount();
        table.ids.reserve(count);
        table.texts.reserve(count);
        for (unsigned index = 0; index < count; ++index)
        {
            const char* id = nullptr;
            const char* text = nullptr;
            if (ZRuntime_LocalizationRowAt(index, &id, &text) != 0)
            {
                table.ids.emplace_back(id);
                table.texts.emplace_back(text);
            }
        }

        // Pointers are taken only after both vectors have stopped growing, so a
        // reallocation cannot invalidate an entry we already published.
        table.entries.reserve(table.ids.size());
        for (std::size_t index = 0; index < table.ids.size(); ++index)
        {
            table.entries.push_back({table.ids[index].c_str(), table.texts[index].c_str()});
        }
        return table;
    }
#else
    // clang-format off
    constexpr UiStringEntry kChineseSimplified[] = {
        // Shell / tabs
        {"tab.collection", "环境采集"},
        {"tab.compatibility", "兼容性检查"},
        {"tab.gpu", "GPU 选卡验证"},
        {"window.title", "环境信息采集 - ZSlate"},

        // Collection tab
        {"collection.title", "Windows 环境信息采集"},
        {"collection.project", "项目："},
        {"collection.intro", "采集硬件、BIOS、驱动、系统事件、已有内存检测结果，并按项目配置附带最新游戏日志。"},
        {"collection.safety", "不上传、不启动游戏、不压测、不重启、不修改BIOS。加载探针会运行GameCore——在隔离子进程里，不在本进程。"},
        {"collection.gamedir", "游戏目录（可选；游戏 DLL 所在处，用于静态检查与加载探针）"},
        {"collection.gamedir.hint", "例如 C:\\Program Files\\TheGame"},
        {"collection.kind", "问题类型"},
        {"collection.kind.crash", "闪退"},
        {"collection.kind.hang", "卡住"},
        {"collection.kind.unknown", "不确定"},
        {"collection.time", "发生时间（可留空自动找记录；输入需带时区）"},
        {"collection.time.fill", "填写当前时间"},
        {"collection.events", "同意附带事件明细"},
        {"collection.start", "开始采集并生成 ZIP"},
        {"collection.cancel", "取消采集（打包阶段不可取消）"},
        {"collection.ready", "就绪。报告保存在桌面的 EnvironmentReports 文件夹。"},
        {"collection.open", "打开报告所在文件夹"},
        {"collection.open.failed", "无法打开目录，请检查访问权限。"},
        {"collection.after", "完成后确认报告内容，将 ZIP 手动发给研发。"},
        {"collection.disclaimer", "未发现异常不代表硬件健康；部分采集失败会记入报告。"},

        // Capture (advanced diagnostics)
        {"capture.section", "高级诊断：捕获一次游戏崩溃 · "},
        {"capture.privacy", "完整内存可能包含账号和聊天信息；仅本地保存，请勿公开分享。"},
        {"capture.mode.wer", "Windows系统崩溃转储（推荐）"},
        {"capture.mode.procdump", "ProcDump附加（需允许调试）"},
        {"capture.wer.note", "WER仅配置当前exe名称；需要UAC，一次启用可连续收集多次崩溃，点“安全停止捕获”后才恢复，不附加游戏。"},
        {"capture.procdump.note", "下方ProcDump下载与选择只用于ProcDump模式。"},
        {"capture.procdump.download", "打开微软ProcDump下载及许可页面"},
        {"capture.procdump.pick", "选择 procdump64.exe"},
        {"capture.procdump.reselect", "重新选择 ProcDump（已记住位置）"},
        {"capture.procdump.selected", "ProcDump已选择（启动前验证签名）"},
        {"capture.procdump.dialog", "选择微软官方 procdump64.exe"},
        {"capture.target.pick", "选择游戏程序（当前项目目标）"},
        {"capture.target.dialog", "选择当前项目的游戏exe，不是启动器"},
        {"capture.target.prefix", "目标："},
        {"capture.start", "开始捕获崩溃（按所选方式）"},
        {"capture.import", "选择已有dump并打包（不改系统配置）"},
        {"capture.import.dialog", "选择已生成的完整dump"},
        {"capture.stop", "安全停止捕获 / 取消后处理"},
        {"capture.restore", "恢复当前项目WER原配置（含上次未完成会话）"},
        {"capture.status.pending", "发现未恢复的WER会话，请先点击恢复原配置。"},
        {"capture.status.idle", "未开启；默认等待最多1小时，启用成功不保证系统一定产生dump。"},
        {"capture.open", "打开捕获结果（定位ZIP或capture.json）"},
        {"capture.open.empty", "本窗口还没有捕获报告；历史结果请到EnvironmentReports内的crash-capture-*目录查找。"},
        {"capture.open.failed", "无法打开捕获目录，请到EnvironmentReports/crash-capture-*查找capture.json。"},
        {"capture.preparing", "准备捕获"},
        {"capture.need.target", "请先选择游戏程序。"},
        {"capture.need.both", "请先选择ProcDump和游戏程序。"},
        {"capture.need.profile", "当前profile没有目标exe，请使用具体项目配置。"},
        {"capture.forget.failed", "（未能记住ProcDump位置，不影响取证结果）"},

        // Capture confirmation dialogs
        {"capture.consent.title", "完整内存采集授权"},
        {"capture.consent.import",
         "确认这个完整dump来自所选游戏。导入只验证结构及模块路径，不能证明它属于本次运行。\n"
         "可能包含账号、聊天和令牌，占用数GB空间，不自动上传，不删除原文件。\n\n是否确认来源并同意打包？"},
        {"capture.consent.wer.prefix",
         "将仅配置HKLM LocalDumps下的 "},
        {"capture.consent.wer.suffix",
         "，DumpType=2，输出到本次报告的system-dumps私有目录。\n"
         "配置按exe名称生效，可能影响其它同名程序；需要UAC授权，结束收集时再次请求UAC恢复原设置。\n"
         "一次启用可连续收集本次会话内的多次崩溃（每份数GB，最多8份）；点击“安全停止捕获”后才结束并统一恢复配置，未点击前配置一直生效。\n"
         "完整内存含敏感数据，占用数GB，不自动上传。配置成功不保证产生dump。\n"
         "不附加游戏，不终止WER；异常退出可用恢复按钮处理。\n\n是否同意？"},
        {"capture.consent.procdump",
         "仅监控所选游戏。完整内存含账号、聊天等敏感数据，占用数GB，不自动上传。\n"
         "停止时等待ProcDump安全退出，不强杀游戏或调试器。\n\n是否同意？"},
        {"capture.license.title", "Sysinternals许可确认"},
        {"capture.license.body",
         "请先阅读微软Sysinternals许可（下载页及ProcDump自带Eula.txt）。\n"
         "选择“是”表示你已阅读并接受，采集器将以 -accepteula 参数调用你选择的ProcDump。\n"
         "工具不包含ProcDump，也不代替你取得第三方使用许可。\n\n是否接受该许可？"},

        // WER restore / close guard
        {"capture.restore.dialog.title", "恢复系统捕获配置"},
        {"capture.restore.dialog.body",
         "恢复当前项目exe的WER备份，可能需要UAC授权。\n只恢复采集器保存的原值，不删除dump；检测到其他工具修改时拒绝覆盖。\n\n继续？"},
        {"capture.restore.progress", "等待恢复配置授权"},
        {"capture.restore.ok", "WER配置已恢复（或没有待恢复备份），原dump保留。"},
        {"capture.restore.failed.prefix", "恢复未完成：Win32="},
        {"capture.restore.failed.suffix", "。可能UAC被取消或配置存在冲突，备份保留。"},
        {"capture.close.title", "系统捕获配置尚未恢复"},
        {"capture.close.body",
         "WER配置仍有未恢复备份，后续同名程序崩溃可能继续生成dump。\n建议返回并点击恢复原配置。仍要退出？"},

        // Compatibility tab
        {"check.title", "Windows 环境兼容性检查"},
        {"check.intro", "与 -batchmode 默认命令同一条流水线：硬件快照、双显卡选卡验证、DLL静态检查与隔离加载探针。"},
        {"check.output", "报告目录"},
        {"check.output.hint", "留空则使用桌面的 EnvironmentReports"},
        {"check.isa", "产品最低指令集（none 或 avx,avx2,fma,f16c）"},
        {"check.memory", "最低物理内存（GB）"},
        {"check.dll", "包含递归DLL静态检查与隔离加载探针（可能较慢）"},
        {"check.start", "开始检查并生成报告"},
        {"check.cancel", "取消检查"},
        {"check.ready", "就绪。"},
        {"check.isa.invalid", "指令集填写不正确，只能填 none 或 avx,avx2,fma,f16c 的组合。"},
        {"check.memory.invalid", "最低内存必须是整数GB。"},
        {"check.done.prefix", "检查完成，退出码 "},
        {"check.done.suffix", "。报告："},

        // GPU tab
        {"gpu.title", "GPU 自动选卡验证"},
        {"gpu.intro", "与 GfxDeviceSetup.cpp 的选卡逻辑逐字节一致；不链接引擎或游戏，不启动游戏。"},
        {"gpu.intro2", "上半部分是离线回归用例，下半部分是本机真实探测（改BIOS开关独显后点重新探测可复查）。"},
        {"gpu.reprobe", "重新探测"},
        {"gpu.ready", "就绪。"},
        {"gpu.section.mock", "== 离线回归用例 =="},
        {"gpu.section.real", "== 本机真实探测 =="},
        {"gpu.all.adapters", "所有适配器（EnumAdapters1）："},
        {"gpu.regression.pass", "回归：全部通过"},
        {"gpu.regression.fail", "回归：存在失败（选卡逻辑可能与 GfxDeviceSetup.cpp 不一致）"},
        {"gpu.preference.picked", "EnumAdapterByGpuPreference(HIGH_PERFORMANCE) 选中："},
        {"gpu.preference.failed", "EnumAdapterByGpuPreference 不可用或失败："},
        {"gpu.reason.unknown", "未知原因"},
        {"gpu.status.pass", "探测完成，回归全部通过。"},
        {"gpu.status.fail", "探测完成，回归存在失败，请把本页截图发给研发。"},

        // Status line and report view
        {"status.preparing", "准备中"},
        {"status.cancelled", "已取消，已生成包含部分信息的ZIP。"},
        {"status.partial", "ZIP已生成；部分步骤失败，请查看manifest。"},
        {"status.done", "ZIP已生成。请检查报告后手动发送。"},
        {"status.collect.failed", "采集或打包失败，已写出的本地文件会保留。请检查输出目录。"},
        {"status.time.invalid", "时间格式不正确，请输入带Z或+08:00的ISO时间。"},
        {"status.time.future", "发生时间不能在未来。"},
        {"report.truncated", "… 报告过长，已截断显示；完整内容见同目录 OSGameEnvironmentReport.json。"},

        // Incident anchor: without an incident time, an incident kind and the
        // game exe, a package cannot say whether the game caused anything. The
        // form says so before the ZIP exists rather than letting the developer
        // find out days later.
        {"collection.anchor.warning", "本包缺少以下内容，无法用于事故归因："},
        {"collection.anchor.missing.time", "事故发生时间"},
        {"collection.anchor.missing.kind", "现象类型（崩溃还是卡死）"},
        {"collection.anchor.missing.game", "游戏 exe（当前 profile 未配置）"},
        {"collection.confirm.anchor.title", "缺少事故锚点"},
        {"collection.confirm.anchor.body",
         "这次采集缺少事故时间、现象类型或游戏 exe，拿到的包无法回答「是不是游戏导致的」，只能回答「这台机器现在是什么状态」。\n"
         "如果你是要发给研发排查崩溃或卡死，请先返回补齐这三项。\n\n仍然采集一个不可归因的包？"},

        // Start-up failures
        {"dialog.gpuinit.title", "图形初始化失败"},
        {"dialog.gpuinit.body", "图形界面启动失败。可运行同目录 ZEnvironmentCollector.exe -batchmode --collect 生成报告。"},
        {"dialog.startup.title", "环境采集"},
        {"dialog.startup.body", "无法启动采集界面。请检查项目配置、系统字体及工具分发是否完整。可使用 -batchmode --collect 模式。"},
    };
    // clang-format on
#endif
}

const UiStringEntry* ChineseSimplifiedStrings(std::size_t& out_count)
{
#if defined(ZENVCHECK_USE_ZRUNTIME)
    const LoadedTable& table = Table();
    out_count = table.entries.size();
    return table.entries.empty() ? nullptr : table.entries.data();
#else
    out_count = sizeof(kChineseSimplified) / sizeof(kChineseSimplified[0]);
    return kChineseSimplified;
#endif
}

std::string LoadLanguagePreference()
{
    wchar_t value[64]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kLanguageValue, RRF_RT_REG_SZ,
                     nullptr, value, &size) != ERROR_SUCCESS)
        return {};
    std::string text;
    for (const wchar_t* c = value; *c; ++c) text.push_back(static_cast<char>(*c));
    return text;
}

void SaveLanguagePreference(const std::string& languageId)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const std::wstring text(languageId.begin(), languageId.end());
    RegSetValueExW(key, kLanguageValue, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(text.c_str()),
                   static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

} // namespace zengine::envcheck::ui
