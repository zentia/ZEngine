// =============================================================================
// ZEngineToolApi -- see ZEngineToolApi.h
// =============================================================================

#include "Runtime/ZEngineApi/ZEngineToolApi.h"

#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Log/LogSystem.h"
#include "Runtime/Core/Memory/MemoryManager.h"
#include "Runtime/RegisterRuntime.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/Asset/Data/LocalizationTable.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace
{
    std::mutex g_ToolApiMutex;
    bool g_Initialized = false;

    // The one loaded language table. Owned by the engine's ObjectManager (that is
    // what AssetManager::loadAsset hands back), so we never delete it -- it goes
    // away with SHUTDOWN_SYSTEM().
    LocalizationDataTable* g_Table = nullptr;

    // -------------------------------------------------------------------------
    // CSV: RFC 4180 subset -- comma separator, double-quote escaping (a quoted
    // field may contain commas, CR/LF and a doubled "" for a literal quote),
    // first row is the header. Enough for what a translator produces out of
    // Excel, and deliberately NOT a general CSV library: V2 (shared with the
    // editor's DataTableImporter::ParseCsv) can widen it if a table ever needs
    // more.
    // -------------------------------------------------------------------------

    eastl::vector<eastl::string> ParseCsvLine(const eastl::string& line)
    {
        eastl::vector<eastl::string> cells;
        eastl::string cell;
        bool in_quotes = false;

        for (size_t i = 0; i < line.size(); ++i)
        {
            const char c = line[i];

            if (in_quotes)
            {
                if (c == '"')
                {
                    if (i + 1 < line.size() && line[i + 1] == '"')
                    {
                        cell += '"';  // escaped quote
                        ++i;
                    }
                    else
                    {
                        in_quotes = false;
                    }
                }
                else
                {
                    cell += c;
                }
                continue;
            }

            if (c == '"')
            {
                in_quotes = true;
            }
            else if (c == ',')
            {
                cells.push_back(cell);
                cell.clear();
            }
            else
            {
                cell += c;
            }
        }
        cells.push_back(cell);
        return cells;
    }

    /// Split a whole file into logical CSV rows. A row may span physical lines
    /// when a quoted field contains a newline, so this walks characters rather
    /// than calling getline().
    eastl::vector<eastl::string> SplitCsvRows(const eastl::string& text)
    {
        eastl::vector<eastl::string> rows;
        eastl::string row;
        bool in_quotes = false;

        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];

            if (c == '"')
            {
                // Doubled quote inside a quoted field: consume both, keep one.
                if (in_quotes && i + 1 < text.size() && text[i + 1] == '"')
                {
                    row += '"';
                    row += '"';
                    ++i;
                    continue;
                }
                in_quotes = !in_quotes;
                row += c;
                continue;
            }

            if (!in_quotes && (c == '\n' || c == '\r'))
            {
                // Swallow the CR of a CRLF pair so Windows-authored files don't
                // produce a blank row per line.
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                {
                    ++i;
                }
                rows.push_back(row);
                row.clear();
                continue;
            }

            row += c;
        }

        if (!row.empty())
        {
            rows.push_back(row);
        }
        return rows;
    }

    bool ParseLocalizationCsv(const std::filesystem::path& csv_path,
                              eastl::vector<LocalizationRow>& out_rows,
                              eastl::string& out_error)
    {
        std::ifstream input(csv_path, std::ios::binary);
        if (!input)
        {
            out_error = "cannot open CSV";
            return false;
        }

        std::ostringstream buffer;
        buffer << input.rdbuf();
        eastl::string text(buffer.str().c_str());

        // Excel on Windows writes a UTF-8 BOM; the rest of the engine's CSV
        // readers strip it too.
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text = text.substr(3);
        }

        const eastl::vector<eastl::string> physical_rows = SplitCsvRows(text);
        if (physical_rows.empty())
        {
            out_error = "CSV is empty";
            return false;
        }

        const eastl::vector<eastl::string> header = ParseCsvLine(physical_rows[0]);
        int id_column = -1;
        int text_column = -1;
        for (size_t i = 0; i < header.size(); ++i)
        {
            if (header[i] == "id")
            {
                id_column = static_cast<int>(i);
            }
            else if (header[i] == "text")
            {
                text_column = static_cast<int>(i);
            }
        }

        if (id_column < 0 || text_column < 0)
        {
            out_error = "CSV header must contain both 'id' and 'text' columns";
            return false;
        }

        for (size_t r = 1; r < physical_rows.size(); ++r)
        {
            if (physical_rows[r].empty())
            {
                continue;  // trailing newline / blank separator line
            }

            const eastl::vector<eastl::string> cells = ParseCsvLine(physical_rows[r]);
            if (static_cast<int>(cells.size()) <= id_column || static_cast<int>(cells.size()) <= text_column)
            {
                continue;  // short row: skip rather than emit an empty key
            }

            const eastl::string& id = cells[id_column];
            if (id.empty())
            {
                continue;
            }

            LocalizationRow row;
            row.id = id;
            row.text = cells[text_column];
            out_rows.push_back(row);
        }

        if (out_rows.empty())
        {
            out_error = "CSV produced no rows";
            return false;
        }
        return true;
    }
}  // namespace

