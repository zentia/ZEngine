#pragma once

#include "EnvironmentProbe.h"

namespace zengine::envcheck
{
void CollectGraphicsInformation(EnvironmentSnapshot& snapshot);
void CollectStorageInformation(EnvironmentSnapshot& snapshot);

// SMBIOS system-enclosure (type 3) chassis type, 0..127. `known` is false when
// the firmware table is missing or malformed, which is the common case inside a
// VM and on some OEM boards -- callers must treat that as "do not guess".
unsigned int ReadSmbiosChassisType(bool& known);
} // namespace zengine::envcheck
