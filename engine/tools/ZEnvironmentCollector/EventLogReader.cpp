#include "EventLogReader.h"

#include "AttributionRules.h"
#include "DiagnosisUtil.h"

#include <Windows.h>
#include <winevt.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace zengine::envcheck::diagnosis
{
namespace
{
constexpr DWORD EventBatchSize = 64;
constexpr DWORD EventNextTimeoutMilliseconds = 1000;
constexpr std::size_t MaxSystemEventTimes = 200000;
constexpr std::size_t MaxRelevantEvents = 5000;
constexpr std::size_t InitialRenderBufferBytes = 4096;
constexpr std::size_t InitialXmlBufferCharacters = 4096;
constexpr std::size_t MaxXmlEntityLength = 10;
constexpr unsigned long MaxBmpCodePoint = 0xFFFF;

constexpr std::uint32_t ApplicationErrorEventId = 1000;
constexpr std::uint32_t ApplicationHangEventId = 1002;
constexpr std::uint32_t WindowsErrorReportingEventId = 1001;
const std::string ApplicationErrorProvider = "application error";
const std::string ApplicationHangProvider = "application hang";
const std::string WindowsErrorReportingProvider = "windows error reporting";

// Positional fallbacks for classic providers whose EventData carries no Name
// attributes. Field order to be confirmed against real machines (see README).
constexpr std::size_t AppErrorAppNameIndex = 0;
constexpr std::size_t AppErrorModuleNameIndex = 3;
constexpr std::size_t AppErrorExceptionCodeIndex = 6;
constexpr std::size_t AppErrorAppPathIndex = 10;
constexpr std::size_t AppErrorModulePathIndex = 11;
constexpr std::size_t AppHangAppNameIndex = 0;
constexpr std::size_t AppHangAppPathIndex = 5;
constexpr std::size_t WerEventNameIndex = 2;
constexpr std::size_t WerP1Index = 5;

class EventHandle
{
public:
    EventHandle() = default;

    explicit EventHandle(EVT_HANDLE handle)
        : handle_(handle)
    {
    }

    ~EventHandle()
    {
        Reset();
    }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    EVT_HANDLE Get() const
    {
        return handle_;
    }

    explicit operator bool() const
    {
        return handle_ != nullptr;
    }

    void Reset()
    {
        if (handle_ != nullptr)
        {
            EvtClose(handle_);
            handle_ = nullptr;
        }
    }

private:
    EVT_HANDLE handle_ = nullptr;
};

struct SystemValues
{
    std::string provider;
    std::uint32_t eventId = 0;
    std::uint32_t level = 0;
    UtcTicks time = 0;
};

struct EventDataItem
{
    std::string name;
    std::string value;
};

struct RenderBuffers
{
    std::vector<BYTE> values = std::vector<BYTE>(InitialRenderBufferBytes);
    std::vector<wchar_t> xml = std::vector<wchar_t>(InitialXmlBufferCharacters);
};

bool DeadlineReached(std::uint64_t deadlineTickCount)
{
    return deadlineTickCount != 0 && GetTickCount64() >= deadlineTickCount;
}

std::wstring BuildTimeRangeQuery(UtcTicks start, UtcTicks end)
{
    const std::wstring endText = Utf8ToWide(FormatUtcIso8601(end));
    if (start == 0)
    {
        return L"*[System[TimeCreated[@SystemTime<='" + endText + L"']]]";
    }
    const std::wstring startText = Utf8ToWide(FormatUtcIso8601(start));
    return L"*[System[TimeCreated[@SystemTime>='" + startText +
        L"' and @SystemTime<='" + endText + L"']]]";
}

DWORD VariantType(const EVT_VARIANT& variant)
{
    return variant.Type & EVT_VARIANT_TYPE_MASK;
}

std::string VariantString(const EVT_VARIANT& variant)
{
    if (VariantType(variant) == EvtVarTypeString && variant.StringVal != nullptr)
    {
        return WideToUtf8(variant.StringVal);
    }
    return {};
}

std::uint32_t VariantUnsigned(const EVT_VARIANT& variant)
{
    switch (VariantType(variant))
    {
    case EvtVarTypeByte: return variant.ByteVal;
    case EvtVarTypeUInt16: return variant.UInt16Val;
    case EvtVarTypeUInt32: return variant.UInt32Val;
    default: return 0;
    }
}

UtcTicks VariantFileTime(const EVT_VARIANT& variant)
{
    return VariantType(variant) == EvtVarTypeFileTime ? variant.FileTimeVal : 0;
}

bool RenderSystemValues(EVT_HANDLE context, EVT_HANDLE event, RenderBuffers& buffers, SystemValues& values)
{
    DWORD usedBytes = 0;
    DWORD propertyCount = 0;
    if (EvtRender(context, event, EvtRenderEventValues, static_cast<DWORD>(buffers.values.size()),
            buffers.values.data(), &usedBytes, &propertyCount) == FALSE)
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return false;
        }
        buffers.values.resize(usedBytes);
        if (EvtRender(context, event, EvtRenderEventValues, static_cast<DWORD>(buffers.values.size()),
                buffers.values.data(), &usedBytes, &propertyCount) == FALSE)
        {
            return false;
        }
    }
    if (propertyCount <= static_cast<DWORD>(EvtSystemTimeCreated))
    {
        return false;
    }
    const auto* properties = reinterpret_cast<const EVT_VARIANT*>(buffers.values.data());
    values.provider = VariantString(properties[EvtSystemProviderName]);
    values.eventId = VariantUnsigned(properties[EvtSystemEventID]);
    values.level = VariantUnsigned(properties[EvtSystemLevel]);
    values.time = VariantFileTime(properties[EvtSystemTimeCreated]);
    return values.time != 0;
}

