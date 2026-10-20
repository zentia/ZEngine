#include "Collection.h"
#include "DiagnosisUtil.h"
#include <Windows.h>
#include <winevt.h>
#include <array>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

namespace zengine::envcheck::collector
{
using namespace diagnosis;
namespace
{
std::string RegistryText(HKEY root, const wchar_t* path, const wchar_t* name)
{
    wchar_t data[1024]{};
    DWORD size = sizeof(data);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS)
        return {};
    return WideToUtf8(data);
}
std::string HexBytes(const BYTE* data, size_t size)
{
    std::ostringstream out;
    for (size_t i = 0; i < size; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]);
    return out.str();
}
struct EventHandle
{
    EVT_HANDLE value = nullptr;
    ~EventHandle() { if (value) EvtClose(value); }
};
std::string EventValue(const EVT_VARIANT& v)
{
    if (v.Type & EVT_VARIANT_TYPE_ARRAY) return {};
    switch (v.Type)
    {
    case EvtVarTypeString: return v.StringVal ? WideToUtf8(v.StringVal) : "";
    case EvtVarTypeUInt16: return std::to_string(v.UInt16Val);
    case EvtVarTypeUInt32: case EvtVarTypeHexInt32: return std::to_string(v.UInt32Val);
    case EvtVarTypeUInt64: case EvtVarTypeHexInt64: return std::to_string(v.UInt64Val);
    case EvtVarTypeInt32: return std::to_string(v.Int32Val);
    case EvtVarTypeFileTime: return FormatUtcIso8601(v.FileTimeVal);
    default: return {};
    }
}
}
std::string CollectFirmwareJson(bool& complete)
{
    complete = true;
    const wchar_t* bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    std::ostringstream out;
    out << "{\"schemaVersion\":1,\"source\":\"WindowsRegistryAndSMBIOS\",\"fields\":{";
    const std::array<const wchar_t*, 7> names = {L"BIOSVendor", L"BIOSVersion", L"BIOSReleaseDate",
        L"SystemManufacturer", L"SystemProductName", L"BaseBoardManufacturer", L"BaseBoardProduct"};
    bool first = true;
    for (auto name : names)
    {
        if (!first) out << ',';
        first = false;
        const auto value = RegistryText(HKEY_LOCAL_MACHINE, bios, name);
        if (value.empty()) complete = false;
        out << JsonString(WideToUtf8(name)) << ':' << (value.empty() ? "null" : JsonString(value));
    }
    BYTE revision[64]{};
    DWORD length = sizeof(revision);
    auto result = RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                              L"Update Revision", RRF_RT_REG_BINARY, nullptr, revision, &length);
    out << "},\"microcode\":{\"source\":\"CentralProcessor/0/Update Revision\",\"rawRegistryBytes\":"
        << (result == ERROR_SUCCESS ? JsonString(HexBytes(revision, length)) : "null")
        << ",\"win32Error\":" << result << ",\"decodedRevision\":null,\"interpretation\":\"raw bytes only; no CPU health or minimum revision inference\"}";
    const DWORD signature = ('R' << 24) | ('S' << 16) | ('M' << 8) | 'B';
    const auto needed = GetSystemFirmwareTable(signature, 0, nullptr, 0);
    std::vector<BYTE> data;
    bool valid = false;
    if (needed >= 8 && needed <= 16 * 1024 * 1024)
    {
        data.resize(needed);
        valid = GetSystemFirmwareTable(signature, 0, data.data(), needed) == needed;
    }
    std::ostringstream modules;
    bool firstModule = true;
    if (valid)
    {
        DWORD tableLength = 0;
        memcpy(&tableLength, data.data() + 4, 4);
        valid = tableLength <= data.size() - 8;
        size_t offset = 8, end = valid ? 8 + tableLength : 8;
        while (valid && offset < end)
        {
            if (offset + 4 > end) { valid = false; break; }
            const BYTE type = data[offset], size = data[offset + 1];
            if (size < 4 || offset + size > end) { valid = false; break; }
            size_t next = offset + size;
            while (next + 1 < end && (data[next] != 0 || data[next + 1] != 0)) ++next;
            if (next + 1 >= end) { valid = false; break; }
            auto u16 = [&](size_t index) -> unsigned {
                return index + 2 <= size ? data[offset + index] | (data[offset + index + 1] << 8) : 0;
            };
            auto u32 = [&](size_t index) -> DWORD {
                DWORD value = 0; if (index + 4 <= size) memcpy(&value, data.data() + offset + index, 4); return value;
            };
            if (type == 17 && size >= 0x15)
            {
                if (!firstModule) modules << ',';
                firstModule = false;
                auto rawSize = u16(0x0c);
                std::uint64_t capacity = 0;
                if (rawSize == 0x7fff) capacity = static_cast<std::uint64_t>(u32(0x1c) & 0x7fffffff) * 1024 * 1024;
                else if (rawSize != 0xffff) capacity = static_cast<std::uint64_t>(rawSize & 0x7fff) * ((rawSize & 0x8000) ? 1024 : 1024 * 1024);
                auto speed = u16(0x15), configured = u16(0x20);
                if (speed == 0xffff) speed = u32(0x54);
                if (configured == 0xffff) configured = u32(0x58);
                modules << "{\"installed\":" << (rawSize ? "true" : "false")
                    << ",\"capacityBytes\":" << (rawSize == 0xffff || (rawSize == 0x7fff && capacity == 0) ? "null" : std::to_string(capacity))
                    << ",\"memoryType\":" << static_cast<unsigned>(data[offset + 0x12])
                    << ",\"speedMTs\":" << (speed ? std::to_string(speed) : "null")
                    << ",\"configuredSpeedMTs\":" << (configured ? std::to_string(configured) : "null") << '}';
            }
            offset = next + 2;
            if (type == 127) break;
        }
    }
    complete = complete && valid && result == ERROR_SUCCESS;
    out << ",\"smbiosStatus\":" << JsonString(valid ? "READ" : "UNAVAILABLE_OR_MALFORMED")
        << ",\"memoryModules\":[" << modules.str() << "],\"xmpOrUndervolt\":\"not reliably observable; ask user\"}";
    return out.str();
}
std::string CollectHealthEventsJson(unsigned lookbackHours, bool& complete)
{
    complete = false;
    std::array<const wchar_t*, 15> paths = {
        L"Event/System/Provider/@Name", L"Event/System/EventID", L"Event/System/Level", L"Event/System/TimeCreated/@SystemTime",
        L"Event/EventData/Data[@Name='ErrorSource']", L"Event/EventData/Data[@Name='ErrorType']",
        L"Event/EventData/Data[@Name='ApicId']", L"Event/EventData/Data[@Name='MCABank']", L"Event/EventData/Data[@Name='MciStat']",
        L"Event/EventData/Data[@Name='CompletionType']", L"Event/EventData/Data[@Name='MemorySize']",
        L"Event/EventData/Data[@Name='TestDuration']", L"Event/EventData/Data[@Name='TestType']",
        L"Event/EventData/Data[@Name='NumBadPages']", L"Event/EventData/Data[@Name='NumPagesTested']"};
    const std::array<const char*, 15> keys = {"provider", "eventId", "level", "timeUtc", "errorSource", "errorType",
        "apicId", "mcaBank", "mciStat", "completionType", "memorySize", "testDuration", "testType", "numBadPages", "numPagesTested"};
    EventHandle context{EvtCreateRenderContext(static_cast<DWORD>(paths.size()), paths.data(), EvtRenderContextValues)};
    if (!context.value) return "{\"status\":\"CONTEXT_FAILED\",\"win32Error\":" + std::to_string(GetLastError()) + "}";
    const auto query = L"*[System[(Provider[@Name='Microsoft-Windows-WHEA-Logger'] or Provider[@Name='Microsoft-Windows-MemoryDiagnostics-Results']) and TimeCreated[timediff(@SystemTime) <= "
        + std::to_wstring(static_cast<std::uint64_t>(lookbackHours) * 3600000) + L"]]]";
    EventHandle events{EvtQuery(nullptr, L"System", query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection)};
    if (!events.value) return "{\"status\":\"QUERY_FAILED\",\"win32Error\":" + std::to_string(GetLastError()) + "}";
    std::ostringstream out;
    out << "{\"schemaVersion\":1,\"lookbackHours\":" << lookbackHours << ",\"events\":[";
    unsigned count = 0, failures = 0;
    DWORD queryError = 0;
    const auto deadline = GetTickCount64() + 15000;
    while (count < 512 && GetTickCount64() < deadline)
    {
        EVT_HANDLE handle = nullptr;
        DWORD returned = 0;
        if (!EvtNext(events.value, 1, &handle, 200, 0, &returned))
        {
            queryError = GetLastError();
            if (queryError == ERROR_NO_MORE_ITEMS) queryError = 0;
            break;
        }
        EventHandle item{handle};
        DWORD size = 0, properties = 0;
        EvtRender(context.value, handle, EvtRenderEventValues, 0, nullptr, &size, &properties);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 1024 * 1024) { ++failures; ++count; continue; }
        std::vector<std::uint64_t> buffer((size + 7) / 8);
        if (!EvtRender(context.value, handle, EvtRenderEventValues, size, buffer.data(), &size, &properties)) { ++failures; ++count; continue; }
        const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer.data());
        if (count != failures) out << ',';
        out << '{';
        for (DWORD i = 0; i < properties && i < keys.size(); ++i)
        {
            if (i) out << ',';
            auto value = EventValue(values[i]);
            if (value.size() > 256) value.resize(256);
            out << JsonString(keys[i]) << ':' << (value.empty() ? "null" : JsonString(value));
        }
        out << '}'; ++count;
    }
    const bool incomplete = queryError || failures || count == 512 || GetTickCount64() >= deadline;
    complete = !incomplete;
    out << "],\"status\":" << JsonString(incomplete ? "INCOMPLETE" : "READ") << ",\"renderFailures\":" << failures
        << ",\"win32Error\":" << queryError << ",\"note\":\"No events is not a hardware health certificate. Missing test results do not mean a passed memory test.\"}";
    return out.str();
}
}
