#pragma once

// ============================================================================
// Shared engine precompiled header - used by Runtime, Editor, Launcher, Profiler (C++23)
// ============================================================================

// ============================================================================
// EASTL (3rdparty, header-only usage in PCH; link EASTL/PuertsCore where needed)
// ============================================================================
#include <EASTL/map.h>
#include <EASTL/sort.h>
#include <EASTL/unordered_map.h>
#include <EASTL/unordered_set.h>
#include <EASTL/vector.h>
#include <eastl/string.h>

// ============================================================================
// Standard Library Headers
// ============================================================================
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ============================================================================
// Windows Platform Headers
// ============================================================================
#ifdef _WIN32
    #include <windows.h>
#endif

// ============================================================================
// Engine Core Headers (full set for Runtime; Editor/Launcher/Profiler reuse)
// ============================================================================
#include "Runtime/Core/Base/Factory.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/JsonSerialize/JSONRead.h"
#include "Runtime/Core/JsonSerialize/JSONWrite.h"
#include "Runtime/Core/YamlSerialize/YAMLRead.h"
#include "Runtime/Core/YamlSerialize/YAMLWrite.h"
#include "Runtime/Core/Serialize/TransferFunctions/GenerateTypeTreeTransfer.h"
#include "Runtime/Core/Serialize/TransferFunctions/SafeBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryRead.h"
#include "Runtime/Core/Serialize/TransferFunctions/StreamedBinaryWrite.h"
#include "Runtime/Profiler/Profiler.h"
#include "Runtime/Project/ProjectInfo.h"
#include "Runtime/Utility/TypeUtility.h"