bool RenderEventXml(EVT_HANDLE event, RenderBuffers& buffers, std::wstring& xml)
{
    DWORD usedBytes = 0;
    DWORD propertyCount = 0;
    auto bufferBytes = [&buffers]()
    {
        return static_cast<DWORD>(buffers.xml.size() * sizeof(wchar_t));
    };
    if (EvtRender(nullptr, event, EvtRenderEventXml, bufferBytes(), buffers.xml.data(),
            &usedBytes, &propertyCount) == FALSE)
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return false;
        }
        buffers.xml.resize(usedBytes / sizeof(wchar_t) + 1);
        if (EvtRender(nullptr, event, EvtRenderEventXml, bufferBytes(), buffers.xml.data(),
                &usedBytes, &propertyCount) == FALSE)
        {
            return false;
        }
    }
    const std::size_t characterCount = usedBytes / sizeof(wchar_t);
    xml.assign(buffers.xml.data(), characterCount);
    const std::size_t terminator = xml.find(L'\0');
    if (terminator != std::wstring::npos)
    {
        xml.resize(terminator);
    }
    return true;
}

std::wstring ReadAttribute(const std::wstring& attributes, const std::wstring& name)
{
    std::size_t position = 0;
    while ((position = attributes.find(name, position)) != std::wstring::npos)
    {
        const bool boundary = position == 0 || std::iswspace(attributes[position - 1]) != 0;
        std::size_t cursor = position + name.size();
        while (cursor < attributes.size() && std::iswspace(attributes[cursor]) != 0)
        {
            ++cursor;
        }
        if (boundary && cursor < attributes.size() && attributes[cursor] == L'=')
        {
            ++cursor;
            while (cursor < attributes.size() && std::iswspace(attributes[cursor]) != 0)
            {
                ++cursor;
            }
            if (cursor < attributes.size() && (attributes[cursor] == L'\'' || attributes[cursor] == L'"'))
            {
                const wchar_t quote = attributes[cursor];
                const std::size_t end = attributes.find(quote, cursor + 1);
                if (end != std::wstring::npos)
                {
                    return attributes.substr(cursor + 1, end - cursor - 1);
                }
            }
            return {};
        }
        position += name.size();
    }
    return {};
}

