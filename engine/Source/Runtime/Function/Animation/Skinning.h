#pragma once

// =============================================================================
// Skeletal skinning core (PR-SK1).
//
// Pure math + data: no engine systems, no logging, no allocation policies. It
// can be exercised by a headless smoke test (see
// `Runtime/Function/Animation/Test/AnimationSkinningSmokeTest.cpp`), which is
// the only way this layer can be regression-tested without a GPU.
//
// Conventions (must match the GPU path -- they are the #1 source of "my
// character explodes" bugs):
//
//   * Bone indices are 0-based inside this file's palettes.
//   * The palette handed to the GPU has ONE EXTRA SLOT IN FRONT: slot 0 is the
//     identity "root" and bone i lives at slot i+1. The shaders test
//     `index > 0` so that an unused influence (stored as 0) skips cleanly.
//     `SkinMeshRenderer` builds that layout; see
//     `BuildGpuPaletteWithRootSlot()` below so the offset stops being folklore.
//   * Skinning matrix for bone i is  global[i] * inverse(globalBind[i]).
//     `globalBind` is derived from `RawBone::binding_pose` through the
//     hierarchy, NOT from `RawBone::tpose_matrix`: nothing in the codebase ever
//     writes `tpose_matrix` (grep it), so trusting it silently produces
//     unskinned output.
// =============================================================================

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Core/Math/Quaternion.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

#include <cstdint>
#include <vector>

namespace AnimationSkinning
{
    /// glTF / FBX practice: four influences per vertex.
    constexpr uint32_t kMaxInfluences = 4u;

    /// How a clip behaves outside [0, 1].
    enum class WrapMode
    {
        /// Freeze on the first / last frame (one-shot animations).
        Clamp,
        /// Restart from the beginning (walk / run cycles).
        Loop,
        /// Run forwards then backwards (idle breathing).
        PingPong
    };

    /// One bone's local (parent-relative) pose.
    struct BonePose
    {
        Vector3 m_Position {Vector3::ZERO};
        Quaternion m_Rotation {Quaternion::IDENTITY};
        Vector3 m_Scale {Vector3::UNIT_SCALE};

        static BonePose Identity()
        {
            return BonePose {};
        }

        Matrix4x4 GetMatrix() const;
    };

    // ---- hierarchy ----------------------------------------------------------

    /// Reorders `in_out_bones` so a parent always precedes its children and
    /// rewrites `index` / `parent_index` to match the new positions. Bones whose
    /// parent is missing or out of range are treated as roots. Returns false on a
    /// cycle (the hierarchy is unusable in that case).
    ///
    /// `Skeleton::BuildSkeleton()` used to bail out silently when a skeleton was
    /// not already in topological order, which showed up as "the character does
    /// not animate at all" with no diagnostic. Sorting here removes that trap.
    bool EnsureTopologicalOrder(std::vector<RawBone>& in_out_bones);

    /// Parent index of a bone, or -1 for a root / broken reference.
    int32_t ResolveParentIndex(const std::vector<RawBone>& in_bones, size_t in_bone_index);

    // ---- bind pose ----------------------------------------------------------

    /// Global (model-space) matrices of the bind pose, one per bone.
    void ComputeGlobalBindMatrices(const std::vector<RawBone>& in_bones, std::vector<Matrix4x4>& out_global_bind);

    /// Inverse of `ComputeGlobalBindMatrices`. A vertex authored in mesh space is
    /// multiplied by this before it enters the animated bone's space.
    void ComputeInverseBindMatrices(const std::vector<RawBone>& in_bones, std::vector<Matrix4x4>& out_inverse_bind);

    /// Global matrices for the current frame given one local pose per bone.
    /// `in_bones` supplies the hierarchy only; it must be topologically ordered.
    void ComputeGlobalMatrices(const std::vector<RawBone>& in_bones,
                               const std::vector<BonePose>& in_local_poses,
                               std::vector<Matrix4x4>& out_global);

    /// palette[i] = global[i] * inverseBind[i].
    void BuildSkinningPalette(const std::vector<Matrix4x4>& in_global,
                              const std::vector<Matrix4x4>& in_inverse_bind,
                              std::vector<Matrix4x4>& out_palette);

    /// Prepends the identity root slot the shaders expect (see file header).
    void BuildGpuPaletteWithRootSlot(const std::vector<Matrix4x4>& in_palette, std::vector<Matrix4x4>& out_gpu_palette);

    // ---- clip sampling ------------------------------------------------------

    /// Maps an arbitrary normalised time into [0, 1] according to `in_mode`.
    float ApplyWrap(float in_normalized_time, WrapMode in_mode);

