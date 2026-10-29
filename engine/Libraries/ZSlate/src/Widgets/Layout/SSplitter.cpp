#include "ZSlate/Widgets/Layout/SSplitter.h"

#include "ZSlate/Core/SlateGeometry.h"
#include "ZSlate/Core/SlateReply.h"
#include "ZSlate/Widgets/SLeafWidget.h"

#include <algorithm>

namespace ZSlate
{
class SSplitterHandle : public SLeafWidget
{
public:
    SSplitter* Owner {nullptr};
    UIColor NormalColor {0.18f, 0.18f, 0.20f, 1.0f};
    UIColor HoverColor {0.28f, 0.45f, 0.72f, 1.0f};
    float Width {4.0f};

    Vector2 ComputeDesiredSize() const override { return Vector2(Width, 0.0f); }

    ECursorType GetCursor() const override { return ECursorType::ResizeEW; }

    void OnPaint(const FPaintContext& ctx, const FGeometry& geom) const override
    {
        if (ctx.Renderer == nullptr)
            return;
        ctx.Renderer->DrawQuad(geom.ToRect(), m_Hovered ? HoverColor : NormalColor);
    }

    void OnMouseEnter() override { m_Hovered = true; }
    void OnMouseLeave() override { m_Hovered = false; }

    FReply OnMouseButtonDown(const Vector2& pos, int button) override
    {
        if (button != 0 || Owner == nullptr)
            return FReply::Unhandled();
        m_Dragging = true;
        m_DragStartX = pos.x;
        m_DragStartWidth = Owner->LeftPanelWidth;
        return FReply::Handled().CaptureMouse(const_cast<SSplitterHandle*>(this));
    }

    void OnMouseMove(const Vector2& pos) override
    {
        if (!m_Dragging || Owner == nullptr)
            return;

        const float delta = pos.x - m_DragStartX;
        const float total = Owner->GetCachedGeometry().LocalSize.x;
        const float next = Owner->ClampLeftWidth(m_DragStartWidth + delta, total);
        if (next != Owner->LeftPanelWidth)
        {
            Owner->LeftPanelWidth = next;
            if (Owner->OnLeftPanelResized)
                Owner->OnLeftPanelResized(next);
        }
    }

    FReply OnMouseButtonUp(const Vector2& /*pos*/, int button) override
    {
        if (button != 0)
            return FReply::Unhandled();
        if (m_Dragging && Owner != nullptr && Owner->OnLeftPanelResizeFinished)
            Owner->OnLeftPanelResizeFinished(Owner->LeftPanelWidth);
        m_Dragging = false;
        return FReply::Handled().ReleaseMouseCapture();
    }

    void OnMouseCaptureLost() override
    {
        if (m_Dragging && Owner != nullptr && Owner->OnLeftPanelResizeFinished)
            Owner->OnLeftPanelResizeFinished(Owner->LeftPanelWidth);
        m_Dragging = false;
    }

private:
    bool m_Hovered {false};
    bool m_Dragging {false};
    float m_DragStartX {0.0f};
    float m_DragStartWidth {0.0f};
};

SSplitter::SSplitter()
{
    m_Handle = std::make_shared<SSplitterHandle>();
    m_Handle->Owner = this;
}

void SSplitter::SetLeftContent(std::shared_ptr<SWidget> content)
{
    m_Left = std::move(content);
}

void SSplitter::SetRightContent(std::shared_ptr<SWidget> content)
{
    m_Right = std::move(content);
}

float SSplitter::ClampLeftWidth(float width, float total_width) const
{
    const float max_left = std::max(MinLeftPanelWidth, total_width - HandleWidth - MinRightPanelWidth);
    return std::clamp(width, MinLeftPanelWidth, max_left);
}

Vector2 SSplitter::ComputeDesiredSize() const
{
    float height = 0.0f;
    if (m_Left && m_Left->Visibility != EVisibility::Collapsed)
        height = std::max(height, m_Left->GetDesiredSize().y);
    if (m_Right && m_Right->Visibility != EVisibility::Collapsed)
        height = std::max(height, m_Right->GetDesiredSize().y);
    return Vector2(LeftPanelWidth + HandleWidth + MinRightPanelWidth, height);
}

int SSplitter::GetChildCount() const
{
    int count = 0;
    if (m_Left)
        ++count;
    if (m_Handle)
        ++count;
    if (m_Right)
        ++count;
    return count;
}

std::shared_ptr<SWidget> SSplitter::GetChildAt(int index) const
{
    int cursor = 0;
    if (m_Left)
    {
        if (index == cursor)
            return m_Left;
        ++cursor;
    }
    if (m_Handle)
    {
        if (index == cursor)
            return m_Handle;
        ++cursor;
    }
    if (m_Right)
    {
        if (index == cursor)
            return m_Right;
    }
    return nullptr;
}

void SSplitter::ArrangeChildren(const FGeometry& allotted, std::vector<FArrangedWidget>& out) const
{
    if (m_Handle)
    {
        m_Handle->Owner = const_cast<SSplitter*>(this);
        m_Handle->Width = HandleWidth;
        m_Handle->NormalColor = HandleColor;
        m_Handle->HoverColor = HandleHoverColor;
    }

    const float left_w = ClampLeftWidth(LeftPanelWidth, allotted.LocalSize.x);
    const float right_w = std::max(0.0f, allotted.LocalSize.x - left_w - HandleWidth);

    if (m_Left && m_Left->Visibility != EVisibility::Collapsed)
    {
        out.push_back({m_Left, FGeometry(allotted.AbsolutePosition, Vector2(left_w, allotted.LocalSize.y))});
    }

    const float handle_x = allotted.AbsolutePosition.x + left_w;
    if (m_Handle)
    {
        out.push_back({m_Handle,
                       FGeometry(Vector2(handle_x, allotted.AbsolutePosition.y),
                                 Vector2(HandleWidth, allotted.LocalSize.y))});
    }

    if (m_Right && m_Right->Visibility != EVisibility::Collapsed)
    {
        out.push_back({m_Right,
                       FGeometry(Vector2(handle_x + HandleWidth, allotted.AbsolutePosition.y),
                                 Vector2(right_w, allotted.LocalSize.y))});
    }
}

}  // namespace ZSlate
