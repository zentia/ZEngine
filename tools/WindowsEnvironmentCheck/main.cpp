#include "EnvironmentProbe.h"
#include "ReportWriter.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
using zengine::envcheck::Requirements;

struct CommandLineOptions
{
    std::filesystem::path outputDirectory = std::filesystem::current_path();
    Requirements requirements;
    bool quiet = false;
    bool showHelp = false;
};

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string NarrowAscii(const std::wstring& value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value)
    {
        if (character < 0 || character > 0x7f)
        {
            throw std::invalid_argument("CPU feature names must use ASCII characters.");
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

void SetRequiredFeatures(Requirements& requirements, const std::string& value)
{
    requirements.avx = false;
    requirements.avx2 = false;
    requirements.fma = false;
    requirements.f16c = false;

    std::stringstream stream(value);
    std::string feature;
    while (std::getline(stream, feature, ','))
    {
        feature = ToLower(feature);
        if (feature.empty() || feature == "none")
        {
            continue;
        }
        if (feature == "avx")
        {
            requirements.avx = true;
        }
        else if (feature == "avx2")
        {
            requirements.avx2 = true;
        }
        else if (feature == "fma")
        {
            requirements.fma = true;
        }
        else if (feature == "f16c")
        {
            requirements.f16c = true;
        }
        else
        {
            throw std::invalid_argument("Unknown CPU feature in --require: " + feature);
        }
    }
}

CommandLineOptions ParseCommandLine(int argumentCount, wchar_t** arguments)
{
    CommandLineOptions options;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring argument = arguments[index];
        if (argument == L"--output-dir")
        {
            if (++index >= argumentCount)
            {
                throw std::invalid_argument("--output-dir requires a path.");
            }
            options.outputDirectory = arguments[index];
        }
        else if (argument == L"--require")
        {
            if (++index >= argumentCount)
            {
                throw std::invalid_argument("--require requires a comma-separated feature list.");
            }
            const std::wstring wideValue = arguments[index];
            SetRequiredFeatures(options.requirements, NarrowAscii(wideValue));
        }
        else if (argument == L"--min-memory-gb")
        {
            if (++index >= argumentCount)
            {
                throw std::invalid_argument("--min-memory-gb requires a positive integer.");
            }
            const unsigned long long gibibytes = std::stoull(arguments[index]);
            options.requirements.minimumMemoryBytes = gibibytes * 1024ull * 1024ull * 1024ull;
        }
        else if (argument == L"--quiet")
        {
            options.quiet = true;
        }
        else if (argument == L"--help" || argument == L"-h" || argument == L"/?")
        {
            options.showHelp = true;
        }
        else
        {
            throw std::invalid_argument("Unknown command-line option.");
        }
    }
    return options;
}

void PrintUsage()
{
    std::cout
        << "ZWindowsEnvironmentCheck [options]\n\n"
        << "Options:\n"
        << "  --output-dir <path>        Report directory. Default: current directory.\n"
        << "  --require <features>       Required features: avx,avx2,fma,f16c or none.\n"
        << "                             Default: avx.\n"
        << "  --min-memory-gb <number>   Required physical memory. Default: 4.\n"
        << "  --quiet                    Do not print the full text report.\n"
        << "  --help                     Show this help.\n\n"
        << "Exit codes: 0=compatible, 10=CPU ISA, 11=Windows x64, 12=memory, 30=tool error.\n";
}
} // namespace

int wmain(int argumentCount, wchar_t** arguments)
{
    SetConsoleOutputCP(CP_UTF8);

    try
    {
        const CommandLineOptions options = ParseCommandLine(argumentCount, arguments);
        if (options.showHelp)
        {
            PrintUsage();
            return 0;
        }

        std::filesystem::create_directories(options.outputDirectory);

        const auto snapshot = zengine::envcheck::CollectEnvironment();
        const auto evaluation = zengine::envcheck::EvaluateEnvironment(snapshot, options.requirements);
        const std::string textReport =
            zengine::envcheck::BuildTextReport(snapshot, options.requirements, evaluation);
        const std::string jsonReport =
            zengine::envcheck::BuildJsonReport(snapshot, options.requirements, evaluation);

        const auto textPath = options.outputDirectory / "OSGameEnvironmentReport.txt";
        const auto jsonPath = options.outputDirectory / "OSGameEnvironmentReport.json";
        zengine::envcheck::WriteReportFile(textPath, textReport);
        zengine::envcheck::WriteReportFile(jsonPath, jsonReport);

        if (!options.quiet)
        {
            std::cout << textReport << '\n';
        }
        std::cout << "Reports written to:\n  " << textPath.string()
                  << "\n  " << jsonPath.string() << '\n';
        return evaluation.exitCode;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Environment check failed: " << exception.what() << '\n';
        return 30;
    }
}
