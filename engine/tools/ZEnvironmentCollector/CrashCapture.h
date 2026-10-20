#pragma once
#include "Collection.h"
#include "EvidenceFiles.h"

namespace zengine::envcheck::collector
{
struct CaptureOptions
{
    Options environment;
    std::filesystem::path procDump;
    std::filesystem::path gameExecutable;
    bool memoryConsent = false;
    bool licenseAccepted = false;
    unsigned waitSeconds = 3600;
};
struct CaptureResult
{
    Result report;
    bool dumpCaptured = false;
    std::string message;
    bool procDumpVerified = false;
};
struct ProcDumpVerification
{
    bool trustAttempted = false;
    LONG trustStatus = ERROR_SUCCESS;
    DWORD signerNameError = ERROR_SUCCESS;
    bool microsoftSigner = false;
    bool verified = false;
    std::string signer;
    std::string issuer;
    std::string originalName;
    std::string fileVersion;
};
std::string DescribeProcDumpTrustFailure(const ProcDumpVerification& verification);
std::string ValidateProcDump(const std::filesystem::path& file, ProcDumpVerification* verification = nullptr);
CaptureResult CaptureCrash(const CaptureOptions& options, const std::atomic_bool& stop,
                           const std::function<void(const std::string&, bool)>& progress);
std::filesystem::path LoadProcDumpPreference();
void SaveProcDumpPreference(const std::filesystem::path& file);
}
