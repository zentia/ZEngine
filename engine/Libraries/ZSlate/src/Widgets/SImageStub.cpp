// SImage::OnPaint out-of-line definition for linker.
#include "ZSlate/Widgets/SImage.h"
namespace ZSlate {
void SImage::OnPaint(const FPaintContext& c, const FGeometry& g) const {
    if (c.Renderer) c.Renderer->DrawQuad(g.ToRect(), UIColor(0.3f,0.3f,0.3f,1.0f));
}
}
