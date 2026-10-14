#pragma once

#include "ZSlate/Widgets/Views/STableViewBase.h"
#include "ZSlate/Widgets/SHeaderRow.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Svn/SvnProbe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace ClientProj {

// =============================================================================
// SBranchTable — multi-column SVN branch table
// =============================================================================
// Inherits STableViewBase for header + scroll state. Rows are custom-painted
// (Drive, Type, Name, Revision, Path) instead of being child widgets.
//
class SBranchTable : public ZSlate::STableViewBase
{
public:
    SBranchTable()
    {
        auto header = std::make_shared<ZSlate::SHeaderRow>();
        // SHeaderRow reserves 28px per column for the sort-arrow gutter.
        header->Columns = {
            {"Drive",      70.0f, 50.0f},
            {"Type",       95.0f, 70.0f},
            {"Name",      180.0f, 120.0f},
            {"Revision",   90.0f, 70.0f},
            {"Path",      475.0f, 180.0f},
        };
        header->RowHeight = 28.0f;
        header->FontSize = 13.0f;

        SetHeaderRow(header);
        ItemHeight = 28.0f;
    }

    // ---- Data ----------------------------------------------------------------

    void SetItems(std::vector<Svn::SvnItem> items)
    {
        m_Items = std::move(items);
        if (SelectedIndex >= GetRowCount()) SelectedIndex = -1;
    }
    const std::vector<Svn::SvnItem>& GetItems() const { return m_Items; }
    int32_t GetRowCount() const { return static_cast<int32_t>(m_Items.size()); }

    int32_t SelectedIndex = -1;

    std::function<void(const Svn::SvnItem&, int32_t)> OnSelectionChanged;
    std::function<void(int32_t, const ZSlate::Vector2&)> OnItemRightClicked;   // screen position
    std::function<void(int32_t)> OnItemDoubleClicked;

    // ---- Layout --------------------------------------------------------------

    ZSlate::Vector2 ComputeDesiredSize() const override
    {
        return ZSlate::Vector2(std::max(HeaderWidth(), 920.0f),
                               GetHeaderHeight() + ItemHeight * static_cast<float>(GetRowCount()));
    }

    // ---- Paint ---------------------------------------------------------------

    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;

        const ZSlate::UIRect r = geom.ToRect();
        const float headerH = GetHeaderHeight();

        // Table background + 1px outline
        ctx.Renderer->DrawQuad(r, ZSlate::UIColor(0.14f, 0.16f, 0.20f, 1.0f));
        ctx.Renderer->DrawRect(r, ZSlate::UIColor(0.05f, 0.05f, 0.07f, 1.0f), 1.0f);

        PaintHeader(ctx, geom);

        // Column extents from the header (display order)
        float colX[5] = {};
        float colW[5] = {};
        int   colField[5] = {-1, -1, -1, -1, -1};
        if (const auto& hdr = GetHeaderRow())
        {
            float cx = r.x;
            for (size_t i = 0; i < hdr->Columns.size() && i < 5; ++i)
            {
                colX[i] = cx;
                colW[i] = hdr->Columns[i].Width;
                colField[i] = FieldIndex(hdr->Columns[i].Label);
                cx += colW[i];
            }
        }

        const float viewTop = r.y + headerH;
        const float viewH   = std::max(0.0f, r.h - headerH);
        ClampScroll(viewH);

        ctx.Renderer->PushClipRect(ZSlate::UIRect(r.x, viewTop, r.w, viewH));

