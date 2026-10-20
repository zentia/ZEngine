#include <Windows.h>
#include <string>
#include <vector>
int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    const bool systemCrash = std::wstring(argv[2]) == L"wer-crash";
    SetErrorMode(SEM_FAILCRITICALERRORS | (systemCrash ? 0 : SEM_NOGPFAULTERRORBOX));
    HANDLE trigger = OpenEventW(SYNCHRONIZE, FALSE, argv[1]);
    if (!trigger) return 3;
    std::vector<unsigned char> memory(32 * 1024 * 1024, 0x5a);
    const auto wait = WaitForSingleObject(trigger, 45000);
    CloseHandle(trigger);
    if (wait != WAIT_OBJECT_0) return 4;
    if (systemCrash || std::wstring(argv[2]) == L"crash") RaiseException(0xe0424242, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    return memory[0] == 0x5a ? 0 : 5;
}
