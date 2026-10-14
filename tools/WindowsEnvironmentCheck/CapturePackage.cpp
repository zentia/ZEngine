#include "CapturePackage.h"
#include "DiagnosisUtil.h"
#include "ReportWriter.h"
#include <sstream>
#include <stdexcept>

namespace zengine::envcheck::collector
{
void CollectDumpEnvironment(const Options& options, const DumpMetadata& metadata, Result& result,
    const std::atomic_bool& stop, const std::function<void(const std::string&, bool)>& progress)
{
    Options environment = options; environment.outputDirectory = result.directory;
    environment.incidentKind = diagnosis::IncidentKind::Crash;
    environment.incidentTime = (static_cast<diagnosis::UtcTicks>(metadata.timestamp) + 11644473600ull) * 10000000ull;
    auto collected = Collect(environment, stop, progress);
    if (stop.load()) throw std::runtime_error("后处理已取消，完整dump保留本地");
    std::filesystem::rename(collected.archive, result.directory / "environment.zip");
    result.partial = result.partial || collected.partial;
}
void PackageDumpEvidence(Result& result, std::vector<std::string> files, const std::atomic_bool& stop,
    const std::function<void(const std::string&, bool)>& progress)
{
    progress("计算证据SHA256（大dump可能耗时）", true);
    std::ostringstream hashes; hashes << "{\"schemaVersion\":1,\"files\":[";
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (i) hashes << ',';
        hashes << "{\"name\":" << JsonString(files[i]) << ",\"bytes\":" << std::filesystem::file_size(result.directory / files[i])
            << ",\"sha256\":" << JsonString(Sha256File(result.directory / files[i], &stop)) << '}';
    }
    hashes << "]}"; WriteReportFile(result.directory / "checksums.json", hashes.str()); files.push_back("checksums.json");
    ULARGE_INTEGER available{};
    const auto bytes = std::filesystem::file_size(result.directory / "crash.dmp");
    if (!GetDiskFreeSpaceExW(result.directory.c_str(), &available, nullptr, nullptr) || available.QuadPart < bytes + (1ull << 30))
        throw std::runtime_error("压缩空间不足，原始dump及环境报告已保留");
    progress("流式压缩并回读校验ZIP64，不自动上传", true);
    const auto archive = result.directory / "capture.zip";
    WriteZip64(result.directory, archive, files, stop);
    result.archive = archive;
}
}