        const float fontSize = 13.0f;
        const int32_t n = GetRowCount();
        for (int32_t i = 0; i < n; ++i)
        {
            const auto& item = m_Items[i];
            const float rowY = viewTop + static_cast<float>(i) * ItemHeight - m_ScrollOffset;
            if (rowY + ItemHeight < viewTop) continue;
            if (rowY > r.Bottom()) break;
            const ZSlate::UIRect rowRect(r.x, rowY, r.w, ItemHeight);

            // Row background: selection > hover > alternating stripes
            ZSlate::UIColor rowColor = (i % 2 == 0) ? ZSlate::UIColor(0.14f, 0.16f, 0.20f, 1.0f)
                                                    : ZSlate::UIColor(0.16f, 0.18f, 0.22f, 1.0f);
            if (i == SelectedIndex)       rowColor = ZSlate::UIColor(0.20f, 0.36f, 0.62f, 1.0f);
            else if (i == m_HoveredIndex) rowColor = ZSlate::UIColor(0.20f, 0.23f, 0.29f, 1.0f);
            ctx.Renderer->DrawQuad(rowRect, rowColor);

            // Vertical column dividers (very subtle)
            for (int c = 0; c < 4; ++c)
            {
                if (colW[c] > 0.0f)
                    ctx.Renderer->DrawQuad(ZSlate::UIRect(colX[c] + colW[c] - 1.0f, rowY, 1.0f, ItemHeight),
                                           ZSlate::UIColor(0.05f, 0.05f, 0.07f, 1.0f));
            }

            // Color-code the BranchType
            ZSlate::UIColor typeColor(0.80f, 0.82f, 0.88f, 1.0f);
            if (item.BranchType == "trunk")         typeColor = ZSlate::UIColor(0.40f, 0.90f, 0.45f, 1.0f);
            else if (item.BranchType == "branches") typeColor = ZSlate::UIColor(0.50f, 0.72f, 1.00f, 1.0f);
            else if (item.BranchType == "tags")     typeColor = ZSlate::UIColor(0.95f, 0.70f, 0.20f, 1.0f);

            for (int c = 0; c < 5; ++c)
            {
                if (colW[c] <= 0.0f || colField[c] < 0) continue;
                std::string text;
                ZSlate::UIColor color;
                switch (colField[c])
                {
                case 0: text = item.Drive;            color = ZSlate::UIColor(0.65f, 0.70f, 0.80f, 1.0f); break;
                case 1: text = item.BranchType;       color = typeColor; break;
                case 2: text = item.BranchName;       color = ZSlate::UIColor(0.95f, 0.97f, 1.00f, 1.0f); break;
                case 3: text = "r" + item.Revision;   color = ZSlate::UIColor(0.75f, 0.80f, 0.90f, 1.0f); break;
                default: text = item.Path;            color = ZSlate::UIColor(0.70f, 0.75f, 0.85f, 1.0f); break;
                }
                const ZSlate::UIRect cell(colX[c] + 8.0f, rowY, std::max(0.0f, colW[c] - 12.0f), ItemHeight);
                ctx.Renderer->PushClipRect(cell);
                ctx.Renderer->DrawTextLabel(cell, text, fontSize, color,
                    ZSlate::TextAnchor::MiddleLeft, ZSlate::TextWrapMode::NoWrap, nullptr);
                ctx.Renderer->PopClipRect();
            }
        }

        ctx.Renderer->PopClipRect();

