#pragma once

#include "DiagnosisTypes.h"

#include <cstdint>
#include <vector>

namespace zengine::envcheck::diagnosis
{
struct EventLogReadOptions
{
    UtcTicks rangeStart = 0;
    UtcTicks rangeEnd = 0;
    // Window that decides the verdict, scanned before the rest of the range so
    // the event cap and the deadline can never drop the records that matter.
    // Without it a noisy machine fills the cap with old events and the incident
    // window is never reached. Set it whenever an incident time is known.
    bool hasPriorityWindow = false;
    UtcTicks priorityStart = 0;
    UtcTicks priorityEnd = 0;
    // GetTickCount64() value after which reading stops; 0 means no deadline.
    std::uint64_t deadlineTickCount = 0;
};

// One closed time interval to query. Both bounds are inclusive, so consecutive
// intervals must stay at least one tick apart or an event is read twice.
// start == 0 means "no lower bound".
struct EventScanRange
{
    UtcTicks start = 0;
    UtcTicks end = 0;
    // True for the window that decides the verdict. It is always scanned first,
    // so truncation after it costs context only. Stays false when the requested
    // window lies outside the collected range and a single pass is used instead.
    bool priority = false;
};

bool IsEmptyEventScanRange(const EventScanRange& range);

// Scan order, most relevant first. Entry 0 is the priority window when one is
// requested; everything after it is context, so a cap or deadline that cuts the
// scan short can only cost context. Exposed for regression tests.
std::vector<EventScanRange> BuildEventScanOrder(const EventLogReadOptions& options);

// Reads the System and Application channels in-process through wevtapi.
// Never throws for "log not readable": it is reported through Observability.
EventCollection ReadEventLogs(const EventLogReadOptions& options);
} // namespace zengine::envcheck::diagnosis
