#pragma once

// ============================================================================
// ZSlate/Widgets/Views/SListView.h
//
// Single-column virtualised list.
//
//   auto ds   = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>();
//   auto list = std::make_shared<ZSlate::SListView<std::string>>();
//   list->SetDataSource(ds);
//   list->Options.ItemHeight = 26.0f;
//   list->SetOnGenerateItemWidget([](const std::string& item, int32_t index,
//                                    const auto& lv) -> std::shared_ptr<ZSlate::SWidget> {
//       auto label = std::make_shared<ZSlate::STextBlock>();
//       label->Text = item;
//       return label;
//   });
//
// There is no explicit refresh call: replacing the data source contents with
// SetItems() bumps a revision counter and the list regenerates its rows on the
// next paint.
// ============================================================================

#include "../SWidget.h"
#include "../Text/STextBlock.h"
#include "../../Core/ZSlateTypes.h"
#include "../../Core/SlateTextMeasurer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZSlate
{

// ---------------------------------------------------------------------------
// Data source
// ---------------------------------------------------------------------------
template<class T>
class TSimpleListViewDataSource
{
public:
    TSimpleListViewDataSource() = default;
    explicit TSimpleListViewDataSource(std::vector<T> items) : m_Items(std::move(items)) {}

    void SetItems(std::vector<T> items) { m_Items = std::move(items); ++m_Revision; }
    std::vector<T> GetItems() const { return m_Items; }
    const std::vector<T>& GetItemsRef() const { return m_Items; }

    // Bumped on every mutation; the list view uses it to detect staleness.
    uint64_t GetRevision() const { return m_Revision; }
    void MarkDirty() { ++m_Revision; }

private:
    std::vector<T> m_Items;
    uint64_t       m_Revision = 1;
};

// ---------------------------------------------------------------------------
// SListView
// ---------------------------------------------------------------------------
template<class T>
class SListView : public SWidget
{
public:
    struct FOptions
    {
        float   ItemHeight               = 26.0f;
        UIColor SelectedBackgroundColor  { 0.25f, 0.50f, 0.90f, 0.30f };
        UIColor BackgroundColor          { 0.10f, 0.11f, 0.13f, 1.0f };
        UIColor HoverBackgroundColor     { 0.17f, 0.19f, 0.23f, 1.0f };
        UIColor BorderColor              { 0.25f, 0.27f, 0.32f, 1.0f };
    };

    FOptions Options;

    using FGenerateRowCallback =
        std::function<std::shared_ptr<SWidget>(const T& item,
                                               int32_t index,
                                               const std::shared_ptr<SListView<T>>& listView)>;

    // ---- Data --------------------------------------------------------------
    void SetDataSource(const std::shared_ptr<TSimpleListViewDataSource<T>>& dataSource)
    {
        m_DataSource  = dataSource;
        m_LastRevision = 0;
    }

    std::shared_ptr<TSimpleListViewDataSource<T>> GetDataSource() const { return m_DataSource; }

    // ---- Callbacks ---------------------------------------------------------
    template<class F>
    void SetOnGenerateItemWidget(F&& callback)
    {
        m_OnGenerateItemWidget = std::forward<F>(callback);
        m_LastRevision = 0;   // force regeneration with the new factory
    }

    void SetOnSelectionChanged(std::function<void(const std::vector<int32_t>&)> callback)
    {
        m_OnSelectionChanged = std::move(callback);
    }

    void SetOnItemRightClicked(std::function<void(int32_t index, const Vector2& position)> callback)
    {
        m_OnItemRightClicked = std::move(callback);
    }

    void SetOnItemDoubleClicked(std::function<void(int32_t index)> callback)
    {
        m_OnItemDoubleClicked = std::move(callback);
    }

    // ---- Selection ---------------------------------------------------------
    void SetSelection(int32_t index, bool selected)
    {
        auto it = std::find(m_SelectedIndices.begin(), m_SelectedIndices.end(), index);
        if (selected)
        {
            if (it == m_SelectedIndices.end()) m_SelectedIndices.push_back(index);
        }
        else if (it != m_SelectedIndices.end())
        {
            m_SelectedIndices.erase(it);
        }
    }

    void ClearSelection() { m_SelectedIndices.clear(); }

    const std::vector<int32_t>& GetSelectedIndices() const { return m_SelectedIndices; }

    int32_t GetSelectedIndex() const
    {
        return m_SelectedIndices.empty() ? -1 : m_SelectedIndices.front();
    }

    bool IsIndexSelected(int32_t index) const
    {
        return std::find(m_SelectedIndices.begin(), m_SelectedIndices.end(), index)
            != m_SelectedIndices.end();
    }

    // ---- Widget overrides --------------------------------------------------
    bool WantsFillHeight() const override { return true; }

    Vector2 ComputeDesiredSize() const override
    {
        const int32_t count = GetItemCount();
        return Vector2(0.0f, 2.0f + Options.ItemHeight * static_cast<float>(std::max(0, count)));
    }

    void OnPaint(const FPaintContext& context, const FGeometry& geometry) const override
    {
        if (!context.Renderer) return;

        const UIRect r = geometry.ToRect();
        context.Renderer->DrawQuad(r, Options.BackgroundColor);
        context.Renderer->DrawRect(r, Options.BorderColor, 1.0f);

        if (!m_DataSource) return;

        const int32_t count = GetItemCount();
        if (count == 0) return;

        const float itemH   = Options.ItemHeight;
        const float viewTop = r.y + 1.0f;
        const float viewBot = r.Bottom() - 1.0f;
        const float viewH   = std::max(0.0f, viewBot - viewTop);

        ClampScroll(viewH, itemH * static_cast<float>(count));

        const int32_t first = std::max(0, static_cast<int32_t>(std::floor(m_ScrollOffset / itemH)));
        const int32_t last  = std::min(count,
            first + static_cast<int32_t>(std::ceil(viewH / itemH)) + 2);

        // Virtualisation: only the rows inside the window exist. Rows are built
        // on demand and dropped once they scroll out, so cost is bounded by the
        // number of visible rows instead of by Items.size().
        SyncRowCache(first, last);

        for (int32_t i = first; i < last; ++i)
        {
            const float y = viewTop + static_cast<float>(i) * itemH - m_ScrollOffset;
            const UIRect rowRect(r.x + 1.0f, y, std::max(0.0f, r.w - 2.0f), itemH);

            if (IsIndexSelected(i))
                context.Renderer->DrawQuad(rowRect, Options.SelectedBackgroundColor);
            else if (i == m_HoverIndex)
                context.Renderer->DrawQuad(rowRect, Options.HoverBackgroundColor);

            if (auto row = GetRowWidget(i))
            {
                PaintWidget(row, context,
                            FGeometry(Vector2(rowRect.x, rowRect.y),
                                      Vector2(rowRect.w, rowRect.h)));
            }
        }
    }

    FReply OnMouseMove(const Vector2& absolutePos) override
    {
        m_HoverIndex = HitTestIndex(absolutePos);
        return FReply::Unhandled();
    }

    FReply OnMouseButtonDown(const Vector2& absolutePos, int button) override
    {
        const int32_t index = HitTestIndex(absolutePos);
        if (index < 0) return FReply::Unhandled();

        if (button == 1)   // right
        {
            if (m_OnItemRightClicked) m_OnItemRightClicked(index, absolutePos);
            return FReply::Handled();
        }

        if (button == 0)   // left
        {
            const bool alreadySelected = IsIndexSelected(index);
            m_SelectedIndices.clear();
            m_SelectedIndices.push_back(index);
            if (m_OnSelectionChanged) m_OnSelectionChanged(m_SelectedIndices);
            (void)alreadySelected;
            return FReply::Handled();
        }

        return FReply::Unhandled();
    }

    FReply OnMouseButtonUp(const Vector2& absolutePos, int button) override
    {
        (void)absolutePos; (void)button;
        return FReply::Handled();
    }

    FReply OnMouseButtonDoubleClick(const Vector2& absolutePos, int button) override
    {
        const int32_t index = HitTestIndex(absolutePos);
        if (index < 0 || button != 0) return FReply::Unhandled();

        m_SelectedIndices.clear();
        m_SelectedIndices.push_back(index);
        if (m_OnSelectionChanged) m_OnSelectionChanged(m_SelectedIndices);

        if (m_OnItemDoubleClicked) m_OnItemDoubleClicked(index);
        return FReply::Handled();
    }

    FReply OnMouseWheel(const Vector2& absolutePos, float delta) override
    {
        (void)absolutePos;
        m_ScrollOffset -= delta * Options.ItemHeight;
        const int32_t count = GetItemCount();
        const float   viewH = std::max(0.0f, GetCachedGeometry().LocalSize.y - 2.0f);
        ClampScroll(viewH, Options.ItemHeight * static_cast<float>(std::max(0, count)));
        return FReply::Handled();
    }

private:
    int32_t GetItemCount() const
    {
        return m_DataSource ? static_cast<int32_t>(m_DataSource->GetItemsRef().size()) : 0;
    }

    int32_t HitTestIndex(const Vector2& absolutePos) const
    {
        const UIRect r = GetCachedGeometry().ToRect();
        if (!r.Contains(absolutePos)) return -1;
        const float rel = absolutePos.y - (r.y + 1.0f) + m_ScrollOffset;
        if (rel < 0.0f) return -1;
        const int32_t index = static_cast<int32_t>(rel / Options.ItemHeight);
        return (index >= 0 && index < GetItemCount()) ? index : -1;
    }

    void ClampScroll(float viewHeight, float contentHeight) const
    {
        const float maxScroll = std::max(0.0f, contentHeight - viewHeight);
        if (m_ScrollOffset < 0.0f)       m_ScrollOffset = 0.0f;
        if (m_ScrollOffset > maxScroll)  m_ScrollOffset = maxScroll;
    }

    // Drop every cached row. Called when the data source (or the row factory)
    // changes, so rows are rebuilt from the new data on the next paint.
    void InvalidateRows() const
    {
        m_RowCache.clear();
        m_LastRevision = 0;
    }

    std::shared_ptr<SWidget> GetRowWidget(int32_t index) const
    {
        const auto it = m_RowCache.find(index);
        return (it == m_RowCache.end()) ? nullptr : it->second;
    }

    std::shared_ptr<SWidget> BuildRowWidget(int32_t index) const
    {
        if (!m_DataSource || index < 0 || index >= GetItemCount()) return nullptr;

        const std::vector<T>& items = m_DataSource->GetItemsRef();

        std::shared_ptr<SWidget> row;
        if (m_OnGenerateItemWidget)
        {
            auto self = std::static_pointer_cast<SListView<T>>(
                const_cast<SListView<T>*>(this)->shared_from_this());
            row = m_OnGenerateItemWidget(items[static_cast<size_t>(index)], index, self);
        }
        if (!row)
        {
            // Fallback: plain label, so a list is never blank.
            auto label = std::make_shared<STextBlock>();
            label->Text = ToDisplayString(items[static_cast<size_t>(index)]);
            label->FontSize = 14.0f;
            label->Color = Colors::White;
            row = label;
        }

        row->SetParent(const_cast<SListView<T>*>(this));
        m_RowCache[index] = row;
        return row;
    }

    // Keep the cache limited to the visible window (plus the two rows of slack
    // the paint pass asks for), dropping anything that scrolled away.
    void SyncRowCache(int32_t first, int32_t last) const
    {
        if (!m_DataSource) return;

        // A bumped revision means the backing data changed: start over.
        const uint64_t revision = m_DataSource->GetRevision();
        if (revision != m_LastRevision)
        {
            m_RowCache.clear();
            m_LastRevision = revision;
        }

        for (auto it = m_RowCache.begin(); it != m_RowCache.end();)
        {
            it = (it->first < first || it->first >= last) ? m_RowCache.erase(it) : std::next(it);
        }

        for (int32_t i = first; i < last; ++i)
        {
            if (m_RowCache.find(i) == m_RowCache.end()) BuildRowWidget(i);
        }
    }

    static std::string ToDisplayString(const T& value)
    {
        if constexpr (std::is_convertible_v<T, std::string>) return std::string(value);
        else return std::string("[item]");
    }

private:
    std::shared_ptr<TSimpleListViewDataSource<T>> m_DataSource;

    FGenerateRowCallback m_OnGenerateItemWidget;
    std::function<void(const std::vector<int32_t>&)> m_OnSelectionChanged;
    std::function<void(int32_t, const Vector2&)>     m_OnItemRightClicked;
    std::function<void(int32_t)>                     m_OnItemDoubleClicked;

    // Only the rows inside the visible window are materialised.
    mutable std::unordered_map<int32_t, std::shared_ptr<SWidget>> m_RowCache;
    mutable uint64_t m_LastRevision = 0;
    mutable float    m_ScrollOffset = 0.0f;
    int32_t          m_HoverIndex   = -1;
    std::vector<int32_t> m_SelectedIndices;
};

} // namespace ZSlate
