#pragma once
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

#include <vector>
#include "AnimationClip.generated.h"

class AnimNodeMap
{
public:
    DECLARE_SERIALIZE(AnimNodeMap)

    std::vector<eastl::string> convert;
};

class AnimationChannel
{
public:
    DECLARE_SERIALIZE(AnimationChannel)

    eastl::string name;
    std::vector<Vector3> position_keys;
    std::vector<Quaternion> rotation_keys;
    std::vector<Vector3> scaling_keys;
};

class AnimationClip
{
public:
    DECLARE_SERIALIZE(AnimationClip)

    int total_frame {0};
    int node_count {0};
    std::vector<AnimationChannel> node_channels;

    /// Clip length in SECONDS (P0.1). Baked by the FBX importer from the FBX's
    /// own playback duration; the key arrays alone cannot express it, because
    /// `total_frame` is a sample count and the bake FPS is not stored anywhere.
    ///
    /// Why it had to exist: `AnimatorState::clip_length` was the only source of
    /// a clip's duration, and it is HAND-AUTHORED per state. That is already
    /// error-prone with one clip per state; a BlendTree has N children per
    /// state, so hand-filling N lengths that must stay in sync with the assets
    /// is not workable.
    ///
    /// 0 means "unknown" -- every clip written before this field existed reads
    /// back as 0 (SafeBinaryRead returns kNotFound for the missing node and
    /// leaves the default), and consumers must treat 0 as "fall back to the
    /// authored length" rather than as a zero-length clip.
    ///
    /// Transferred LAST, after `node_channels`: the serializer's positional
    /// readers take the writer's field sequence, so a new field may only be
    /// appended -- inserting one mid-sequence misaligns the stream for every
    /// existing asset.
    float duration {0.0f};
};

ZCLASS()
class AnimationAsset : public Object
{
    ZGENERATED_BODY(AnimationAsset)

public:
    ZPROPERTY()
    AnimNodeMap node_map;
    ZPROPERTY()
    AnimationClip clip_data;
    ZPROPERTY()
    eastl::string skeleton_file_path;
};
