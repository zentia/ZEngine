#include "Runtime/UI/Render/UIRenderer.h"

// The only live UIRenderer backend is BatchedUIRenderer (native UiRenderBatch -> RHI).
// The former ImGui-draw-list backend (ImGuiUIRenderer) was removed: it was never
// instantiated (CreateImGuiUIRenderer had no callers) and only existed as the original
// bootstrap path before the native batcher landed.

UIRenderer* CreateDefaultUIRenderer()
{
    return CreateBatchedUIRenderer();
}

void DestroyUIRenderer(UIRenderer* renderer)
{
    delete renderer;
}
