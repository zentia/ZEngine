#pragma once

#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Resource/ResType/Data/BlendState.h"

#include <vector>

class AnimationResultElement
{
public:
    int index;
    Matrix4x4_ transform;
};

class AnimationResult
{
public:
    std::vector<AnimationResultElement> node;
};

class AnimationComponentRes
{
public:
    DECLARE_SERIALIZE(AnimationComponentRes)

    eastl::string skeleton_file_path;
    BlendState blend_state;
    // animation to skeleton map
    float frame_position;  // 0-1

    // PR-SK3: optional state machine. Empty == legacy blend-state playback,
    // which is what every asset authored before the controller existed has.
    // Deliberately a PATH and not a PPtr<AnimatorController>: the surrounding
    // BlendState addresses clips by path too, and PPtr round-tripping is still
    // being rolled out (PR-SE3a).
    eastl::string controller_file_path;

    AnimationResult animation_result;
};

template<typename TransferFunction>
void AnimationComponentRes::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(skeleton_file_path, "skeleton_file_path");
    transfer.Transfer(blend_state, "blend_state");
    transfer.Transfer(frame_position, "frame_position");
    // Old .zasset files have no "controller_file_path" node; SafeBinaryRead
    // leaves the field at its default "" and the component keeps blending.
    transfer.Transfer(controller_file_path, "controller_file_path");
}

INSTANTIATE_TEMPLATE_TRANSFER_WITH_DECL(AnimationComponentRes, , Transfer, void)