std::wstring DecodeXmlEntities(const std::wstring& text)
{
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] != L'&')
        {
            result.push_back(text[index]);
            continue;
        }
        const std::size_t end = text.find(L';', index);
        if (end == std::wstring::npos || end - index > MaxXmlEntityLength)
        {
            result.push_back(L'&');
            continue;
        }
        const std::wstring entity = text.substr(index + 1, end - index - 1);
        if (entity == L"amp")
        {
            result.push_back(L'&');
        }
        else if (entity == L"lt")
        {
            result.push_back(L'<');
        }
        else if (entity == L"gt")
        {
            result.push_back(L'>');
        }
        else if (entity == L"quot")
        {
            result.push_back(L'"');
        }
        else if (entity == L"apos")
        {
            result.push_back(L'\'');
        }
        else if (entity.size() > 1 && entity[0] == L'#')
        {
            const bool hex = entity[1] == L'x' || entity[1] == L'X';
            const unsigned long code = hex
                ? std::wcstoul(entity.c_str() + 2, nullptr, 16)
                : std::wcstoul(entity.c_str() + 1, nullptr, 10);
            if (code == 0 || code > MaxBmpCodePoint)
            {
                result.push_back(L'&');
                continue;
            }
            result.push_back(static_cast<wchar_t>(code));
        }
        else
        {
            result.push_back(L'&');
            continue;
        }
        index = end;
    }
    return result;
}

std::vector<EventDataItem> ParseEventData(const std::wstring& xml)
{
    std::vector<EventDataItem> items;
    const std::size_t sectionStart = xml.find(L"<EventData");
    if (sectionStart == std::wstring::npos)
    {
        return items;
    }
    const std::size_t sectionEnd = xml.find(L"</EventData>", sectionStart);
    const std::wstring tagName = L"<Data";
    std::size_t position = sectionStart + 1;
    while (true)
    {
        position = xml.find(tagName, position);
        if (position == std::wstring::npos || (sectionEnd != std::wstring::npos && position >= sectionEnd))
        {
            break;
        }
        const std::size_t afterName = position + tagName.size();
        if (afterName >= xml.size())
        {
            break;
        }
        const wchar_t next = xml[afterName];
        if (next != L' ' && next != L'>' && next != L'/')
        {
            position = afterName;
            continue;
        }
        const std::size_t tagClose = xml.find(L'>', afterName);
        if (tagClose == std::wstring::npos)
        {
            break;
        }
        EventDataItem item;
        item.name = WideToUtf8(ReadAttribute(xml.substr(afterName, tagClose - afterName), L"Name"));
        if (xml[tagClose - 1] == L'/')
        {
            items.push_back(std::move(item));
            position = tagClose + 1;
            continue;
        }
        const std::size_t valueEnd = xml.find(L"</Data>", tagClose + 1);
        if (valueEnd == std::wstring::npos)
        {
            break;
        }
        item.value = WideToUtf8(DecodeXmlEntities(xml.substr(tagClose + 1, valueEnd - tagClose - 1)));
        items.push_back(std::move(item));
        position = valueEnd + 1;
    }
    return items;
}

std::string LookupData(const std::vector<EventDataItem>& items, const std::string& name, std::size_t position)
{
    bool anyNamed = false;
    for (const auto& item : items)
    {
        if (item.name.empty())
        {
            continue;
        }
        anyNamed = true;
        if (EqualsIgnoreCaseAscii(item.name, name))
        {
            return item.value;
        }
    }
    if (!anyNamed && position < items.size())
    {
        return items[position].value;
    }
    return {};
}

