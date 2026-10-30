#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"

#include <algorithm>

// =============================================================================
// Default tables.
//
// These are what a fresh Unity project ships with, so a ZEngine project opened
// by a Unity user looks familiar and, more practically, so `NameToLayer("UI")`
// works before anyone has opened the settings window.
// =============================================================================

namespace
{
    // 0..5 locked (Unity built-ins), 6/7 reserved, 8..31 user.
    struct BuiltinLayer
    {
        int layer;
        const char* name;
    };

    const BuiltinLayer k_BuiltinLayers[] = {
        {0, "Default"},
        {1, "TransparentFX"},
        {2, "Ignore Raycast"},
        // Layer 3 has no name in Unity either -- it is the retired one. Leaving
        // it empty is deliberate: naming it would invite someone to use it.
        {4, "Water"},
        {5, "UI"},
    };
}  // namespace

const eastl::vector<eastl::string>& TagLayerManager::BuiltinTags()
{
    static const eastl::vector<eastl::string> s_Tags = {
        "Untagged",        // must stay first: it is the default
        "Respawn",
        "Finish",
        "EditorOnly",
        "MainCamera",
        "Player",
        "GameController",
    };
    return s_Tags;
}

int TagLayerManager::CountBuiltinTags()
{
    return static_cast<int>(BuiltinTags().size());
}

bool TagLayerManager::Initialize()
{
    // Idempotent: re-initialising (editor play-mode restart, hot reload) must
    // not duplicate the built-ins, or `GetTagCount()` grows every cycle.
    m_UserTags.clear();

    for (const BuiltinLayer& entry : k_BuiltinLayers)
    {
        m_LayerNames[entry.layer] = entry.name;
    }
    for (int i = kFirstUserLayer; i < kLayerCount; ++i)
    {
        m_LayerNames[i].clear();
    }

    ResetCollisionMatrix();
    return true;
}

void TagLayerManager::Shutdown()
{
    m_UserTags.clear();
    for (eastl::string& name : m_LayerNames)
    {
        name.clear();
    }
    for (uint32_t& mask : m_CollidesWith)
    {
        mask = 0u;
    }
}

// ---- tags ------------------------------------------------------------------

bool TagLayerManager::AddTag(const eastl::string& tag)
{
    if (tag.empty() || HasTag(tag))
    {
        return false;
    }
    m_UserTags.push_back(tag);
    return true;
}

bool TagLayerManager::RemoveTag(const eastl::string& tag)
{
    if (tag.empty())
    {
        return false;
    }

    const eastl::vector<eastl::string>& builtin = BuiltinTags();
    if (eastl::find(builtin.begin(), builtin.end(), tag) != builtin.end())
    {
        return false;
    }

    const auto it = eastl::find(m_UserTags.begin(), m_UserTags.end(), tag);
    if (it == m_UserTags.end())
    {
        return false;
    }
    m_UserTags.erase(it);
    return true;
}

bool TagLayerManager::HasTag(const eastl::string& tag) const
{
    if (tag.empty())
    {
        return false;
    }
    const eastl::vector<eastl::string>& builtin = BuiltinTags();
    if (eastl::find(builtin.begin(), builtin.end(), tag) != builtin.end())
    {
        return true;
    }
    return eastl::find(m_UserTags.begin(), m_UserTags.end(), tag) != m_UserTags.end();
}

eastl::vector<eastl::string> TagLayerManager::GetTags() const
{
    eastl::vector<eastl::string> out = BuiltinTags();
    out.insert(out.end(), m_UserTags.begin(), m_UserTags.end());
    return out;
}

// ---- layers ----------------------------------------------------------------

bool TagLayerManager::SetLayerName(int layer, const eastl::string& name)
{
    if (!IsUserLayer(layer) || name.empty())
    {
        return false;
    }

    // Duplicate names are rejected across the whole table, built-ins included:
    // `NameToLayer` has one answer per name and dangling a second candidate
    // makes the choice invisible to the caller.
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (i != layer && m_LayerNames[i] == name)
        {
            return false;
        }
    }

    m_LayerNames[layer] = name;
    return true;
}

