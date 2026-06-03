#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <cstdint>
#include <string>
#include <vector>

// On-disk representation of a UMG widget tree (UE WidgetBlueprint / UWidgetTree
// analogue). Persisted as a binary `.zasset` through the standard SerializedFile
// pipeline (AssetManager::saveAsset / loadAsset).
//
// Design (see plan + AGENTS.md 2.3): rather than serialising each UWidget as its
// own Object with file-local PPtrs (the heavyweight Prefab model), the whole
// tree is flattened into an array of nodes. Each node carries its widget class
// name, parent index, designer name and a typed property bag flattened to three
// parallel string vectors (key / type-char / value). This keeps the format
// trivially serialisable with the existing SerializeTraits specialisations
// (std::vector<eastl::string>, eastl::string, int32) and avoids per-widget
// Object lifetime. UMGWidgetSerializer converts live UWidget trees <-> this node
// array.
struct FUMGWidgetNode
{
    DECLARE_SERIALIZE(FUMGWidgetNode)

    eastl::string m_ClassName;          // matches UWidget::GetWidgetClassName()
    eastl::string m_Name;               // designer name (UWidget::Name)
    int32_t m_ParentIndex {-1};         // index into UWidgetAsset::m_Nodes, -1 = root
    std::vector<eastl::string> m_PropKeys;
    std::vector<eastl::string> m_PropTypes;   // single char per UMGProperties EPropType
    std::vector<eastl::string> m_PropValues;
};

template<typename TransferFunction>
void FUMGWidgetNode::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_ClassName, "class_name");
    transfer.Transfer(m_Name, "name");
    transfer.Transfer(m_ParentIndex, "parent_index");
    transfer.Transfer(m_PropKeys, "prop_keys");
    transfer.Transfer(m_PropTypes, "prop_types");
    transfer.Transfer(m_PropValues, "prop_values");
}

class UWidgetAsset : public Object
{
    REGISTER_CLASS(UWidgetAsset);
    DECLARE_OBJECT_SERIALIZE();

public:
    UWidgetAsset() = default;

    static constexpr uint32_t kCurrentSchemaVersion = 1u;

    std::vector<FUMGWidgetNode> m_Nodes;
    uint32_t m_SchemaVersion {kCurrentSchemaVersion};
};
