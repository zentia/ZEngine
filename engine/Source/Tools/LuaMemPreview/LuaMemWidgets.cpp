#include "LuaMemWidgets.h"

#include "LuaMemTypeDoc.h"

#include "ZSlate/Core/SlatePaint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace LuaMemPreview
{
using namespace ZSlate;

namespace
{
// --- Metrics ---------------------------------------------------------------
constexpr float kRowHeight = 22.0f;
constexpr float kHeaderHeight = 28.0f;
constexpr float kScrollbarWidth = 10.0f;
constexpr float kRowFontSize = 12.5f;
constexpr float kHeaderFontSize = 11.5f;
constexpr float kCellPadX = 8.0f;
// bar(8) + gap(6) + 2 chip rows(20 each) + row gap(4) + top/bottom pad(8+6)
constexpr float kTypeBarHeight = 80.0f;

// --- Palette ---------------------------------------------------------------
constexpr UIColor kBgPanel {0.106f, 0.106f, 0.118f, 1.0f};
constexpr UIColor kBgHeader {0.137f, 0.137f, 0.153f, 1.0f};
constexpr UIColor kBgRowAlt {0.125f, 0.125f, 0.137f, 1.0f};
constexpr UIColor kBgRowHover {0.180f, 0.180f, 0.200f, 1.0f};
constexpr UIColor kBgSelected {0.153f, 0.278f, 0.471f, 1.0f};
constexpr UIColor kTextMain {0.878f, 0.878f, 0.902f, 1.0f};
constexpr UIColor kTextDim {0.569f, 0.569f, 0.608f, 1.0f};
constexpr UIColor kBorder {0.216f, 0.216f, 0.235f, 1.0f};
constexpr UIColor kAccent {0.306f, 0.631f, 1.0f, 1.0f};
constexpr UIColor kScrollThumb {0.333f, 0.333f, 0.365f, 1.0f};
// Used for the vm_other chip when the residual is non-zero, i.e. the buckets
// do not add up to Lua's own total and the split cannot be fully trusted.
constexpr UIColor kWarn {0.925f, 0.682f, 0.306f, 1.0f};

// Lowercase ASCII hash for the unknown-type fallback hue.
uint32_t HashString(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (const char c : s)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

UIColor HsvToRgb(float h, float s, float v)
{
    const float c = v * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hp < 1)      { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    const float m = v - c;
    return UIColor(r + m, g + m, b + m, 1.0f);
}
}  // namespace

// ============================================================================
// Type palette - mirrors the tags in ltracker.h (lua_track_type_t).
// ============================================================================
UIColor ColorForType(const std::string& t)
{
    if (t == "table")        return UIColor(0.306f, 0.631f, 1.000f, 1.0f);
    if (t == "string")       return UIColor(0.624f, 0.816f, 0.659f, 1.0f);
    if (t == "userdata")     return UIColor(0.878f, 0.643f, 0.345f, 1.0f);
    if (t == "lua_closure")  return UIColor(0.780f, 0.573f, 0.918f, 1.0f);
    if (t == "c_closure")    return UIColor(0.627f, 0.478f, 0.839f, 1.0f);
    if (t == "proto")        return UIColor(1.000f, 0.561f, 0.439f, 1.0f);
    if (t == "upval")        return UIColor(0.373f, 0.702f, 0.702f, 1.0f);
    if (t == "thread")       return UIColor(0.941f, 0.443f, 0.471f, 1.0f);
    if (t == "string_table") return UIColor(0.541f, 0.541f, 0.573f, 1.0f);
    if (t == "vm_other")     return UIColor(0.353f, 0.353f, 0.384f, 1.0f);
    // Unknown tag (e.g. a bucket added later): derive a stable hue so it still
    // renders distinctly instead of collapsing onto "table".
    return HsvToRgb(static_cast<float>(HashString(t) % 360u), 0.55f, 0.75f);
}

// ============================================================================
// SLuaTypeBar
// ============================================================================

Vector2 SLuaTypeBar::ComputeDesiredSize() const
{
    // Fixed height on purpose. Deriving it from the previous frame's chip
    // layout would make the panel's size depend on its own paint output, which
    // both lags one frame behind a resize and makes a single-frame headless
    // check disagree with the steady state. Two chip rows always fit the 11
    // buckets ltracker.h can emit at any sane window width.
    return Vector2(400.0f, kTypeBarHeight);
}

void SLuaTypeBar::OnPaint(const FPaintContext& ctx, const FGeometry& geom) const
{
    ISlateRenderer* r = ctx.Renderer;
    if (r == nullptr || Snapshot == nullptr || !Snapshot->IsLoaded())
        return;

    const UIRect view = geom.ToRect();
    r->DrawQuad(view, kBgPanel);

    const std::vector<FTypeBucket>& buckets = Snapshot->GetTypeBuckets();
    const uint64_t total = Snapshot->GetTotalSize();
    if (total == 0 || buckets.empty())
        return;

    // --- Proportion bar ----------------------------------------------------
    const float bar_y = view.y + 8.0f;
    const float bar_h = 8.0f;
    const float bar_w = view.w - 2.0f * kCellPadX;
    r->DrawQuad(UIRect(view.x + kCellPadX, bar_y, bar_w, bar_h), UIColor(0.165f, 0.165f, 0.180f, 1.0f));

    float x = view.x + kCellPadX;
    for (const FTypeBucket& b : buckets)
    {
        const float w = bar_w * (static_cast<float>(b.Size) / static_cast<float>(total));
        if (w < 0.5f)
            continue;
        r->DrawQuad(UIRect(x, bar_y, w, bar_h), ColorForType(Snapshot->GetTypeName(b.TypeId)));
        x += w;
    }

    // --- Chips -------------------------------------------------------------
    m_ChipHits.clear();
    const float chip_h = 20.0f;
    const float chip_gap = 6.0f;
    const float row_gap = 4.0f;
    float cx = view.x + kCellPadX;
    float cy = bar_y + bar_h + 6.0f;
    int chip_row = 0;

    const uint16_t active = (View != nullptr) ? View->GetTypeFilter() : FRowView::kAllTypes;

    // Total chip first: it is the reconciliation anchor, so it reads as the
    // headline number rather than being buried among the types.
    struct FChipDef
    {
        std::string Label;
        UIColor Dot;
        uint16_t TypeId;
        bool ShowDot;
        uint64_t Size {0};
    };
    std::vector<FChipDef> defs;
    defs.reserve(buckets.size() + 1);
    {
        FChipDef d;
        d.Label = "ALL  " + FormatBytes(total);
        d.Dot = kAccent;
        d.TypeId = FRowView::kAllTypes;
        d.ShowDot = false;
        defs.push_back(std::move(d));
    }
    for (const FTypeBucket& b : buckets)
    {
        const std::string& name = Snapshot->GetTypeName(b.TypeId);
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%.1f%%", static_cast<double>(b.Size) * 100.0 / static_cast<double>(total));
        FChipDef d;
        d.Label = name + "  " + FormatBytes(b.Size) + "  " + pct;
        d.Dot = ColorForType(name);
        d.TypeId = b.TypeId;
        d.ShowDot = true;
        d.Size = b.Size;
        defs.push_back(std::move(d));
    }

    for (const FChipDef& d : defs)
    {
        const float text_w = r->MeasureText(d.Label, 11.0f).x;
        const float chip_w = text_w + (d.ShowDot ? 26.0f : 16.0f);

        if (cx + chip_w > view.x + view.w - kCellPadX && cx > view.x + kCellPadX)
        {
            // Wrap. Only two rows fit in the reserved height; the rest is
            // reachable through the table's own type column anyway.
            if (++chip_row >= 2)
                break;
            cx = view.x + kCellPadX;
            cy += chip_h + row_gap;
        }

        const UIRect chip(cx, cy, chip_w, chip_h);
        const bool is_active = (d.TypeId == active);
        const bool is_hover = (static_cast<int>(m_ChipHits.size()) == m_HoverChip);

        // vm_other is the residual bucket: a non-zero value means some
        // allocation path is not covered, so every other percentage here is
        // suspect. That is a statement about the whole snapshot's
        // trustworthiness, so it is flagged on the bar instead of being left
        // for whoever happens to hover the chip. Zero is the healthy case and
        // must stay unmarked.
        const bool is_unaccounted =
            d.ShowDot && d.Size > 0 && Snapshot->GetTypeName(d.TypeId) == "vm_other";

        if (is_active)
            r->DrawQuad(chip, kBgSelected);
        else if (is_hover)
            r->DrawQuad(chip, kBgRowHover);
        r->DrawRect(chip, is_active ? kAccent : (is_unaccounted ? kWarn : kBorder), 1.0f);

        float tx = cx + 8.0f;
        if (d.ShowDot)
        {
            r->DrawQuad(UIRect(tx, cy + chip_h * 0.5f - 4.0f, 8.0f, 8.0f), d.Dot);
            tx += 12.0f;
        }
        r->DrawTextLabel(UIRect(tx, cy, text_w + 2.0f, chip_h), d.Label, 11.0f,
                         (is_active || is_hover) ? kTextMain : kTextDim,
                         TextAnchor::MiddleLeft, TextWrapMode::NoWrap);

        m_ChipHits.push_back(FChipHit {chip, d.TypeId});
        cx += chip_w + chip_gap;
    }

    r->DrawQuad(UIRect(view.x, view.y + view.h - 1.0f, view.w, 1.0f), kBorder);
}

void PaintTypeDocCard(ISlateRenderer* r, const UIRect& screen, const SLuaTypeBar& bar,
                      const FSnapshot& snapshot)
{
    if (r == nullptr)
        return;
    const SLuaTypeBar::FHoverInfo hover = bar.GetHoveredChip();
    if (!hover.Valid)
        return;

    const std::string& name = snapshot.GetTypeName(hover.TypeId);
    const char* detail = TypeDetail(name);
    const std::string body = (detail != nullptr && detail[0] != '\0') ? detail : TypeSummary(name);

    // Wrapping is done here rather than by passing TextWrapMode::Wrap, because
    // the card has to be sized to the text before it is placed, and the text
    // already carries deliberate line breaks that a re-wrap would lose.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : body)
        {
            if (c == '\n')
            {
                lines.push_back(cur);
                cur.clear();
            }
            else
            {
                cur.push_back(c);
            }
        }
        lines.push_back(cur);
    }

    constexpr float kFont = 11.0f;
    constexpr float kTitleFont = 12.0f;
    constexpr float kPad = 10.0f;
    constexpr float kLineH = 15.0f;

    const std::string title = name + (IsNonGcBucket(name) ? "   (非 GC 对象)" : "");

    float text_w = r->MeasureText(title, kTitleFont).x;
    for (const std::string& ln : lines)
    {
        const float w = r->MeasureText(ln, kFont).x;
        if (w > text_w)
            text_w = w;
    }

    // Cap the width so a long line pushes the card narrower rather than off
    // the edge of the screen. Clamping the placement alone is not enough: the
    // card is anchored left, so an over-wide card would still overhang right.
    const float max_card_w = screen.w - 2.0f * kPad;
    float card_w = text_w + 2.0f * kPad;
    if (card_w > max_card_w)
    {
        card_w = max_card_w;
        text_w = card_w - 2.0f * kPad;
    }
    const float card_h = kPad + kLineH + 6.0f + static_cast<float>(lines.size()) * kLineH + kPad;

    // Prefer below the chip; flip above when it would run off the bottom.
    float cx = hover.ChipRect.x;
    float cy = hover.ChipRect.y + hover.ChipRect.h + 6.0f;
    if (cy + card_h > screen.y + screen.h)
        cy = hover.ChipRect.y - card_h - 6.0f;
    if (cy < screen.y)
        cy = screen.y;
    if (cx + card_w > screen.x + screen.w)
        cx = screen.x + screen.w - card_w;
    if (cx < screen.x)
        cx = screen.x;

    const UIRect card(cx, cy, card_w, card_h);
    r->DrawQuad(card, UIColor(0.055f, 0.055f, 0.067f, 0.98f));
    r->DrawRect(card, ColorForType(name), 1.0f);

    float y = cy + kPad;
    r->DrawTextLabel(UIRect(cx + kPad, y, text_w, kLineH), title, kTitleFont, ColorForType(name),
                     TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
    y += kLineH + 6.0f;
    for (const std::string& ln : lines)
    {
        r->DrawTextLabel(UIRect(cx + kPad, y, text_w, kLineH), ln, kFont, kTextMain,
                         TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        y += kLineH;
    }
}

SLuaTypeBar::FHoverInfo SLuaTypeBar::GetHoveredChip() const
{
    FHoverInfo info;
    if (m_HoverChip < 0 || m_HoverChip >= static_cast<int>(m_ChipHits.size()))
        return info;
    const FChipHit& hit = m_ChipHits[static_cast<size_t>(m_HoverChip)];
    // The ALL chip is the reconciliation total, not a type; it has no doc entry.
    if (hit.TypeId == FRowView::kAllTypes)
        return info;
    info.Valid = true;
    info.TypeId = hit.TypeId;
    info.ChipRect = hit.Rect;
    return info;
}

FReply SLuaTypeBar::OnMouseButtonDown(const Vector2& pos, int button)
{
    if (button != 0)
        return FReply::Unhandled();
    for (const FChipHit& hit : m_ChipHits)
    {
        if (hit.Rect.Contains(pos))
        {
            if (OnTypeClicked)
                OnTypeClicked(hit.TypeId);
            return FReply::Handled();
        }
    }
    return FReply::Unhandled();
}

void SLuaTypeBar::OnMouseMove(const Vector2& pos)
{
    m_HoverChip = -1;
    for (size_t i = 0; i < m_ChipHits.size(); ++i)
    {
        if (m_ChipHits[i].Rect.Contains(pos))
        {
            m_HoverChip = static_cast<int>(i);
            break;
        }
    }
}

void SLuaTypeBar::OnMouseLeave()
{
    m_HoverChip = -1;
}

ECursorType SLuaTypeBar::GetCursor() const
{
    return (m_HoverChip >= 0) ? ECursorType::Hand : ECursorType::Default;
}

// ============================================================================
// SLuaMemTable
// ============================================================================

Vector2 SLuaMemTable::ComputeDesiredSize() const
{
    // Fills whatever the parent gives it; the real extent is virtual.
    return Vector2(600.0f, 200.0f);
}

SLuaMemTable::FLayout SLuaMemTable::ComputeLayout(const UIRect& view) const
{
    FLayout l {};
    l.CountW = 72.0f;
    l.SizeW = 100.0f;
    l.TypeW = 112.0f;
    l.ScrollbarW = kScrollbarWidth;
    l.ScrollbarX = view.x + view.w - l.ScrollbarW;
    l.StackX = view.x + l.CountW + l.SizeW + l.TypeW;
    l.StackW = std::max(40.0f, l.ScrollbarX - l.StackX);
    return l;
}

float SLuaMemTable::TotalContentHeight() const
{
    const size_t n = (View != nullptr) ? View->Size() : 0;
    return static_cast<float>(n) * kRowHeight;
}

void SLuaMemTable::ClampScroll(float content_height) const
{
    const float max_scroll = std::max(0.0f, TotalContentHeight() - content_height);
    m_ScrollOffset = std::max(0.0f, std::min(m_ScrollOffset, max_scroll));
}

void SLuaMemTable::OnPaint(const FPaintContext& ctx, const FGeometry& geom) const
{
    ISlateRenderer* r = ctx.Renderer;
    if (r == nullptr || Snapshot == nullptr || View == nullptr)
        return;

    const UIRect view = geom.ToRect();
    const FLayout l = ComputeLayout(view);
    r->DrawQuad(view, kBgPanel);

    // ---- Header -----------------------------------------------------------
    const UIRect header(view.x, view.y, view.w, kHeaderHeight);
    r->DrawQuad(header, kBgHeader);
    r->DrawQuad(UIRect(view.x, view.y + kHeaderHeight - 1.0f, view.w, 1.0f), kBorder);

    const EColumn sort_col = View->GetSortColumn();
    const bool desc = View->IsSortDescending();

    struct FColDef
    {
        const char* Label;
        EColumn Col;
        float X, W;
        TextAnchor Anchor;
    };
    const FColDef cols[] = {
        {"COUNT", EColumn::Count, view.x,                            l.CountW, TextAnchor::MiddleRight},
        {"SIZE",  EColumn::Size,  view.x + l.CountW,                  l.SizeW,  TextAnchor::MiddleRight},
        {"TYPE",  EColumn::Type,  view.x + l.CountW + l.SizeW,        l.TypeW,  TextAnchor::MiddleLeft},
        {"STACK", EColumn::Stack, l.StackX,                           l.StackW, TextAnchor::MiddleLeft},
    };

    for (const FColDef& c : cols)
    {
        const bool active = (c.Col == sort_col);
        // Arrow is ASCII on purpose: the font atlas falls back per-glyph and
        // triangle glyphs are missing from some of the fonts we may load.
        std::string label = c.Label;
        if (active)
            label += desc ? "  v" : "  ^";

        const float pad_l = (c.Anchor == TextAnchor::MiddleLeft) ? kCellPadX : 0.0f;
        const float pad_r = (c.Anchor == TextAnchor::MiddleRight) ? kCellPadX : 0.0f;
        r->DrawTextLabel(UIRect(c.X + pad_l, view.y, c.W - pad_l - pad_r, kHeaderHeight),
                         label, kHeaderFontSize, active ? kTextMain : kTextDim,
                         c.Anchor, TextWrapMode::NoWrap);
    }

    // ---- Rows -------------------------------------------------------------
    const float content_y = view.y + kHeaderHeight;
    const float content_h = view.h - kHeaderHeight;
    m_ViewportHeight = content_h;
    ClampScroll(content_h);

    const std::vector<uint32_t>& indices = View->GetIndices();
    const std::vector<FRow>& rows = Snapshot->GetRows();

    r->PushClipRect(UIRect(view.x, content_y, view.w - l.ScrollbarW, content_h));

    const int64_t first = static_cast<int64_t>(m_ScrollOffset / kRowHeight);
    const int64_t visible = static_cast<int64_t>(content_h / kRowHeight) + 2;
    const int64_t last = std::min(static_cast<int64_t>(indices.size()), first + visible);

    for (int64_t vi = first; vi < last; ++vi)
    {
        const FRow& row = rows[indices[static_cast<size_t>(vi)]];
        const float y = content_y + static_cast<float>(vi) * kRowHeight - m_ScrollOffset;
        const UIRect row_rect(view.x, y, view.w - l.ScrollbarW, kRowHeight);

        if (vi == m_SelectedViewIndex)
            r->DrawQuad(row_rect, kBgSelected);
        else if (vi == m_HoverViewIndex)
            r->DrawQuad(row_rect, kBgRowHover);
        else if ((vi & 1) != 0)
            r->DrawQuad(row_rect, kBgRowAlt);

        const bool sel = (vi == m_SelectedViewIndex);
        const UIColor main = sel ? UIColor(1, 1, 1, 1) : kTextMain;
        const UIColor dim = sel ? UIColor(0.85f, 0.90f, 1.0f, 1.0f) : kTextDim;

        // COUNT
        r->DrawTextLabel(UIRect(view.x, y, l.CountW - kCellPadX, kRowHeight),
                         FormatCount(row.Count), kRowFontSize, dim,
                         TextAnchor::MiddleRight, TextWrapMode::NoWrap);
        // SIZE
        r->DrawTextLabel(UIRect(view.x + l.CountW, y, l.SizeW - kCellPadX, kRowHeight),
                         FormatBytes(row.Size), kRowFontSize, main,
                         TextAnchor::MiddleRight, TextWrapMode::NoWrap);
        // TYPE (swatch + name), clipped so a long tag cannot bleed into STACK
        {
            const std::string& tname = Snapshot->GetTypeName(row.TypeId);
            const float tx = view.x + l.CountW + l.SizeW + kCellPadX;
            r->PushClipRect(UIRect(tx, y, l.TypeW - kCellPadX, kRowHeight));
            r->DrawQuad(UIRect(tx, y + kRowHeight * 0.5f - 4.0f, 8.0f, 8.0f), ColorForType(tname));
            r->DrawTextLabel(UIRect(tx + 13.0f, y, l.TypeW - kCellPadX - 13.0f, kRowHeight),
                             tname, kRowFontSize, dim, TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
            r->PopClipRect();
        }
        // STACK (first line only; the detail pane shows the rest)
        {
            const std::string_view first_line = Snapshot->GetStackFirstLine(row);
            r->PushClipRect(UIRect(l.StackX, y, l.StackW, kRowHeight));
            r->DrawTextLabel(UIRect(l.StackX + kCellPadX, y, l.StackW - kCellPadX, kRowHeight),
                             std::string(first_line), kRowFontSize, main,
                             TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
            r->PopClipRect();
        }
    }

    r->PopClipRect();

    // ---- Scrollbar --------------------------------------------------------
    const float total_h = TotalContentHeight();
    if (total_h > content_h && content_h > 0.0f)
    {
        const UIRect track(l.ScrollbarX, content_y, l.ScrollbarW, content_h);
        r->DrawQuad(track, UIColor(0.090f, 0.090f, 0.102f, 1.0f));

        const float frac = content_h / total_h;
        const float thumb_h = std::max(24.0f, content_h * frac);
        const float max_scroll = total_h - content_h;
        const float t = (max_scroll > 0.0f) ? (m_ScrollOffset / max_scroll) : 0.0f;
        const float thumb_y = content_y + t * (content_h - thumb_h);
        m_ThumbRect = UIRect(l.ScrollbarX + 1.0f, thumb_y, l.ScrollbarW - 2.0f, thumb_h);
        r->DrawQuad(m_ThumbRect, m_DraggingScrollbar ? kAccent : kScrollThumb);
    }
    else
    {
        m_ThumbRect = UIRect(0, 0, 0, 0);
    }
}

int64_t SLuaMemTable::ViewIndexAtY(float y, const UIRect& view) const
{
    if (View == nullptr)
        return -1;
    const float content_y = view.y + kHeaderHeight;
    if (y < content_y)
        return -1;
    const int64_t vi = static_cast<int64_t>((y - content_y + m_ScrollOffset) / kRowHeight);
    if (vi < 0 || vi >= static_cast<int64_t>(View->Size()))
        return -1;
    return vi;
}

void SLuaMemTable::SelectViewIndex(int64_t vi)
{
    if (View == nullptr)
        return;
    if (vi < 0 || vi >= static_cast<int64_t>(View->Size()))
    {
        m_SelectedViewIndex = -1;
        m_SelectedRow = -1;
    }
    else
    {
        m_SelectedViewIndex = vi;
        m_SelectedRow = static_cast<int64_t>(View->GetIndices()[static_cast<size_t>(vi)]);
    }
    if (OnSelectionChanged)
        OnSelectionChanged(m_SelectedRow);
}

FReply SLuaMemTable::OnMouseButtonDown(const Vector2& pos, int button)
{
    if (button != 0 || View == nullptr || Snapshot == nullptr)
        return FReply::Unhandled();

    const UIRect view = GetCachedGeometry().ToRect();

    // Scrollbar thumb.
    if (m_ThumbRect.w > 0.0f && m_ThumbRect.Contains(pos))
    {
        m_DraggingScrollbar = true;
        m_DragGrabOffset = pos.y - m_ThumbRect.y;
        return FReply::Handled().CaptureMouse(this);
    }

    // Header: toggle direction when the same column is clicked again,
    // otherwise switch column and start from the most useful direction
    // (descending for the numeric columns, ascending for the textual ones).
    if (pos.y < view.y + kHeaderHeight)
    {
        const FLayout l = ComputeLayout(view);
        const float rel = pos.x - view.x;
        EColumn col;
        if (rel < l.CountW)                              col = EColumn::Count;
        else if (rel < l.CountW + l.SizeW)               col = EColumn::Size;
        else if (rel < l.CountW + l.SizeW + l.TypeW)     col = EColumn::Type;
        else                                             col = EColumn::Stack;

        bool new_desc;
        if (col == View->GetSortColumn())
            new_desc = !View->IsSortDescending();
        else
            new_desc = (col == EColumn::Count || col == EColumn::Size);

        View->SetSort(*Snapshot, col, new_desc);

        // The selected row moved; follow it so the detail pane stays in sync.
        if (m_SelectedRow >= 0)
        {
            const std::vector<uint32_t>& idx = View->GetIndices();
            m_SelectedViewIndex = -1;
            for (size_t i = 0; i < idx.size(); ++i)
            {
                if (static_cast<int64_t>(idx[i]) == m_SelectedRow)
                {
                    m_SelectedViewIndex = static_cast<int64_t>(i);
                    break;
                }
            }
        }
        if (OnSortChanged)
            OnSortChanged();
        return FReply::Handled();
    }

    SelectViewIndex(ViewIndexAtY(pos.y, view));
    return FReply::Handled();
}

FReply SLuaMemTable::OnMouseButtonUp(const Vector2& /*pos*/, int button)
{
    if (button == 0 && m_DraggingScrollbar)
    {
        m_DraggingScrollbar = false;
        return FReply::Handled().ReleaseMouseCapture();
    }
    return FReply::Unhandled();
}

void SLuaMemTable::OnMouseMove(const Vector2& pos)
{
    const UIRect view = GetCachedGeometry().ToRect();

    if (m_DraggingScrollbar)
    {
        const float content_y = view.y + kHeaderHeight;
        const float content_h = view.h - kHeaderHeight;
        const float total_h = TotalContentHeight();
        if (total_h > content_h && content_h > 0.0f)
        {
            const float thumb_h = std::max(24.0f, content_h * (content_h / total_h));
            const float span = content_h - thumb_h;
            const float t = (span > 0.0f) ? ((pos.y - m_DragGrabOffset - content_y) / span) : 0.0f;
            m_ScrollOffset = std::max(0.0f, std::min(1.0f, t)) * (total_h - content_h);
        }
        return;
    }

    m_HoverHeader = (pos.y < view.y + kHeaderHeight);
    m_HoverViewIndex = m_HoverHeader ? -1 : ViewIndexAtY(pos.y, view);
}

void SLuaMemTable::OnMouseLeave()
{
    m_HoverViewIndex = -1;
    m_HoverHeader = false;
}

FReply SLuaMemTable::OnMouseWheel(const Vector2& /*pos*/, float delta)
{
    if (View == nullptr || View->Size() == 0)
        return FReply::Unhandled();
    m_ScrollOffset -= delta * kRowHeight * 3.0f;
    ClampScroll(m_ViewportHeight);
    return FReply::Handled();
}

FReply SLuaMemTable::OnKeyDown(EKey key)
{
    if (View == nullptr || View->Size() == 0)
        return FReply::Unhandled();

    const int64_t n = static_cast<int64_t>(View->Size());
    int64_t target = m_SelectedViewIndex;

    switch (key)
    {
    case EKey::Up:   target = (target < 0) ? 0 : std::max<int64_t>(0, target - 1); break;
    case EKey::Down: target = (target < 0) ? 0 : std::min<int64_t>(n - 1, target + 1); break;
    case EKey::Home: target = 0; break;
    case EKey::End:  target = n - 1; break;
    default: return FReply::Unhandled();
    }

    SelectViewIndex(target);

    // Keep the selection inside the viewport.
    const float row_top = static_cast<float>(target) * kRowHeight;
    if (row_top < m_ScrollOffset)
        m_ScrollOffset = row_top;
    else if (row_top + kRowHeight > m_ScrollOffset + m_ViewportHeight)
        m_ScrollOffset = row_top + kRowHeight - m_ViewportHeight;
    ClampScroll(m_ViewportHeight);
    return FReply::Handled();
}

ECursorType SLuaMemTable::GetCursor() const
{
    return m_HoverHeader ? ECursorType::Hand : ECursorType::Default;
}

void SLuaMemTable::ScrollByPage(int direction)
{
    m_ScrollOffset += static_cast<float>(direction) * std::max(kRowHeight, m_ViewportHeight - kRowHeight);
    ClampScroll(m_ViewportHeight);
}

void SLuaMemTable::ResetView()
{
    m_ScrollOffset = 0.0f;
    m_SelectedViewIndex = -1;
    m_SelectedRow = -1;
    m_HoverViewIndex = -1;
    if (OnSelectionChanged)
        OnSelectionChanged(-1);
}

// ============================================================================
// SLuaStackDetail
// ============================================================================

void SLuaStackDetail::SetRow(int64_t row_index)
{
    m_Row = row_index;
    m_Header.clear();
    m_Lines.clear();
    m_ScrollOffset = 0.0f;

    if (Snapshot == nullptr || !Snapshot->IsLoaded() || row_index < 0 ||
        row_index >= static_cast<int64_t>(Snapshot->GetRowCount()))
        return;

    const FRow& row = Snapshot->GetRows()[static_cast<size_t>(row_index)];
    m_Header = Snapshot->GetTypeName(row.TypeId) + "   " + FormatBytes(row.Size) +
               "   " + FormatCount(row.Count) + " objects";

    const std::string_view stack = Snapshot->GetStack(row);
    size_t start = 0;
    while (start <= stack.size())
    {
        const size_t nl = stack.find('\n', start);
        const size_t end = (nl == std::string_view::npos) ? stack.size() : nl;
        m_Lines.emplace_back(stack.substr(start, end - start));
        if (nl == std::string_view::npos)
            break;
        start = nl + 1;
    }
    if (m_Lines.size() == 1 && m_Lines[0].empty())
        m_Lines[0] = "(no stack recorded - allocated before the profiler was registered)";
}

Vector2 SLuaStackDetail::ComputeDesiredSize() const
{
    return Vector2(400.0f, 150.0f);
}

void SLuaStackDetail::OnPaint(const FPaintContext& ctx, const FGeometry& geom) const
{
    ISlateRenderer* r = ctx.Renderer;
    if (r == nullptr)
        return;

    const UIRect view = geom.ToRect();
    r->DrawQuad(view, UIColor(0.090f, 0.090f, 0.102f, 1.0f));
    r->DrawQuad(UIRect(view.x, view.y, view.w, 1.0f), kBorder);

    if (m_Row < 0)
    {
        r->DrawTextLabel(view, "Select a row to inspect its allocation stack", 12.0f, kTextDim,
                         TextAnchor::MiddleCenter, TextWrapMode::NoWrap);
        return;
    }

    const float line_h = 17.0f;
    float y = view.y + 8.0f;

    r->DrawTextLabel(UIRect(view.x + kCellPadX, y, view.w - 2 * kCellPadX, line_h),
                     m_Header, 13.0f, kAccent, TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
    y += line_h + 4.0f;

    r->PushClipRect(UIRect(view.x, y, view.w, view.y + view.h - y));
    float ly = y - m_ScrollOffset;
    for (const std::string& line : m_Lines)
    {
        if (ly + line_h >= y && ly <= view.y + view.h)
        {
            r->DrawTextLabel(UIRect(view.x + kCellPadX, ly, view.w - 2 * kCellPadX, line_h),
                             line, 12.0f, kTextMain, TextAnchor::MiddleLeft, TextWrapMode::NoWrap);
        }
        ly += line_h;
    }
    r->PopClipRect();

    // Clamp here: the viewport height is only known during paint.
    const float content_h = static_cast<float>(m_Lines.size()) * line_h;
    const float avail = view.y + view.h - y;
    m_ScrollOffset = std::max(0.0f, std::min(m_ScrollOffset, std::max(0.0f, content_h - avail)));
}

FReply SLuaStackDetail::OnMouseWheel(const Vector2& /*pos*/, float delta)
{
    if (m_Row < 0)
        return FReply::Unhandled();
    m_ScrollOffset = std::max(0.0f, m_ScrollOffset - delta * 17.0f * 3.0f);
    return FReply::Handled();
}

}  // namespace LuaMemPreview
