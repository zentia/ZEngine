// ============================================================================
// Cross-window drag-drop regression test (headless)
// ============================================================================
// Two SlateInputRouters stand for two editor windows (Hierarchy = source,
// Content Browser = target), each with its own widget tree. The source router
// publishes its op through the process-wide channel (SlateDragDrop.h); the
// target router must deliver the drop on the release frame IN EITHER TICK
// ORDER. When the source window ticked first it ended its gesture (cleared the
// channel) on the release frame, and the target's cached copy was only used
// while the button was still down -> the drop was silently lost. That was
// "drag a Hierarchy object onto a Content Browser folder does not make a
// prefab".
//
// Headless: no window, no GPU; geometry comes from painting into the recorder.

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Application/SlateDragDrop.h"
#include "ZSlate/Application/SlateInput.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Testing/SlateRecordingRenderer.h"
#include "ZSlate/Widgets/Layout/SBox.h"
#include "ZSlate/Widgets/SDragSource.h"
#include "ZSlate/Widgets/SDropTarget.h"

#include <chrono>
#include <cstdio>
#include <memory>

namespace
{
class FTestPlatform : public ZSlate::ISlatePlatform
{
public:
    explicit FTestPlatform(ZSlate::ISlateRenderer* renderer) : m_Renderer(renderer) {}
    ZSlate::ISlateRenderer* GetRenderer() override { return m_Renderer; }
    ZSlate::Vector2 GetMousePosition() const override { return ZSlate::Vector2(-1.0f, -1.0f); }
    bool IsMouseButtonDown(int) const override { return false; }
    bool IsKeyDown(int) const override { return false; }
    float GetTimeSeconds() const override
    {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }
    ZSlate::Vector2 GetWindowSize() const override { return ZSlate::Vector2(800.0f, 600.0f); }

private:
    ZSlate::ISlateRenderer* m_Renderer;
};

int g_checks = 0;
int g_failures = 0;
void Check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s\n", what);
    }
}

struct FWindow
{
    std::shared_ptr<ZSlate::SWidget> root;
    ZSlate::UIRect rect;
    ZSlate::SlateInputRouter router;

    void Paint(ZSlate::FSlateRecordingRenderer& rec)
    {
        root->CacheDesiredSize();
        ZSlate::FPaintContext ctx;
        ctx.Renderer = &rec;
        ctx.LayerId = 0;
        root->Paint(ctx, ZSlate::FGeometry(ZSlate::Vector2(rect.x, rect.y), ZSlate::Vector2(rect.w, rect.h)));
    }
    void Tick(const ZSlate::Vector2& mouse, bool left_down)
    {
        const bool over = mouse.x >= rect.x && mouse.x < rect.x + rect.w && mouse.y >= rect.y &&
                          mouse.y < rect.y + rect.h;
        router.ProcessMouse(root, mouse, over, left_down, 0.0f);
    }
};

/// Press on the source window, drag over the target, release there. `source_first`
/// is the per-frame tick order of the two windows.
int RunGesture(bool source_first, int& drops, uint64_t& dropped_id)
{
    ZSlate::FSlateRecordingRenderer rec;
    FTestPlatform platform(&rec);
    ZSlate::SetPlatform(&platform);
    ZSlate::SetActiveDragOperation(nullptr);

    FWindow source;
    source.rect = ZSlate::UIRect(0.0f, 0.0f, 200.0f, 200.0f);
    auto drag = std::make_shared<ZSlate::SDragSource>();
    drag->SetContent(std::make_shared<ZSlate::SBox>());
    drag->OnDragDetectedHandler = [](const ZSlate::Vector2&) {
        auto op = std::make_shared<ZSlate::FDragDropOperation>();
        op->PayloadType = "GObjectID";
        op->Id = 0;  // GObjectID 0 is a valid object id (the invalid one is SIZE_MAX)
        return op;
    };
    source.root = drag;

    FWindow target;
    target.rect = ZSlate::UIRect(300.0f, 0.0f, 200.0f, 200.0f);
    auto drop = std::make_shared<ZSlate::SDropTarget>();
    drop->SetContent(std::make_shared<ZSlate::SBox>());
    drop->CanAcceptDrop = [](const std::shared_ptr<ZSlate::FDragDropOperation>& op) {
        return op != nullptr && op->PayloadType == "GObjectID";
    };
    drop->OnDropHandler = [&](const std::shared_ptr<ZSlate::FDragDropOperation>& op) {
        ++drops;
        dropped_id = op->Id;
    };
    target.root = drop;

    const ZSlate::Vector2 press(50.0f, 50.0f);
    const ZSlate::Vector2 moved(80.0f, 50.0f);
    const ZSlate::Vector2 over_target(400.0f, 100.0f);
    struct Frame
    {
        ZSlate::Vector2 mouse;
        bool down;
    };
    const Frame frames[] = {{press, false}, {press, true}, {moved, true}, {over_target, true}, {over_target, true},
                            {over_target, false}, {over_target, false}};
    for (const Frame& f : frames)
    {
        source.Paint(rec);
        target.Paint(rec);
        if (source_first)
        {
            source.Tick(f.mouse, f.down);
            target.Tick(f.mouse, f.down);
        }
        else
        {
            target.Tick(f.mouse, f.down);
            source.Tick(f.mouse, f.down);
        }
    }
    return ZSlate::IsDragDropActive() ? 1 : 0;
}
}  // namespace

int main()
{
    std::fprintf(stderr, "ZSlate cross-window drag-drop tests\n");
    for (const bool source_first : {false, true})
    {
        int drops = 0;
        uint64_t id = 99;
        const int still_active = RunGesture(source_first, drops, id);
        if (source_first)
        {
            Check(drops == 1, "source window ticks first: the drop must still be delivered (release frame)");
            Check(id == 0, "source first: payload id 0 delivered");
            Check(still_active == 0, "source first: the channel is cleared after the gesture");
        }
        else
        {
            Check(drops == 1, "target window ticks first: the drop is delivered");
            Check(id == 0, "target first: payload id 0 delivered");
            Check(still_active == 0, "target first: the channel is cleared after the gesture");
        }
    }
    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    std::fprintf(stderr, g_failures == 0 ? "ALL PASSED\n" : "FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