    /// Interpolates one channel at normalised time (0 = first key, 1 = last key).
    /// Missing key arrays fall back to the identity for that component rather
    /// than reading out of bounds (the old code clamped per-component in a way
    /// that mixed frames when the three curves had different lengths).
    BonePose SampleChannel(const AnimationChannel& in_channel, float in_normalized_time, WrapMode in_mode);

    /// Samples every channel of a clip. Output is sized to `clip.node_count`.
    void SampleClip(const AnimationClip& in_clip, float in_normalized_time, WrapMode in_mode, std::vector<BonePose>& out_poses);

    /// out = a * (1 - weight) + b * weight.
    void BlendPose(const BonePose& in_a, const BonePose& in_b, float in_weight, BonePose& out_pose);

    /// Composes an animation delta on top of a bind pose the way this engine's
    /// `Node` does it (Skeleton::ApplyAnimation calls Translate/Rotate/Scale,
    /// whose defaults are parent-space):
    ///   position = bind.position + delta.position
    ///   rotation = delta.rotation * bind.rotation
    ///   scale    = bind.scale * delta.scale (component-wise)
    /// The clip data in this engine is a DELTA on the bind pose, not an absolute
    /// local transform, so every sampled pose goes through here before it
    /// reaches `ComputeGlobalMatrices()`.
    BonePose ComposeDelta(const BonePose& in_bind, const BonePose& in_delta);

    /// Weighted accumulation for N clips. Start with `in_out_accumulator` zeroed
    /// and `in_out_total_weight` at 0, call this per clip, then call
    /// `NormalizeBlendAccumulator()`.
    void AccumulateBlend(const BonePose& in_pose, float in_weight, BonePose& in_out_accumulator, float& in_out_total_weight);

    /// Divides the accumulated pose by the accumulated weight. Blending rotations
    /// this way is the standard "quaternion blender": summing same-hemisphere
    /// quaternions and normalising is stable for reasonably similar poses, which
    /// is what clip blending needs (180-degree mixes are undefined by nature).
    void NormalizeBlendAccumulator(BonePose& in_out_accumulator, float in_total_weight);

    // ---- CPU skinning -------------------------------------------------------

    struct VertexInfluence
    {
        int32_t m_Indices[kMaxInfluences] {-1, -1, -1, -1};
        float m_Weights[kMaxInfluences] {0.0f, 0.0f, 0.0f, 0.0f};
    };

    /// One (bone, weight) pair as read straight from an asset: an FBX skin
    /// cluster or glTF `JOINTS_0` / `WEIGHTS_0`.
    struct RawInfluence
    {
        /// 0-based bone index.
        int32_t m_BoneIndex {0};
        float m_Weight {0.0f};
    };

    /// Reduces an arbitrary-length influence list to the four heaviest and
    /// rescales them to sum to 1, which is what the shaders expect.
    ///
    /// `in_influences` carries 0-BASED bone indices; the output stores GPU
    /// palette slots (bone + 1), because slot 0 of the palette is the identity
    /// root. Returns false when there is no usable influence at all -- the
    /// caller should then emit a bind-pose vertex rather than a zeroed one.
    bool PackVertexInfluence(const RawInfluence* in_influences, size_t in_count, VertexInfluence& out_influence);

    /// Drops negative indices and rescales the weights to sum to 1. A vertex with
    /// no usable influence falls back to bone 0 at weight 1 so it stays attached
    /// to the root instead of collapsing to the origin.
    void NormalizeInfluence(VertexInfluence& in_out_influence);

    /// Weighted sum of the palette entries this vertex binds to.
    Matrix4x4 BuildBlendMatrix(const VertexInfluence& in_influence, const Matrix4x4* in_palette, size_t in_palette_size);

    Vector3 SkinPosition(const Vector3& in_bind_position,
                         const VertexInfluence& in_influence,
                         const Matrix4x4* in_palette,
                         size_t in_palette_size);

    /// Normals / tangents: skinned with the linear part and re-normalised. The
    /// inverse-transpose is skipped on purpose -- skinning matrices are rigid +
    /// uniform scale in practice, and the GPU path (mesh.vert / mesh.vert.hlsl)
    /// does the same, so CPU and GPU agree.
    Vector3 SkinDirection(const Vector3& in_bind_direction,
                          const VertexInfluence& in_influence,
                          const Matrix4x4* in_palette,
                          size_t in_palette_size);

    /// Batch convenience wrapper; either output array may be null.
    void SkinVertexBuffer(const Vector3* in_bind_positions,
                          const Vector3* in_bind_normals,
                          const VertexInfluence* in_influences,
                          size_t in_vertex_count,
                          const std::vector<Matrix4x4>& in_palette,
                          Vector3* out_positions,
                          Vector3* out_normals);
}  // namespace AnimationSkinning