void FillApplicationEventData(
    EVT_HANDLE event,
    const std::string& providerLower,
    RenderBuffers& buffers,
    NormalizedEvent& normalized)
{
    const bool isApplicationError =
        providerLower == ApplicationErrorProvider && normalized.eventId == ApplicationErrorEventId;
    const bool isApplicationHang =
        providerLower == ApplicationHangProvider && normalized.eventId == ApplicationHangEventId;
    const bool isWindowsErrorReporting =
        providerLower == WindowsErrorReportingProvider && normalized.eventId == WindowsErrorReportingEventId;
    if (!isApplicationError && !isApplicationHang && !isWindowsErrorReporting)
    {
        return;
    }

    std::wstring xml;
    if (!RenderEventXml(event, buffers, xml))
    {
        return;
    }
    const auto items = ParseEventData(xml);
    if (isApplicationError)
    {
        normalized.appName = LookupData(items, "AppName", AppErrorAppNameIndex);
        normalized.moduleName = LookupData(items, "ModuleName", AppErrorModuleNameIndex);
        normalized.exceptionCode = LookupData(items, "ExceptionCode", AppErrorExceptionCodeIndex);
        normalized.appPath = SanitizePath(LookupData(items, "AppPath", AppErrorAppPathIndex));
        normalized.modulePath = SanitizePath(LookupData(items, "ModulePath", AppErrorModulePathIndex));
    }
    else if (isApplicationHang)
    {
        normalized.appName = LookupData(items, "AppName", AppHangAppNameIndex);
        normalized.appPath = SanitizePath(LookupData(items, "AppPath", AppHangAppPathIndex));
    }
    else
    {
        normalized.werEventName = LookupData(items, "EventName", WerEventNameIndex);
        normalized.werP1 = LookupData(items, "P1", WerP1Index);
    }
}

void ProcessEvent(
    EventChannel channel,
    EVT_HANDLE event,
    EVT_HANDLE context,
    RenderBuffers& buffers,
    EventCollection& collection)
{
    SystemValues values;
    if (!RenderSystemValues(context, event, buffers, values))
    {
        return;
    }
    if (channel == EventChannel::System)
    {
        if (collection.systemEventTimes.size() < MaxSystemEventTimes)
        {
            collection.systemEventTimes.push_back(values.time);
        }
        else
        {
            collection.observability.eventCapReached = true;
        }
    }

    const std::string providerLower = ToLowerAscii(values.provider);
    if (!IsRelevantEvent(channel, providerLower, values.eventId, values.level))
    {
        return;
    }
    if (collection.events.size() >= MaxRelevantEvents)
    {
        collection.observability.eventCapReached = true;
        return;
    }

    NormalizedEvent normalized;
    normalized.channel = channel;
    normalized.provider = values.provider;
    normalized.eventId = values.eventId;
    normalized.level = values.level;
    normalized.time = values.time;
    if (channel == EventChannel::Application)
    {
        FillApplicationEventData(event, providerLower, buffers, normalized);
    }
    collection.events.push_back(std::move(normalized));
}

void ReadOldestSystemEventTime(EVT_HANDLE context, RenderBuffers& buffers, Observability& observability)
{
    EventHandle query(EvtQuery(nullptr, L"System", L"*", EvtQueryChannelPath | EvtQueryForwardDirection));
    if (!query)
    {
        return;
    }
    EVT_HANDLE handle = nullptr;
    DWORD returned = 0;
    if (EvtNext(query.Get(), 1, &handle, EventNextTimeoutMilliseconds, 0, &returned) == FALSE || returned == 0)
    {
        return;
    }
    EventHandle event(handle);
    SystemValues values;
    if (RenderSystemValues(context, event.Get(), buffers, values))
    {
        observability.systemOldestKnown = true;
        observability.systemOldest = values.time;
    }
}

bool ReadChannelRange(
    const wchar_t* channelPath,
    EventChannel channel,
    EVT_HANDLE context,
    const EventScanRange& range,
    const EventLogReadOptions& options,
    RenderBuffers& buffers,
    EventCollection& collection)
{
    if (IsEmptyEventScanRange(range))
    {
        return true;
    }
    const std::wstring queryText = BuildTimeRangeQuery(range.start, range.end);
    // Reverse direction: when MaxRelevantEvents is reached the events that are
    // dropped are the oldest ones, not the ones closest to the incident.
    EventHandle query(EvtQuery(
        nullptr, channelPath, queryText.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection));
    if (!query)
    {
        return false;
    }

    std::array<EVT_HANDLE, EventBatchSize> handles{};
    while (true)
    {
        if (DeadlineReached(options.deadlineTickCount))
        {
            collection.observability.budgetExceeded = true;
            break;
        }
        DWORD returned = 0;
        if (EvtNext(query.Get(), EventBatchSize, handles.data(), EventNextTimeoutMilliseconds, 0, &returned) == FALSE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (error == ERROR_TIMEOUT)
            {
                continue;
            }
            collection.observability.queryErrorOccurred = true;
            break;
        }
        for (DWORD index = 0; index < returned; ++index)
        {
            EventHandle event(handles[index]);
            ProcessEvent(channel, event.Get(), context, buffers, collection);
        }
    }
    return true;
}
} // namespace

