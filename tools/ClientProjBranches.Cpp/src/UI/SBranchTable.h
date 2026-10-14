#pragma once

#include "ZSlate/Widgets/Views/STableViewBase.h"
#include "ZSlate/Widgets/SHeaderRow.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Core/ZSlateTypes.h"
#include "../Svn/SvnProbe.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <functional>
#include <memory>
#include <vector>

namespace ClientProj {

// =============================================================================
// SBranchTable — multi-column SVN branch table
// =============================================================================
// Inherits STableViewBase for header integration + row hit-testing.
// Each row paints 4 columns: Drive, Type, Name, Revision, Path.
//
class SBranchTable : public ZSlate::STableViewBase<Svn::SvnItem>
{
public:
    SBranchTable()
    {
        // Define columns
        auto header = std::make_shared<ZSlate::SHeaderRow>();
        // SHeaderRow reserves 28px per column for sort-arrow gutter.
        // Text area = Width - 28px.  At FontSize 13, each char ≈ 8px.
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
        RowHeight = 28.0f;
    }

    // ---- Data ----------------------------------------------------------------

    void SetItems(std::vector<Svn::SvnItem> items) { m_Items = std::move(items); }
    const std::vector<Svn::SvnItem>& GetItems() const { return m_Items; }

    int32_t GetRowCount() const override { return static_cast<int32_t>(m_Items.size()); }
    const Svn::SvnItem* GetItemAt(int32_t index) const override
    {
        if (index < 0 || index >= static_cast<int32_t>(m_Items.size())) return nullptr;
        return &m_Items[index];
    }

    // Right-click callback
    std::function<void(int32_t, const ZSlate::Vector2&)> OnItemRightClicked;

    // Double-click callback (C# grids use CellDoubleClick; the branch row opens
    // the working copy folder).
    std::function<void(int32_t)> OnItemDoubleClicked;

    // ---- Layout --------------------------------------------------------------

    ZSlate::Vector2 ComputeDesiredSize() const override
    {
        ZSlate::Vector2 sz = STableViewBase::ComputeDesiredSize();
        sz.x = std::max(sz.x, 920.0f);  // minimum table width
        return sz;
    }

    void OnArrangeChildren(const ZSlate::FGeometry&, std::vector<ZSlate::FArrangedChild>&) const override
    {
        // No child widgets — everything is custom-painted.
    }

    // ---- Paint ---------------------------------------------------------------

    void OnPaint(const ZSlate::FPaintContext& ctx, const ZSlate::FGeometry& geom) const override
    {
        if (!ctx.Renderer) return;

        const ZSlate::UIRect r = geom.ToRect();
        const float headerH = GetHeaderRow() ? GetHeaderRow()->ComputeDesiredSize().y : 0.0f;

        // 0. Table background (slightly lighter than parent so it stands out)
        ctx.Renderer->DrawQuad(r, ZSlate::UIColor(0.14f, 0.16f, 0.20f, 1.0f));
        // Outer border (1px)
        ctx.Renderer->DrawRect(r, ZSlate::UIColor(0.05f, 0.05f, 0.07f, 1.0f), 1.0f);

        // 1. Paint header
        if (GetHeaderRow())
        {
            ZSlate::FGeometry headerGeom(ZSlate::Vector2(r.x, r.y), ZSlate::Vector2(r.w, headerH));
            GetHeaderRow()->OnPaint(ctx, headerGeom);
        }

        // 2. Get column widths from header
        float colX[5] = {};
        float colW[5] = {};
        if (auto* hdr = GetHeaderRow().get())
        {
            float cx = r.x;
            for (size_t i = 0; i < hdr->Columns.size() && i < 5; ++i)
            {
                colX[i] = cx;
                colW[i] = hdr->Columns[i].Width;
                cx += colW[i];
            }
        }

        // 3. Paint rows
        const float fontSize = 13.0f;
        const int32_t n = static_cast<int32_t>(m_Items.size());
        const float viewTop = r.y + headerH;
        const float viewBot = r.Bottom();

        for (int32_t i = 0; i < n; ++i)
        {
            const auto& item = m_Items[i];
            float rowY = viewTop + static_cast<float>(i) * RowHeight - m_ScrollOffset;
            // Skip rows outside the visible area
            if (rowY + RowHeight < viewTop) continue;
            if (rowY > viewBot) break;
            ZSlate::UIRect rowRect(r.x, rowY, r.w, RowHeight);

            // Row background (alternating colors)
            PaintRowBackground(ctx, rowRect, i);

            // Vertical column dividers (very subtle)
            for (int c = 0; c < 4; ++c) {
                if (colX[c] + colW[c] > 0) {
                    ZSlate::UIRect divRect(colX[c] + colW[c] - 1.0f, rowY, 1.0f, RowHeight);
                    ctx.Renderer->DrawQuad(divRect, ZSlate::UIColor(0.05f, 0.05f, 0.07f, 1.0f));
                }
            }

            // Column texts
            auto drawCell = [&](int col, const std::string& text, ZSlate::UIColor color) {
                if (col < 0 || col >= 5 || colW[col] <= 0) return;
                ZSlate::UIRect cr(colX[col] + 8.0f, rowY, colW[col] - 12.0f, RowHeight);
                ctx.Renderer->DrawTextLabel(cr, text, fontSize, color,
                    ZSlate::TextAnchor::MiddleLeft, ZSlate::TextWrapMode::NoWrap, nullptr);
            };

            // Color-code the BranchType
            ZSlate::UIColor typeColor(0.80f, 0.82f, 0.88f, 1.0f);  // default
            if (item.BranchType == "trunk")       typeColor = ZSlate::UIColor(0.40f, 0.90f, 0.45f, 1.0f);
            else if (item.BranchType == "branches") typeColor = ZSlate::UIColor(0.50f, 0.72f, 1.00f, 1.0f);
            else if (item.BranchType == "tags")      typeColor = ZSlate::UIColor(0.95f, 0.70f, 0.20f, 1.0f);

            drawCell(0, item.Drive,       ZSlate::UIColor(0.65f, 0.70f, 0.80f, 1.0f));
            drawCell(1, item.BranchType,  typeColor);
            drawCell(2, item.BranchName,  ZSlate::UIColor(0.95f, 0.97f, 1.00f, 1.0f));
            drawCell(3, "r" + item.Revision, ZSlate::UIColor(0.75f, 0.80f, 0.90f, 1.0f));
            drawCell(4, item.Path,        ZSlate::UIColor(0.70f, 0.75f, 0.85f, 1.0f));
        }

        // 4. Empty-state message
        if (n == 0) {
            ZSlate::UIRect er(r.x, r.y + headerH + 12.0f, r.w, 24.0f);
            ctx.Renderer->DrawTextLabel(er,
                "No working copies found. Use 'Manage Paths' to add scan directories.",
                13.0f, ZSlate::UIColor(0.55f, 0.60f, 0.70f, 1.0f),
                ZSlate::TextAnchor::MiddleCenter, ZSlate::TextWrapMode::NoWrap, nullptr);
        }
    }