        // Empty-state message
        if (n == 0)
        {
            ZSlate::UIRect er(r.x, viewTop + 12.0f, r.w, 24.0f);
            ctx.Renderer->DrawTextLabel(er,
                "No working copies found. Use 'Manage Paths' to add scan directories.",
                13.0f, ZSlate::UIColor(0.55f, 0.60f, 0.70f, 1.0f),
                ZSlate::TextAnchor::MiddleCenter, ZSlate::TextWrapMode::NoWrap, nullptr);
        }
    }

    // ---- Input ---------------------------------------------------------------
    // The header strip is painted rather than routed as a child, so forward
    // its events by hand.

    void OnMouseMove(const ZSlate::Vector2& pos) override
    {
        if (const auto& header = GetHeaderRow())
            header->OnMouseMove(pos);   // also drives an in-flight resize/reorder
        m_HoveredIndex = HitTestRow(pos);
    }

    void OnMouseLeave() override { m_HoveredIndex = -1; }

    ZSlate::FReply OnMouseButtonDown(const ZSlate::Vector2& pos, int btn) override
    {
        if (IsInHeaderStrip(pos))
        {
            if (btn == 0)
            {
                GetHeaderRow()->OnMouseButtonDown(pos, btn);
                m_HeaderPressed = true;
                // Capture on the table so the header keeps receiving moves/ups.
                return ZSlate::FReply::Handled().CaptureMouse(this);
            }
            return ZSlate::FReply::Handled();
        }

        if (btn != 0 && btn != 1) return ZSlate::FReply::Unhandled();

        const int32_t idx = HitTestRow(pos);
        if (idx >= 0) Select(idx);

        if (btn == 1 && idx >= 0 && OnItemRightClicked)
        {
            POINT pt; GetCursorPos(&pt);
            OnItemRightClicked(idx, ZSlate::Vector2(static_cast<float>(pt.x), static_cast<float>(pt.y)));
        }
        return ZSlate::FReply::Handled();
    }

    ZSlate::FReply OnMouseButtonUp(const ZSlate::Vector2& pos, int btn) override
    {
        if (btn == 0 && m_HeaderPressed)
        {
            m_HeaderPressed = false;
            GetHeaderRow()->OnMouseButtonUp(pos, btn);
            return ZSlate::FReply::Handled().ReleaseMouseCapture();
        }
        return ZSlate::FReply::Unhandled();
    }

    void OnMouseCaptureLost() override
    {
        if (m_HeaderPressed)
        {
            m_HeaderPressed = false;
            GetHeaderRow()->OnMouseCaptureLost();
        }
    }

    ZSlate::FReply OnMouseButtonDoubleClick(const ZSlate::Vector2& pos, int btn) override
    {
        if (btn != 0 || IsInHeaderStrip(pos)) return ZSlate::FReply::Unhandled();

        const int32_t idx = HitTestRow(pos);
        if (idx < 0) return ZSlate::FReply::Unhandled();

        Select(idx);
        if (OnItemDoubleClicked) OnItemDoubleClicked(idx);
        return ZSlate::FReply::Handled();
    }

    ZSlate::FReply OnMouseWheel(const ZSlate::Vector2& /*pos*/, float delta) override
    {
        m_ScrollOffset -= delta * 32.0f;
        ClampScroll(std::max(0.0f, GetCachedGeometry().LocalSize.y - GetHeaderHeight()));
        return ZSlate::FReply::Handled();
    }

private:
    static int FieldIndex(const std::string& label)
    {
        if (label == "Drive")    return 0;
        if (label == "Type")     return 1;
        if (label == "Name")     return 2;
        if (label == "Revision") return 3;
        if (label == "Path")     return 4;
        return -1;
    }

    float HeaderWidth() const
    {
        return GetHeaderRow() ? GetHeaderRow()->ComputeDesiredSize().x : 0.0f;
    }

    bool IsInHeaderStrip(const ZSlate::Vector2& pos) const
    {
        if (!GetHeaderRow()) return false;
        const ZSlate::UIRect r = GetCachedGeometry().ToRect();
        return pos.x >= r.x && pos.x < r.Right() && pos.y >= r.y && pos.y < r.y + GetHeaderHeight();
    }

    int32_t HitTestRow(const ZSlate::Vector2& pos) const
    {
        const ZSlate::UIRect r = GetCachedGeometry().ToRect();
        const float top = r.y + GetHeaderHeight();
        if (ItemHeight <= 0.0f || pos.x < r.x || pos.x >= r.Right() || pos.y < top || pos.y >= r.Bottom())
            return -1;
        const int32_t idx = static_cast<int32_t>((pos.y - top + m_ScrollOffset) / ItemHeight);
        return (idx >= 0 && idx < GetRowCount()) ? idx : -1;
    }

    void Select(int32_t idx)
    {
        SelectedIndex = idx;
        if (OnSelectionChanged) OnSelectionChanged(m_Items[idx], idx);
    }

    void ClampScroll(float viewH) const
    {
        const float maxScroll = std::max(0.0f, ItemHeight * static_cast<float>(GetRowCount()) - viewH);
        const_cast<SBranchTable*>(this)->m_ScrollOffset = std::clamp(m_ScrollOffset, 0.0f, maxScroll);
    }

    std::vector<Svn::SvnItem> m_Items;
    int32_t m_HoveredIndex = -1;
    bool    m_HeaderPressed = false;
};

} // namespace ClientProj