bool IsEmptyEventScanRange(const EventScanRange& range)
{
    return range.end == 0 || (range.start != 0 && range.start > range.end);
}

std::vector<EventScanRange> BuildEventScanOrder(const EventLogReadOptions& options)
{
    std::vector<EventScanRange> ranges;
    const EventScanRange whole{options.rangeStart, options.rangeEnd};
    if (!options.hasPriorityWindow)
    {
        ranges.push_back(whole);
        return ranges;
    }

    const UtcTicks priorityStart = std::max(options.priorityStart, options.rangeStart);
    const UtcTicks priorityEnd = std::min(options.priorityEnd, options.rangeEnd);
    if (priorityEnd == 0 || priorityStart > priorityEnd)
    {
        // The requested window falls outside the collected range; nothing to prioritize.
        ranges.push_back(whole);
        return ranges;
    }

    ranges.push_back({priorityStart, priorityEnd, true});
    if (priorityEnd < options.rangeEnd)
    {
        // After the incident: records Windows writes late (bugcheck, WER reports).
        ranges.push_back({priorityEnd + 1, options.rangeEnd, false});
    }
    if (priorityStart > options.rangeStart)
    {
        ranges.push_back({options.rangeStart, priorityStart - 1, false});
    }
    return ranges;
}

EventCollection ReadEventLogs(const EventLogReadOptions& options)
{
    EventCollection collection;
    collection.collectedAt = CurrentUtcTime();
    collection.rangeStart = options.rangeStart;
    collection.rangeEnd = options.rangeEnd;

    EventHandle context(EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem));
    if (!context)
    {
        collection.observability.queryErrorOccurred = true;
        return collection;
    }

    RenderBuffers buffers;
    // Only fills systemOldest*; an empty or unprobeable log must not be reported
    // as unreadable here, that is decided by the range queries below.
    ReadOldestSystemEventTime(context.Get(), buffers, collection.observability);

    bool systemReadable = true;
    bool applicationReadable = true;
    collection.observability.priorityWindowRequested = options.hasPriorityWindow;
    // Both channels are read for the priority window before either is read for
    // context, because the same rule can be evidenced from either channel.
    for (const EventScanRange& range : BuildEventScanOrder(options))
    {
        const bool systemRangeRead =
            ReadChannelRange(L"System", EventChannel::System, context.Get(), range, options, buffers, collection);
        const bool applicationRangeRead =
            ReadChannelRange(L"Application", EventChannel::Application, context.Get(), range, options, buffers, collection);
        systemReadable = systemRangeRead && systemReadable;
        applicationReadable = applicationRangeRead && applicationReadable;
        if (range.priority)
        {
            // The priority range is scanned first, so any truncation flag set by
            // now came from this range and really did cost incident evidence.
            collection.observability.priorityWindowComplete =
                systemRangeRead && applicationRangeRead &&
                !collection.observability.budgetExceeded &&
                !collection.observability.eventCapReached &&
                !collection.observability.queryErrorOccurred;
        }
    }
    collection.observability.systemLogReadable = systemReadable;
    collection.observability.applicationLogReadable = applicationReadable;

    // Reverse scanning and the multi-pass order leave the records unsorted.
    // Rules do not depend on order, but the events file is read by humans and
    // replayed in regression tests, so restore chronological order here.
    std::sort(collection.systemEventTimes.begin(), collection.systemEventTimes.end());
    std::stable_sort(collection.events.begin(), collection.events.end(),
        [](const NormalizedEvent& left, const NormalizedEvent& right)
        {
            return left.time < right.time;
        });
    return collection;
}
} // namespace zengine::envcheck::diagnosis
