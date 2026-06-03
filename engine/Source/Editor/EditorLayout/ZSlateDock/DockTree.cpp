#include "Editor/EditorLayout/ZSlateDock/DockTree.h"

#include "Editor/EditorLayout/EditorLayoutWindowIds.h"

#include <algorithm>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace EditorDock
{
    namespace
    {
        bool RectContains(const UIRect& r, const Vector2& p)
        {
            return p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height;
        }

        std::unique_ptr<DockNode> MakeLeaf(const std::string& panel_id)
        {
            auto leaf = std::make_unique<DockNode>();
            if (!panel_id.empty())
            {
                leaf->Tabs.push_back(DockTab {panel_id});
            }
            return leaf;
        }
    }  // namespace

    DockTree::DockTree()
        : m_Root(std::make_unique<DockNode>()) {}

    void DockTree::Clear()
    {
        m_Root = std::make_unique<DockNode>();
        m_Dirty = true;
    }

    void DockTree::ResetToSingleLeaf(const std::vector<std::string>& panel_ids)
    {
        m_Root = std::make_unique<DockNode>();
        for (const std::string& id : panel_ids)
        {
            m_Root->Tabs.push_back(DockTab {id});
        }
        m_Root->ActiveTab = 0;
        m_Dirty = true;
    }

    // -------------------------------------------------------------------------
    // Geometry
    // -------------------------------------------------------------------------

    void DockTree::ComputeGeometry(const UIRect& rect)
    {
        if (m_Root != nullptr)
        {
            ComputeNode(*m_Root, rect);
        }
    }

    void DockTree::ComputeNode(DockNode& node, const UIRect& rect)
    {
        node.NodeRect = rect;

        if (node.IsLeaf())
        {
            const float strip_h = node.Tabs.empty() ? 0.0f : std::min(m_Metrics.TabStripHeight, rect.height);
            node.TabStripRect = UIRect(rect.x, rect.y, rect.width, strip_h);
            node.ContentRect = UIRect(rect.x, rect.y + strip_h, rect.width, std::max(0.0f, rect.height - strip_h));
            node.SplitterRect = UIRect(0.0f, 0.0f, 0.0f, 0.0f);
            return;
        }

        node.TabStripRect = UIRect(0.0f, 0.0f, 0.0f, 0.0f);
        node.ContentRect = rect;

        const float thickness = m_Metrics.SplitterThickness;
        const float ratio = std::clamp(node.SplitRatio, 0.01f, 0.99f);

        if (node.Orientation == EOrientation::Horizontal)
        {
            const float usable = std::max(0.0f, rect.width - thickness);
            const float a_w = usable * ratio;
            const float b_w = usable - a_w;
            const UIRect a_rect(rect.x, rect.y, a_w, rect.height);
            const UIRect b_rect(rect.x + a_w + thickness, rect.y, b_w, rect.height);
            node.SplitterRect = UIRect(rect.x + a_w, rect.y, thickness, rect.height);
            if (node.ChildA)
                ComputeNode(*node.ChildA, a_rect);
            if (node.ChildB)
                ComputeNode(*node.ChildB, b_rect);
        }
        else
        {
            const float usable = std::max(0.0f, rect.height - thickness);
            const float a_h = usable * ratio;
            const float b_h = usable - a_h;
            const UIRect a_rect(rect.x, rect.y, rect.width, a_h);
            const UIRect b_rect(rect.x, rect.y + a_h + thickness, rect.width, b_h);
            node.SplitterRect = UIRect(rect.x, rect.y + a_h, rect.width, thickness);
            if (node.ChildA)
                ComputeNode(*node.ChildA, a_rect);
            if (node.ChildB)
                ComputeNode(*node.ChildB, b_rect);
        }
    }

    // -------------------------------------------------------------------------
    // Tree walks
    // -------------------------------------------------------------------------

    void DockTree::ForEachLeaf(const std::function<void(DockNode&)>& fn)
    {
        ForEachNode([&fn](DockNode& n) {
            if (n.IsLeaf())
                fn(n);
        });
    }

    void DockTree::ForEachNode(const std::function<void(DockNode&)>& fn)
    {
        if (m_Root == nullptr)
            return;

        // Iterative depth-first to avoid an extra recursive helper.
        std::vector<DockNode*> stack {m_Root.get()};
        while (!stack.empty())
        {
            DockNode* n = stack.back();
            stack.pop_back();
            fn(*n);
            if (n->ChildA)
                stack.push_back(n->ChildA.get());
            if (n->ChildB)
                stack.push_back(n->ChildB.get());
        }
    }

    DockNode* DockTree::FindPanelLeaf(const std::string& panel_id)
    {
        DockNode* found = nullptr;
        ForEachLeaf([&](DockNode& leaf) {
            if (found != nullptr)
                return;
            for (const DockTab& tab : leaf.Tabs)
            {
                if (tab.PanelId == panel_id)
                {
                    found = &leaf;
                    return;
                }
            }
        });
        return found;
    }

    DockNode* DockTree::LeafAt(const Vector2& p)
    {
        DockNode* found = nullptr;
        ForEachLeaf([&](DockNode& leaf) {
            if (found == nullptr && RectContains(leaf.NodeRect, p))
                found = &leaf;
        });
        return found;
    }

    DockNode* DockTree::HitTestSplitter(const Vector2& p)
    {
        DockNode* found = nullptr;
        ForEachNode([&](DockNode& n) {
            if (found == nullptr && !n.IsLeaf() && RectContains(n.SplitterRect, p))
                found = &n;
        });
        return found;
    }

    // -------------------------------------------------------------------------
    // Structural edits
    // -------------------------------------------------------------------------

    int DockTree::AddTab(DockNode& leaf, const std::string& panel_id)
    {
        for (size_t i = 0; i < leaf.Tabs.size(); ++i)
        {
            if (leaf.Tabs[i].PanelId == panel_id)
            {
                if (leaf.ActiveTab != static_cast<int>(i))
                {
                    leaf.ActiveTab = static_cast<int>(i);
                    m_Dirty = true;
                }
                return leaf.ActiveTab;
            }
        }
        leaf.Tabs.push_back(DockTab {panel_id});
        leaf.ActiveTab = static_cast<int>(leaf.Tabs.size()) - 1;
        m_Dirty = true;
        return leaf.ActiveTab;
    }

    DockNode* DockTree::SplitLeaf(DockNode& leaf, EDockDir dir, const std::string& panel_id, float ratio)
    {
        if (!leaf.IsLeaf())
            return nullptr;

        if (dir == EDockDir::Center)
        {
            AddTab(leaf, panel_id);
            return &leaf;
        }

        // Preserve the existing tabs on one side; the new panel goes to the other.
        auto child_old = std::make_unique<DockNode>();
        child_old->Tabs = std::move(leaf.Tabs);
        child_old->ActiveTab = std::min(leaf.ActiveTab, static_cast<int>(child_old->Tabs.size()) - 1);
        if (child_old->ActiveTab < 0)
            child_old->ActiveTab = 0;

        auto child_new = MakeLeaf(panel_id);
        DockNode* new_leaf = child_new.get();

        // Convert `leaf` in place into a split (its object identity / parent slot stays).
        leaf.Tabs.clear();
        leaf.ActiveTab = 0;

        const float r = std::clamp(ratio, 0.05f, 0.95f);
        switch (dir)
        {
            case EDockDir::Left:
                leaf.Orientation = EOrientation::Horizontal;
                leaf.ChildA = std::move(child_new);
                leaf.ChildB = std::move(child_old);
                leaf.SplitRatio = r;  // ChildA (new) gets `r`
                break;
            case EDockDir::Right:
                leaf.Orientation = EOrientation::Horizontal;
                leaf.ChildA = std::move(child_old);
                leaf.ChildB = std::move(child_new);
                leaf.SplitRatio = 1.0f - r;  // ChildB (new) gets `r`
                break;
            case EDockDir::Top:
                leaf.Orientation = EOrientation::Vertical;
                leaf.ChildA = std::move(child_new);
                leaf.ChildB = std::move(child_old);
                leaf.SplitRatio = r;
                break;
            case EDockDir::Bottom:
                leaf.Orientation = EOrientation::Vertical;
                leaf.ChildA = std::move(child_old);
                leaf.ChildB = std::move(child_new);
                leaf.SplitRatio = 1.0f - r;
                break;
            case EDockDir::Center:
            default:
                break;
        }
        m_Dirty = true;
        return new_leaf;
    }

    DockNode* DockTree::MovePanel(const std::string& panel_id, DockNode& target, EDockDir dir, float ratio)
    {
        // Capture target identity before the remove may collapse/rearrange the tree.
        // We re-locate by content after removal to keep the pointer valid.
        const bool dropping_onto_self =
            (FindPanelLeaf(panel_id) == &target) && target.IsLeaf() && target.Tabs.size() == 1;
        if (dropping_onto_self)
            return &target;  // no-op: the only panel dropped onto its own leaf

        // Remember a stable panel id that lives in the target leaf so we can find it
        // again after RemovePanel potentially mutates the tree topology.
        std::string target_anchor;
        if (target.IsLeaf() && !target.Tabs.empty())
            target_anchor = target.Tabs.front().PanelId;

        RemovePanel(panel_id);

        DockNode* resolved_target = &target;
        if (!target_anchor.empty())
        {
            if (DockNode* leaf = FindPanelLeaf(target_anchor))
                resolved_target = leaf;
        }
        if (resolved_target == nullptr || !resolved_target->IsLeaf())
            resolved_target = m_Root.get();

        return SplitLeaf(*resolved_target, dir, panel_id, ratio);
    }

    bool DockTree::RemovePanel(const std::string& panel_id)
    {
        if (m_Root == nullptr)
            return false;
        const bool changed = RemoveFromOwner(m_Root, panel_id);
        if (changed)
            m_Dirty = true;
        return changed;
    }

    bool DockTree::RemoveFromOwner(std::unique_ptr<DockNode>& owner, const std::string& panel_id)
    {
        if (owner == nullptr)
            return false;

        DockNode& node = *owner;
        if (node.IsLeaf())
        {
            for (size_t i = 0; i < node.Tabs.size(); ++i)
            {
                if (node.Tabs[i].PanelId == panel_id)
                {
                    node.Tabs.erase(node.Tabs.begin() + static_cast<long>(i));
                    if (node.ActiveTab >= static_cast<int>(node.Tabs.size()))
                        node.ActiveTab = std::max(0, static_cast<int>(node.Tabs.size()) - 1);
                    return true;
                }
            }
            return false;
        }

        // Split node: recurse, then collapse if a direct child emptied.
        if (RemoveFromOwner(node.ChildA, panel_id))
        {
            if (node.ChildA && node.ChildA->IsEmptyLeaf())
                owner = std::move(node.ChildB);  // promote the surviving sibling
            return true;
        }
        if (RemoveFromOwner(node.ChildB, panel_id))
        {
            if (node.ChildB && node.ChildB->IsEmptyLeaf())
                owner = std::move(node.ChildA);
            return true;
        }
        return false;
    }

    void DockTree::SetSplitRatioFromDrag(DockNode& split_node, float new_ratio)
    {
        if (split_node.IsLeaf())
            return;

        const float thickness = m_Metrics.SplitterThickness;
        const float min_extent = m_Metrics.MinNodeExtent;
        const float total = (split_node.Orientation == EOrientation::Horizontal)
                                ? std::max(1.0f, split_node.NodeRect.width - thickness)
                                : std::max(1.0f, split_node.NodeRect.height - thickness);

        const float min_ratio = (total > 0.0f) ? std::clamp(min_extent / total, 0.01f, 0.49f) : 0.05f;
        const float clamped = std::clamp(new_ratio, min_ratio, 1.0f - min_ratio);
        if (clamped != split_node.SplitRatio)
        {
            split_node.SplitRatio = clamped;
            m_Dirty = true;
        }
    }

    std::vector<std::string> DockTree::CollectPanelIds() const
    {
        std::vector<std::string> ids;
        std::vector<const DockNode*> stack;
        if (m_Root)
            stack.push_back(m_Root.get());
        while (!stack.empty())
        {
            const DockNode* n = stack.back();
            stack.pop_back();
            if (n->IsLeaf())
            {
                for (const DockTab& tab : n->Tabs)
                    ids.push_back(tab.PanelId);
            }
            else
            {
                if (n->ChildA)
                    stack.push_back(n->ChildA.get());
                if (n->ChildB)
                    stack.push_back(n->ChildB.get());
            }
        }
        return ids;
    }

    bool DockTree::HasPanel(const std::string& panel_id) const
    {
        const std::vector<std::string> ids = CollectPanelIds();
        return std::find(ids.begin(), ids.end(), panel_id) != ids.end();
    }

    // -------------------------------------------------------------------------
    // Persistence
    // -------------------------------------------------------------------------

    namespace
    {
        void WriteNode(const DockNode& node, rapidjson::Value& out, rapidjson::Document::AllocatorType& alloc)
        {
            out.SetObject();
            if (node.IsLeaf())
            {
                out.AddMember("type", "leaf", alloc);
                out.AddMember("active", node.ActiveTab, alloc);
                rapidjson::Value tabs(rapidjson::kArrayType);
                for (const DockTab& tab : node.Tabs)
                {
                    rapidjson::Value id;
                    id.SetString(tab.PanelId.c_str(), static_cast<rapidjson::SizeType>(tab.PanelId.size()), alloc);
                    tabs.PushBack(id, alloc);
                }
                out.AddMember("tabs", tabs, alloc);
                return;
            }

            out.AddMember("type", "split", alloc);
            out.AddMember("orientation",
                          node.Orientation == EOrientation::Horizontal ? "h" : "v",
                          alloc);
            out.AddMember("ratio", node.SplitRatio, alloc);
            rapidjson::Value a(rapidjson::kObjectType);
            rapidjson::Value b(rapidjson::kObjectType);
            if (node.ChildA)
                WriteNode(*node.ChildA, a, alloc);
            if (node.ChildB)
                WriteNode(*node.ChildB, b, alloc);
            out.AddMember("a", a, alloc);
            out.AddMember("b", b, alloc);
        }

        std::unique_ptr<DockNode> ReadNode(const rapidjson::Value& in)
        {
            if (!in.IsObject() || !in.HasMember("type") || !in["type"].IsString())
                return nullptr;

            auto node = std::make_unique<DockNode>();
            const std::string type = in["type"].GetString();
            if (type == "leaf")
            {
                if (in.HasMember("active") && in["active"].IsInt())
                    node->ActiveTab = in["active"].GetInt();
                if (in.HasMember("tabs") && in["tabs"].IsArray())
                {
                    for (const auto& t : in["tabs"].GetArray())
                    {
                        if (t.IsString())
                        {
                            node->Tabs.push_back(
                                DockTab {EditorLayoutWindowIds::RemapLegacyPanelTitle(t.GetString())});
                        }
                    }
                }
                if (node->ActiveTab >= static_cast<int>(node->Tabs.size()))
                    node->ActiveTab = std::max(0, static_cast<int>(node->Tabs.size()) - 1);
                return node;
            }

            if (type == "split")
            {
                node->Orientation = (in.HasMember("orientation") && in["orientation"].IsString() &&
                                     std::string(in["orientation"].GetString()) == "v")
                                        ? EOrientation::Vertical
                                        : EOrientation::Horizontal;
                if (in.HasMember("ratio") && in["ratio"].IsNumber())
                    node->SplitRatio = std::clamp(static_cast<float>(in["ratio"].GetDouble()), 0.01f, 0.99f);
                if (in.HasMember("a"))
                    node->ChildA = ReadNode(in["a"]);
                if (in.HasMember("b"))
                    node->ChildB = ReadNode(in["b"]);
                // A split with a missing child is invalid; degrade to the surviving side.
                if (!node->ChildA && node->ChildB)
                    return std::move(node->ChildB);
                if (!node->ChildB && node->ChildA)
                    return std::move(node->ChildA);
                if (!node->ChildA && !node->ChildB)
                    return std::make_unique<DockNode>();  // empty leaf
                return node;
            }

            return nullptr;
        }
    }  // namespace

    std::string DockTree::SerializeToJson() const
    {
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();
        doc.AddMember("version", 1, alloc);
        rapidjson::Value root(rapidjson::kObjectType);
        if (m_Root)
            WriteNode(*m_Root, root, alloc);
        doc.AddMember("root", root, alloc);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);
        return std::string(buffer.GetString(), buffer.GetSize());
    }

    bool DockTree::DeserializeFromJson(const std::string& json)
    {
        rapidjson::Document doc;
        doc.Parse(json.c_str(), json.size());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("root"))
            return false;

        std::unique_ptr<DockNode> root = ReadNode(doc["root"]);
        if (root == nullptr)
            return false;

        m_Root = std::move(root);
        // The in-memory tree changed; mark dirty so a host that wants to re-persist (e.g.
        // applying a user layout) sees it. A host restoring its OWN session file should
        // ConsumeDirty() right after to suppress the redundant write-back.
        m_Dirty = true;
        return true;
    }
}  // namespace EditorDock
