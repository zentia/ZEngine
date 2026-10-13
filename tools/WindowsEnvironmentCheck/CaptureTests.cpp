#include "CrashCapture.h"
#include <Windows.h>
#include <DbgHelp.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>
using namespace zengine::envcheck::collector;
void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
int wmain(int argc, wchar_t** argv)
{
    auto root = std::filesystem::temp_directory_path() / ("capture-unit-" + std::to_string(GetCurrentProcessId()));
    try
    {
        Check(std::filesystem::create_directory(root), "unique test directory");
        const auto plain = root / "plain.txt";
        std::ofstream(plain, std::ios::binary) << "abc";
        Check(Sha256File(plain) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "stream SHA256");
        std::atomic_bool stop{true};
        bool rejected = false;
        try { Sha256File(plain, &stop); } catch (...) { rejected = true; }
        Check(rejected, "hash cancellation");
        rejected = false;
        try { ValidateFullDump(plain, 1); } catch (...) { rejected = true; }
        Check(rejected, "reject non-dump");
        rejected = false;
        try { ValidateProcDump(plain); } catch (...) { rejected = true; }
        Check(rejected, "reject unsigned executable");
        CaptureOptions options;
        rejected = false;
        try { CaptureCrash(options, stop, [](const std::string&, bool) {}); } catch (...) { rejected = true; }
        Check(rejected, "consent required before access");
        if (argc == 3 && std::wstring(argv[1]) == L"--large-zip")
        {
            stop = false;
            auto large = root / "large.bin";
            { std::ofstream out(large, std::ios::binary); out.seekp((1ull << 32) + 1023); out.put('Z'); }
            WriteZip64(root, argv[2], {"plain.txt", "large.bin"}, stop);
        }
        stop = false;
        WriteZip64(root, root / "test.zip", {"plain.txt"}, stop);
        Check(std::filesystem::file_size(root / "test.zip") > 40, "ZIP64 generated");
        stop = true;
        rejected = false;
        try { WriteZip64(root, root / "cancel.zip", {"plain.txt"}, stop); } catch (...) { rejected = true; }
        Check(rejected && !std::filesystem::exists(root / "cancel.zip.partial"), "archive cancel cleanup");
        Check(std::filesystem::exists(plain), "source retained");
        std::filesystem::remove_all(root);
        std::cout << "PASS capture consent, signature rejection, SHA256, dump rejection, ZIP64, cancellation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        std::error_code ignored; std::filesystem::remove_all(root, ignored); return 1;
    }
}