    // ---- Input ---------------------------------------------------------------

    // The header strip is custom-painted rather than a child widget, so it is
    // never hit-tested; forward the top strip to it by hand.
    bool IsInHeaderStrip(const ZSlate::Vector2& absolutePos) const
    {
        auto header = GetHeaderRow();
        if (!header) return false;
        const ZSlate::UIRect r = GetCachedGeometry().ToRect();
        return absolutePos.x >= r.x && absolutePos.x < r.Right() &&
               absolutePos.y >= r.y && absolutePos.y < r.y + header->RowHeight;
    }

    ZSlate::FReply OnMouseMove(const ZSlate::Vector2& pos) override
    {
        if (auto header = GetHeaderRow(); header && IsInHeaderStrip(pos))
            header->OnMouseMove(pos);
        return ZSlate::FReply::Unhandled();
    }

    ZSlate::FReply OnMouseButtonDown(const ZSlate::Vector2& pos, int btn) override
    {
        if (auto header = GetHeaderRow(); header && IsInHeaderStrip(pos) && btn == 0)
            return header->OnMouseButtonDown(pos, btn);

        if (btn == 0 || btn == 1)
        {
            int32_t idx = HitTestRow(pos, GetCachedGeometry().ToRect());
            if (idx >= 0 && idx < GetRowCount())
            {
                SelectedIndex = idx;
                if (OnSelectionChanged)
                    OnSelectionChanged(m_Items[idx], idx);
            }
            return ZSlate::FReply::Handled();
        }
        return ZSlate::FReply::Unhandled();
    }

    ZSlate::FReply OnMouseButtonUp(const ZSlate::Vector2& pos, int btn) override
    {
        if (auto header = GetHeaderRow(); header && IsInHeaderStrip(pos) && btn == 0)
            return header->OnMouseButtonUp(pos, btn);

        if (btn == 1)  // right button
        {
            int32_t idx = HitTestRow(pos, GetCachedGeometry().ToRect());
            if (idx >= 0 && idx < GetRowCount() && OnItemRightClicked)
            {
                SelectedIndex = idx;
                if (OnSelectionChanged)
                    OnSelectionChanged(m_Items[idx], idx);
                POINT pt; GetCursorPos(&pt);
                OnItemRightClicked(idx, ZSlate::Vector2((float)pt.x, (float)pt.y));
            }
        }
        return ZSlate::FReply::Handled();
    }

    ZSlate::FReply OnMouseButtonDoubleClick(const ZSlate::Vector2& pos, int btn) override
    {
        if (auto header = GetHeaderRow(); header && IsInHeaderStrip(pos))
            return header->OnMouseButtonDoubleClick(pos, btn);

        if (btn != 0) return ZSlate::FReply::Unhandled();

        const int32_t idx = HitTestRow(pos, GetCachedGeometry().ToRect());
        if (idx < 0 || idx >= GetRowCount()) return ZSlate::FReply::Unhandled();

        SelectedIndex = idx;
        if (OnSelectionChanged) OnSelectionChanged(m_Items[idx], idx);
        if (OnItemDoubleClicked) OnItemDoubleClicked(idx);
        return ZSlate::FReply::Handled();
    }

    ZSlate::FReply OnMouseWheel(const ZSlate::Vector2& /*pos*/, float delta) override
    {
        m_ScrollOffset -= delta * 32.0f;
        if (m_ScrollOffset < 0.0f) m_ScrollOffset = 0.0f;
        float viewH = GetCachedGeometry().LocalSize.y;
        float headerH = GetHeaderRow() ? GetHeaderRow()->ComputeDesiredSize().y : 0.0f;
        float contentH = GetRowCount() * RowHeight;
        float maxScroll = std::max(0.0f, contentH - (viewH - headerH));
        if (m_ScrollOffset > maxScroll) m_ScrollOffset = maxScroll;
        return ZSlate::FReply::Handled();
    }

private:
    std::vector<Svn::SvnItem> m_Items;
    mutable float m_ScrollOffset = 0.0f;
};

} // namespace ClientProj
