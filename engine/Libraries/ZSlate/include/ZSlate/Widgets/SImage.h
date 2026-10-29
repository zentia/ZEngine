#pragma once

// Forward to canonical SImage implementation (brush-based).
// Kept for backward compatibility — prefer including Panels/SImage.h directly.
#include "ZSlate/Widgets/Panels/SImage.h"

// Backward-compat aliases for root-path includes
namespace ZSlate
{
// SImage::TextureHandle → use FSlateBrush::TextureId instead
}