int ZRuntime_ToolInit()
{
    std::lock_guard<std::mutex> lock(g_ToolApiMutex);
    if (g_Initialized)
    {
        return 1;
    }

    // Exactly the slice ZEngineApi::InitMemoryProfilerData() uses, minus
    // ProfilerRuntime: reflection + memory + FileSystem + ObjectManager
    // (RegisterCore) and the runtime AssetManager (RegisterPlatform). No
    // Application, no WindowSystem, no RHI, no ScriptingManager, no ProjectInfo
    // -- a diagnostic tool must not create a window or a GPU device on the
    // machine it was sent to inspect.
    RegisterCore();
    RegisterPlatform();

    g_Initialized = START_SYSTEM_WITHOUT_UI();
    if (!g_Initialized)
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_ToolInit: engine subsystem startup failed");
        return 0;
    }

    LOG_INFO(ZEnvironmentCheck, "ZRuntime_ToolInit: minimal engine slice up (no RHI, no project)");
    return 1;
}

void ZRuntime_ToolShutdown()
{
    std::lock_guard<std::mutex> lock(g_ToolApiMutex);
    if (!g_Initialized)
    {
        return;
    }

    g_Table = nullptr;
    // Drain before teardown: bqlog is async and the last few hundred ms of
    // records would otherwise die with the worker thread.
    LogSystem::ForceFlush();
    SHUTDOWN_SYSTEM();
    g_Initialized = false;
}

void ZRuntime_LogInfo(const char* message)
{
    LOG_INFO(ZEnvironmentCheck, "{}", message != nullptr ? message : "");
}

void ZRuntime_LogWarning(const char* message)
{
    LOG_WARNING(ZEnvironmentCheck, "{}", message != nullptr ? message : "");
}

void ZRuntime_LogError(const char* message)
{
    LOG_ERROR(ZEnvironmentCheck, "{}", message != nullptr ? message : "");
}

void ZRuntime_LogFlush()
{
    // The static form, not the LOG_FLUSH() macro: ForceFlush() does not
    // dereference m_Logger, so a flush before the first record is harmless.
    LogSystem::ForceFlush();
}

const char* ZRuntime_LogFilePath()
{
    // Function-local static so the returned pointer outlives every caller and
    // never hands a C ABI consumer a pointer into a temporary. std::string on
    // purpose: LogFilePath() returns std::string, and this is C ABI glue, not a
    // serialized field (the eastl::string-only rule covers persisted data).
    static std::string cached;
    cached = LogSystem::LogFilePath();
    return cached.c_str();
}

