#pragma once

#include "DiagnosisTypes.h"

#include <cstdint>
#include <string>

namespace zengine::envcheck::diagnosis
{
// Half width of the "nearby" window around an incident. The reader must collect
// at least this much before the incident for the rules to see the whole window.
constexpr UtcTicks IncidentWindowMargin = 30ull * TicksPerMinute;

// Event filter shared with the reader so that the rule table stays the single
// source of truth for "which events matter". providerLower must be lowercase.
bool IsRelevantEvent(EventChannel channel, const std::string& providerLower, std::uint32_t eventId, std::uint32_t level);

// Pure function: no system calls. Same input always gives the same result, which
// lets the events file be replayed for regression tests.
DiagnosisResult Diagnose(const DiagnosisRequest& request, const EventCollection& collection);
} // namespace zengine::envcheck::diagnosis
