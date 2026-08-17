#pragma once

#include "EnvironmentProbe.h"

namespace zengine::envcheck
{
void CollectGraphicsInformation(EnvironmentSnapshot& snapshot);
void CollectStorageInformation(EnvironmentSnapshot& snapshot);
} // namespace zengine::envcheck