int ZRuntime_LoadLocalizationTable(const char* zasset_path_utf8)
{
    if (zasset_path_utf8 == nullptr || zasset_path_utf8[0] == '\0')
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_LoadLocalizationTable: empty path");
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_ToolApiMutex);
    if (!g_Initialized)
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_LoadLocalizationTable: engine not initialised");
        return 0;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_LoadLocalizationTable: AssetManager unavailable");
        return 0;
    }

    // AssetManager::GetFullPath() passes absolute paths straight through, so a
    // tool with no project can still load by absolute path -- which is the only
    // kind it has (it ships next to its own .zasset).
    std::filesystem::path path(zasset_path_utf8);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_LoadLocalizationTable: '{}' does not exist", zasset_path_utf8);
        return 0;
    }

    LocalizationDataTable* table = asset_manager->loadAsset<LocalizationDataTable>(eastl::string(path.string().c_str()));
    if (table == nullptr)
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_LoadLocalizationTable: failed to read '{}'", zasset_path_utf8);
        return 0;
    }

    g_Table = table;
    LOG_INFO(ZEnvironmentCheck,
             "Loaded localization table '{}' ({} rows)",
             zasset_path_utf8,
             static_cast<unsigned>(table->rowCount()));
    return 1;
}

const char* ZRuntime_Localize(const char* id)
{
    if (id == nullptr || id[0] == '\0' || g_Table == nullptr)
    {
        return nullptr;
    }

    const LocalizationRow* row = g_Table->find(eastl::string(id));
    // Pointer into the table's own row storage: valid for as long as the table
    // is loaded. The caller copies before it returns.
    return row != nullptr ? row->text.c_str() : nullptr;
}

unsigned ZRuntime_LocalizationRowCount()
{
    return g_Table != nullptr ? static_cast<unsigned>(g_Table->rowCount()) : 0u;
}

int ZRuntime_LocalizationRowAt(unsigned index, const char** out_id, const char** out_text)
{
    if (out_id == nullptr || out_text == nullptr || g_Table == nullptr)
    {
        return 0;
    }
    if (index >= g_Table->rowCount())
    {
        return 0;
    }

    // m_Rows is a std::vector<LocalizationRow>, so index arithmetic lands on the
    // row storage directly -- the same memory find() hands back.
    const LocalizationRow& row = g_Table->m_Rows[index];
    *out_id = row.id.c_str();
    *out_text = row.text.c_str();
    return 1;
}

void ZRuntime_UnloadLocalizationTable()
{
    std::lock_guard<std::mutex> lock(g_ToolApiMutex);
    g_Table = nullptr;
}

int ZRuntime_CompileLocalizationTable(const char* csv_path_utf8, const char* zasset_path_utf8)
{
    if (csv_path_utf8 == nullptr || csv_path_utf8[0] == '\0' ||
        zasset_path_utf8 == nullptr || zasset_path_utf8[0] == '\0')
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_ToolApiMutex);
    if (!g_Initialized)
    {
        return 0;
    }

    eastl::vector<LocalizationRow> rows;
    eastl::string error;
    if (!ParseLocalizationCsv(std::filesystem::path(csv_path_utf8), rows, error))
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_CompileLocalizationTable: '{}': {}", csv_path_utf8, error.c_str());
        return 0;
    }

    AssetManager* asset_manager = GET_SYSTEM(AssetManager);
    if (asset_manager == nullptr)
    {
        LOG_ERROR(ZEnvironmentCheck, "ZRuntime_CompileLocalizationTable: AssetManager unavailable");
        return 0;
    }

    LocalizationDataTable* table = MemoryManager::CreateObject<LocalizationDataTable>();
    table->m_Rows.reserve(rows.size());
    for (LocalizationRow& row : rows)
    {
        table->m_Rows.push_back(row);
    }
    // find() hashes against m_KeyIndex; the cook step has to build it the same
    // way a load does (DataTable::onPostLoad never runs for a freshly Produced
    // instance).
    table->buildKeyIndex();

    const std::filesystem::path output(zasset_path_utf8);
    if (output.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(output.parent_path(), ec);
    }

    const bool written = asset_manager->WriteObjectToDiskThreadSafe(output, *table);
    LogSystem::ForceFlush();
    return written ? 1 : 0;
}
