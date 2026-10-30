#pragma once

#include "Runtime/Core/Base/EngineSystem.h"

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include <cstdint>

// =============================================================================
// Tags & Layers -- Unity's project-wide name tables plus the layer collision
// matrix (Edit > Project Settings > Tags and Layers).
//
// Why this is a system and not a pile of statics
// ----------------------------------------------
// Two independent consumers need the SAME table, and they must never disagree:
//
//   * `GameObject::CompareTag()` / the Inspector's tag dropdown -- naming.
//   * physics queries (`Physics.Raycast(origin, dir, maxDistance, layerMask)`)
//     -- the collision matrix decides whether a hit against layer N is even
//     reported.
//
// A disagreement is silent: the query returns "no hit" and the game reads that
// as "nothing is there", which is exactly what a correct empty scene looks
// like. One owner makes that impossible.
//
// Layer numbering follows Unity: 32 slots, 0..5 are built-in and locked (the
// editor may rename a user layer but never these), 6 and 7 are reserved, and
// 8..31 are user layers. The reserved gap matters -- code that hard-codes
// "layer 8 is the first free one" breaks the moment someone adds a built-in,
// which is why `kFirstUserLayer` exists instead of a literal 8 sprinkled
// around.
//
// The collision matrix is stored as 32 `uint32_t` masks rather than a
// `bool[32][32]`: a query wants "which layers does layer N collide with" as a
// mask it can AND with the caller's layer mask, and building that on every
// raycast would be wasted work. Storing the mask directly also makes the
// invariant "collision is symmetric" structural -- `SetIgnoreLayerCollision`
// writes both directions in one place, so `Get(a,b) != Get(b,a)` cannot be
// represented.
// =============================================================================

class TagLayerManager : public IEngineSystem
{
public:
    /// 32 on every platform. Not configurable: layers are a bit index in a
    /// `uint32_t` all the way down to the physics query API, and widening that
    /// would be an ABI change in `PhysicsScene::Raycast`.
    static constexpr int kLayerCount = 32;

    /// 0..5 are Unity's built-ins (Default / TransparentFX / Ignore Raycast /
    /// (empty) / Water / UI); 6 and 7 are reserved. User layers start at 8.
    static constexpr int kFirstUserLayer = 8;

    /// The tag every GameObject carries until someone sets another one.
    /// Never an empty string: `CompareTag("")` must be a definite "no" rather
    /// than "matches because both are unset", otherwise `FindGameObjectsWithTag`
    /// on a freshly created object silently returns everything.
    static constexpr const char* kUntagged = "Untagged";

    static constexpr int kInvalidLayer = -1;

    // ---- IEngineSystem -----------------------------------------------------
    // Core phase: physics queries consult the collision matrix, and gameplay
    // components read `layer` in their own Initialize().
    SystemInitPhase GetInitPhase() const override { return SystemInitPhase::Core; }
    bool Initialize() override;
    void Shutdown() override;
    std::string GetName() const override { return AUTO_GET_CLASS_NAME(); }

    // ---- tags --------------------------------------------------------------
    /// Adds a tag. Returns false (no duplicate) when it already exists, and for
    /// an empty name -- an empty tag is indistinguishable from "unset" and would
    /// make `CompareTag` lie.
    bool AddTag(const eastl::string& tag);

    /// Removes a user tag. The built-ins (`kUntagged`, "Respawn", ...) can never
    /// be removed: `GameObject::m_Tag` defaults to `kUntagged`, so deleting it
    /// would leave live objects naming a tag that no longer exists.
    bool RemoveTag(const eastl::string& tag);

    bool HasTag(const eastl::string& tag) const;

    /// Built-ins first, then user tags in insertion order -- the order the
    /// Inspector dropdown shows.
    eastl::vector<eastl::string> GetTags() const;

    int GetTagCount() const { return static_cast<int>(m_UserTags.size()) + CountBuiltinTags(); }

    // ---- layers ------------------------------------------------------------
    /// Names a user layer (8..31). Returns false for a locked slot, an out of
    /// range index, an empty name, or a name already used by another layer --
    /// duplicate layer names would make `NameToLayer` ambiguous and therefore
    /// make every `layerMask` built from a name a coin flip.
    bool SetLayerName(int layer, const eastl::string& name);

    /// Empty string for an unnamed user layer; never null.
    eastl::string LayerToName(int layer) const;

    /// `kInvalidLayer` (-1) when the name is unknown. -1 rather than 0 because
    /// 0 is **Default** -- returning it for a typo would silently aim every
    /// query at the default layer instead of failing.
    int NameToLayer(const eastl::string& name) const;

    /// All 32 names, indexed by layer. Unset user slots come back empty.
    /// For display; use `LayerToName` when you already hold an index.
    eastl::vector<eastl::string> GetLayerNames() const;

    /// True for 8..31. The Inspector greys out everything else.
    bool IsUserLayer(int layer) const { return layer >= kFirstUserLayer && layer < kLayerCount; }

    // ---- layer masks -------------------------------------------------------
    /// 0 for an out-of-range layer, not 1<<layer -- shifting by 32+ is UB, and
    /// a query that asked for a nonsense layer should match nothing rather than
    /// match whatever the shift happened to produce.
    static uint32_t LayerToMask(int layer);

    /// Union of `LayerToMask` over the given names. Unknown names contribute
    /// nothing (and are not an error) so a mask built from a list that later
    /// loses a layer degrades instead of throwing.
    uint32_t GetMask(const eastl::vector<eastl::string>& layer_names) const;

    static bool MaskContainsLayer(uint32_t mask, int layer);

    // ---- collision matrix --------------------------------------------------
    /// Marks two layers as non-colliding. Symmetric by construction; a layer
    /// against itself is allowed and means "objects on this layer ignore each
    /// other". Out-of-range indices are ignored.
    void SetIgnoreLayerCollision(int layer_a, int layer_b, bool ignore);

    bool GetIgnoreLayerCollision(int layer_a, int layer_b) const;

    /// Every pair collides again. `ignoreRaycastLayer` is excluded from
    /// everything afterwards, matching Unity's "Ignore Raycast" semantics.
    void ResetCollisionMatrix();

    /// The mask `layer` collides with -- AND this with a query's layer mask.
    /// Out-of-range layer returns 0 (collides with nothing).
    uint32_t GetCollisionMask(int layer) const;

    /// Layer 2 in Unity: objects on it are invisible to every raycast.
    static constexpr int kIgnoreRaycastLayer = 2;

private:
    static int CountBuiltinTags();

    /// Tags the project always has. Separate from `m_UserTags` so
    /// `RemoveTag()` cannot delete them and `GetTags()` can keep them first.
    static const eastl::vector<eastl::string>& BuiltinTags();

    eastl::vector<eastl::string> m_UserTags;
    eastl::string m_LayerNames[kLayerCount];
    uint32_t m_CollidesWith[kLayerCount] {0};
};
