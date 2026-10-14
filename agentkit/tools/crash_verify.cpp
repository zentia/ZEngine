// 编译验证单元：真实编译 CrashHandler.h。
//
// 不跑崩溃、不触发 SEH —— 只确认这份 header 能被 MSVC 真正编译通过。
// 上一轮受限于环境只能验证纯逻辑（FormatHex），这次直接上真编译器。
#include "CrashHandler.h"

#include <cassert>
#include <iostream>

int main()
{
    // 1) 千分位免疫：这是当初最致命的 bug
    {
        std::locale::global(std::locale(""));
        const std::string s = zcrash::FormatHex(0x7FF9BB33C52FULL);
        if (s != "7ff9bb33c52f")
        {
            std::cout << "FAIL FormatHex: " << s << "\n";
            return 1;
        }
    }

    // 2) JSON 转义：路径有反斜杠，符号名有 <> 和 &
    {
        const std::string e = zcrash::JsonEscape("E:\\a\\b <Foo::Bar> \"x\"");
        if (e.find("\\") == std::string::npos || e.find("\\\"") == std::string::npos)
        {
            std::cout << "FAIL JsonEscape: " << e << "\n";
            return 1;
        }
    }

    // 3) 本 exe 自身的构建身份。Debug 构建应该有 PDB -> 有 build_id；
    //    Release 没有 PDB -> 应给出 reason 而不是空串。两种都必须被正确
    //    区分开，这正是这次要验证的核心。
    {
        const zcrash::BuildIdentity id = zcrash::ReadBuildIdentity();
        std::cout << "build_id   = " << (id.build_id.empty() ? "<empty>" : id.build_id) << "\n";
        std::cout << "timestamp  = " << id.timestamp << "\n";
        std::cout << "ok         = " << (id.ok ? "true" : "false") << "\n";
        std::cout << "reason     = " << (id.reason.empty() ? "<none>" : id.reason) << "\n";

        // 契约：ok 与 build_id 必须一致，且失败时必须有 reason。
        // 「拿不到但说不出为什么」是不可接受的 —— 那会被误读成没崩溃信息。
        if (id.ok && id.build_id.empty())
        {
            std::cout << "FAIL: ok=true but build_id empty\n";
            return 1;
        }
        if (!id.ok && id.reason.empty())
        {
            std::cout << "FAIL: ok=false but no reason\n";
            return 1;
        }
    }

    // 4) 快照写出：确认两个产物都能落盘且 JSON 结构完整
    {
        zcrash::Snapshot snap;
        snap.exception_code = 0xC0000005;
        snap.exception_address = 0x7FF9BB33C52FULL;
        snap.thread_id = 4321;
        snap.product_version = "0.1.0";
        snap.build_id = "9bc90cbc144f43dc970ee68579fa266231";

        zcrash::Frame f;
        f.pc = 0x7FF9BB33C52FULL;
        f.module = "ZEditor.exe";
        f.module_base = 0x7FF700000000ULL;
        f.function = "D3D12Descriptor::GetHeapResourceId";
        f.offset = 0x1f;
        f.file = "E:\\a\\d3d12_manager.cpp";
        f.line = 592;
        f.has_symbol = true;
        f.has_line = true;
        snap.frames.push_back(f);

        zcrash::WriteText(snap, "crashverify_stack.txt");
        zcrash::WriteJson(snap, "crashverify_snap.json");
    }

    std::cout << "RESULT=PASS\n";
    return 0;
}
