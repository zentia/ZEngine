#include "CrashCapture.h"
#include <Windows.h>
#include <DbgHelp.h>
#include <wincrypt.h>
#include <softpub.h>
#include <fstream>
#include <iterator>
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
        ProcDumpVerification verification;
        try { ValidateProcDump(plain, &verification); } catch (...) { rejected = true; }
        Check(rejected && verification.trustAttempted && verification.trustStatus != ERROR_SUCCESS && !verification.verified, "reject unsigned executable with raw trust status");
        CaptureOptions options;
        rejected = false;
        try { CaptureCrash(options, stop, [](const std::string&, bool) {}); } catch (...) { rejected = true; }
        Check(rejected, "consent required before access");
        ProcDumpVerification simulated;
        simulated.trustAttempted = true;
        simulated.trustStatus = CRYPT_E_REVOCATION_OFFLINE;
        auto message = DescribeProcDumpTrustFailure(simulated);
        Check(message.find("0x80092013") != std::string::npos && message.find("吊销查询离线") != std::string::npos, "offline failure classified with raw code");
        simulated.trustStatus = TRUST_E_BAD_DIGEST;
        message = DescribeProcDumpTrustFailure(simulated);
        Check(message.find("0x80096010") != std::string::npos && message.find("摘要与签名不符") != std::string::npos, "digest failure not mislabeled as offline");
        simulated.trustStatus = ERROR_SUCCESS; simulated.signer = "Unrelated publisher";
        Check(DescribeProcDumpTrustFailure(simulated).find("不是Microsoft") != std::string::npos, "wrong publisher is not accepted on trust success");
        stop = false;
        options.memoryConsent = true; options.licenseAccepted = true;
        options.procDump = plain;
        options.gameExecutable = ExecutableDirectory() / "EnvironmentCaptureTests.exe";
        options.environment.profile.id = "preflight-test";
        options.environment.profile.gameExecutable = "EnvironmentCaptureTests.exe";
        options.environment.outputDirectory = root / "preflight";
        auto failed = CaptureCrash(options, stop, [&](const std::string&, bool) {
            for (const auto& entry : std::filesystem::directory_iterator(options.environment.outputDirectory))
                if (entry.is_directory()) std::filesystem::create_directory(entry.path() / "procdump.log");
        });
        Check(!failed.dumpCaptured && !failed.procDumpVerified && failed.report.partial && !failed.report.archive.empty(), "preflight creates diagnostic package but not capture success");
        Check(!std::filesystem::exists(failed.report.directory / "crash.dmp"), "preflight does not capture process memory");
        std::ifstream capture(failed.report.directory / "capture.json", std::ios::binary);
        const std::string report((std::istreambuf_iterator<char>(capture)), std::istreambuf_iterator<char>());
        Check(report.find("\"status\":\"PROCDUMP_PREFLIGHT_FAILED\"") != std::string::npos && report.find("\"procDumpStarted\":false") != std::string::npos &&
            report.find("\"trustAttempted\":true") != std::string::npos && report.find("\"trustStatusHex\":\"0x") != std::string::npos, "preflight evidence preserves trust status and no-process state");
        Check(report.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != std::string::npos, "failed file SHA256 recorded");
        capture.close();
        if (argc == 3 && std::wstring(argv[1]) == L"--export-preflight-report")
        {
            std::filesystem::copy_file(failed.report.archive, argv[2]);
            std::cout << "PASS diagnostic-only ZIP exported\n";
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--verify-procdump")
        {
            ProcDumpVerification trusted;
            ValidateProcDump(argv[2], &trusted);
            Check(trusted.verified && trusted.microsoftSigner && trusted.trustStatus == ERROR_SUCCESS, "official ProcDump still passes unchanged trust policy");
            std::cout << "PASS official ProcDump signature-only check; no process started\n";
        }
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