eastl::string TagLayerManager::LayerToName(int layer) const
{
    if (layer < 0 || layer >= kLayerCount)
    {
        return eastl::string();
    }
    return m_LayerNames[layer];
}

int TagLayerManager::NameToLayer(const eastl::string& name) const
{
    if (name.empty())
    {
        return kInvalidLayer;
    }
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (m_LayerNames[i] == name)
        {
            return i;
        }
    }
    return kInvalidLayer;
}

eastl::vector<eastl::string> TagLayerManager::GetLayerNames() const
{
    eastl::vector<eastl::string> out;
    out.reserve(kLayerCount);
    for (const eastl::string& name : m_LayerNames)
    {
        out.push_back(name);
    }
    return out;
}

// ---- layer masks -----------------------------------------------------------

uint32_t TagLayerManager::LayerToMask(int layer)
{
    if (layer < 0 || layer >= kLayerCount)
    {
        return 0u;
    }
    return 1u << static_cast<uint32_t>(layer);
}

uint32_t TagLayerManager::GetMask(const eastl::vector<eastl::string>& layer_names) const
{
    uint32_t mask = 0u;
    for (const eastl::string& name : layer_names)
    {
        const int layer = NameToLayer(name);
        if (layer != kInvalidLayer)
        {
            mask |= LayerToMask(layer);
        }
    }
    return mask;
}

bool TagLayerManager::MaskContainsLayer(uint32_t mask, int layer)
{
    return (mask & LayerToMask(layer)) != 0u;
}

// ---- collision matrix ------------------------------------------------------

void TagLayerManager::SetIgnoreLayerCollision(int layer_a, int layer_b, bool ignore)
{
    if (layer_a < 0 || layer_a >= kLayerCount || layer_b < 0 || layer_b >= kLayerCount)
    {
        return;
    }

    const uint32_t bit_a = LayerToMask(layer_a);
    const uint32_t bit_b = LayerToMask(layer_b);

    // Written in both directions here, in one place. Any other call site that
    // wanted to poke a single half would reintroduce the asymmetry bug this
    // layout exists to make unrepresentable.
    if (ignore)
    {
        m_CollidesWith[layer_a] &= ~bit_b;
        m_CollidesWith[layer_b] &= ~bit_a;
    }
    else
    {
        m_CollidesWith[layer_a] |= bit_b;
        m_CollidesWith[layer_b] |= bit_a;
    }
}

bool TagLayerManager::GetIgnoreLayerCollision(int layer_a, int layer_b) const
{
    if (layer_a < 0 || layer_a >= kLayerCount || layer_b < 0 || layer_b >= kLayerCount)
    {
        // Out of range is "not a real pair". Reporting `true` (ignoring) here
        // would make a nonsense query look like a deliberate exclusion.
        return false;
    }
    return (m_CollidesWith[layer_a] & LayerToMask(layer_b)) == 0u;
}

void TagLayerManager::ResetCollisionMatrix()
{
    constexpr uint32_t kAll = 0xFFFFFFFFu;
    for (uint32_t& mask : m_CollidesWith)
    {
        mask = kAll;
    }

    // "Ignore Raycast" is excluded from every layer, itself included, so a
    // raycast can never see it. This is the whole point of the layer.
    m_CollidesWith[kIgnoreRaycastLayer] = 0u;
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (i != kIgnoreRaycastLayer)
        {
            m_CollidesWith[i] &= ~LayerToMask(kIgnoreRaycastLayer);
        }
    }
}

uint32_t TagLayerManager::GetCollisionMask(int layer) const
{
    if (layer < 0 || layer >= kLayerCount)
    {
        return 0u;
    }
    return m_CollidesWith[layer];
}
