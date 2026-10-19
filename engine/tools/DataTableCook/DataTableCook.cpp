// =============================================================================
// DataTableCook -- build-time CSV -> .zasset compiler for LocalizationTable.
// =============================================================================
// Usage:
//     DataTableCook <input.csv> <output.zasset>
//
// Why this exists as its own executable rather than a button inside the editor:
// the strings it bakes ship inside tools that are zipped to machines with no
// project, no editor and (by design) no CSV parser -- ZEnvironmentCollector is
// the motivating case. The editor's own CSV pipeline is driven by
// CsvSchemaRegistry out of <Project>/Data/, which needs a project open; this
// cook has to run in a bare build tree.
//
// It therefore goes through the same tool-facing C ABI the shipped tools use
// (Runtime/ZEngineApi/ZEngineToolApi.h). That has a second benefit: the cook
// step and the load step exercise the same code path, so a .zasset that cooks
// successfully is guaranteed to be readable by the tool that ships it.
//
// Exit codes: 0 = cooked, 1 = cook failed, 2 = bad command line.
// =============================================================================

#include "Runtime/ZEngineApi/ZEngineToolApi.h"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: DataTableCook <input.csv> <output.zasset>\n");
        return 2;
    }

    const char* csv_path = argv[1];
    const char* zasset_path = argv[2];

    if (ZRuntime_ToolInit() == 0)
    {
        std::fprintf(stderr, "DataTableCook: engine subsystem startup failed (see the engine log)\n");
        return 1;
    }

    const int cooked = ZRuntime_CompileLocalizationTable(csv_path, zasset_path);
    ZRuntime_ToolShutdown();

    if (cooked == 0)
    {
        std::fprintf(stderr, "DataTableCook: failed to cook '%s' -> '%s'\n", csv_path, zasset_path);
        return 1;
    }

    std::printf("DataTableCook: '%s' -> '%s'\n", csv_path, zasset_path);
    return 0;
}
