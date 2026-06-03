#pragma once

#include "Runtime/Function/Render/RenderGpuResources.h"

// DX12 uses the same GpuMesh / GpuPBRMaterial storage as Vulkan (RHI-abstracted fields on the
// base classes). Do not add parallel empty wrapper types here.
