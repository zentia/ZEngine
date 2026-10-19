// =============================================================================
// Skeletal skinning smoke test (PR-SK1).
//
// Exercises the CPU half of skeletal animation with no engine, no window and no
// GPU, so it runs in CI:
//
//   cmake -B build -DZENGINE_BUILD_ANIMATION_SMOKE_TEST=ON
//   cmake --build build --target AnimationSkinningSmokeTest
//   ./build/.../AnimationSkinningSmokeTest
//
// Scenarios:
//   S1  topological sort (out-of-order input, cycle detection)
//   S2  bind-pose globals / inverse bind / palette is identity at rest
//   S3  a vertex bound 100% to a bone does not move at rest
//   S4  rotating a parent bone moves the child's vertices
//   S5  two-bone weighted blend lands between the two poses
//   S6  influence normalisation (weights, negative indices, unbound vertices)
//   S7  clip sampling + wrap modes (clamp / loop / ping-pong)
//   S8  pose blending (binary + N-way accumulation)
//   S9  GPU palette layout (root slot at 0, bone i at i+1)
//   S10 normals are rotated and stay unit length
// =============================================================================

#include "Runtime/Function/Animation/Skinning.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int g_Failures = 0;
    int g_Checks = 0;

    void Check(bool in_condition, const char* in_message)
    {
        ++g_Checks;
        if (!in_condition)
        {
            ++g_Failures;
            std::printf("[FAIL] %s\n", in_message);
        }
        else
        {
            std::printf("[ ok ] %s\n", in_message);
        }
    }

    void CheckVec(const Vector3& in_actual, const Vector3& in_expected, float in_tolerance, const char* in_message)
    {
        ++g_Checks;
        const float dx = std::fabs(in_actual.x - in_expected.x);
        const float dy = std::fabs(in_actual.y - in_expected.y);
        const float dz = std::fabs(in_actual.z - in_expected.z);
        if (dx > in_tolerance || dy > in_tolerance || dz > in_tolerance)
        {
            ++g_Failures;
            std::printf("[FAIL] %s (got %.4f %.4f %.4f, want %.4f %.4f %.4f)\n",
                        in_message,
                        in_actual.x,
                        in_actual.y,
                        in_actual.z,
                        in_expected.x,
                        in_expected.y,
                        in_expected.z);
        }
        else
        {
            std::printf("[ ok ] %s\n", in_message);
        }
    }

    void CheckNear(float in_actual, float in_expected, float in_tolerance, const char* in_message)
    {
        ++g_Checks;
        if (std::fabs(in_actual - in_expected) > in_tolerance)
        {
            ++g_Failures;
            std::printf("[FAIL] %s (got %.5f, want %.5f)\n", in_message, in_actual, in_expected);
        }
        else
        {
            std::printf("[ ok ] %s\n", in_message);
        }
    }

    using AnimationSkinning::BonePose;
    using AnimationSkinning::VertexInfluence;
    using AnimationSkinning::WrapMode;

    RawBone MakeBone(int in_index, int in_parent, const Vector3& in_position)
    {
        RawBone bone;
        bone.index = in_index;
        bone.parent_index = in_parent;
        bone.binding_pose = TransformTRS(in_position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);
        return bone;
    }

    BonePose MakeBonePose(const Vector3& in_position)
    {
        BonePose pose;
        pose.m_Position = in_position;
        pose.m_Rotation = Quaternion::IDENTITY;
        pose.m_Scale = Vector3::UNIT_SCALE;
        return pose;
    }

    /// Two-bone chain: root at the origin, child one unit along +X.
    std::vector<RawBone> MakeArmSkeleton()
    {
        std::vector<RawBone> bones;
        bones.push_back(MakeBone(0, -1, Vector3(0.0f, 0.0f, 0.0f)));
        bones.push_back(MakeBone(1, 0, Vector3(1.0f, 0.0f, 0.0f)));
        return bones;
    }

    AnimationClip MakeTwoFrameClip(const Vector3& in_first, const Vector3& in_second)
    {
        AnimationClip clip;
        clip.total_frame = 2;
        clip.node_count = 1;
        AnimationChannel channel;
        channel.position_keys = {in_first, in_second};
        channel.rotation_keys = {Quaternion::IDENTITY, Quaternion::IDENTITY};
        channel.scaling_keys = {Vector3::UNIT_SCALE, Vector3::UNIT_SCALE};
        clip.node_channels.push_back(channel);
        return clip;
    }
}  // namespace

int main()
{
    // ---- S1: topological sort ----
    {
        std::vector<RawBone> bones;
        bones.push_back(MakeBone(1, 0, Vector3(1.0f, 0.0f, 0.0f)));  // child first on purpose
        bones.push_back(MakeBone(0, -1, Vector3(0.0f, 0.0f, 0.0f)));
        bones.push_back(MakeBone(2, 1, Vector3(1.0f, 0.0f, 0.0f)));

        Check(AnimationSkinning::EnsureTopologicalOrder(bones), "S1: out-of-order skeleton sorts");
        Check(bones.size() == 3u, "S1: no bone is lost");
        bool parents_first = true;
        for (size_t slot = 0; slot < bones.size(); ++slot)
        {
            if (static_cast<int>(slot) != bones[slot].index)
            {
                parents_first = false;
            }
            if (bones[slot].parent_index >= static_cast<int>(slot))
            {
                parents_first = false;
            }
        }
        Check(parents_first, "S1: parents precede children and indices are flat");

        std::vector<RawBone> cycle;
        cycle.push_back(MakeBone(0, 1, Vector3::ZERO));
        cycle.push_back(MakeBone(1, 0, Vector3::ZERO));
        Check(!AnimationSkinning::EnsureTopologicalOrder(cycle), "S1: a cycle is reported, not silently accepted");
    }

    // ---- S2: bind pose ----
    std::vector<Matrix4x4> inverse_bind;
    {
        const std::vector<RawBone> bones = MakeArmSkeleton();

        std::vector<Matrix4x4> global_bind;
        AnimationSkinning::ComputeGlobalBindMatrices(bones, global_bind);

        const Vector3 root_origin = global_bind[0] * Vector4(0.0f, 0.0f, 0.0f, 1.0f) == Vector4(0.0f, 0.0f, 0.0f, 1.0f)
                                        ? Vector3(0.0f, 0.0f, 0.0f)
                                        : Vector3(999.0f, 0.0f, 0.0f);
        Check(root_origin == Vector3(0.0f, 0.0f, 0.0f), "S2: root bind matrix leaves the origin alone");

        const Vector4 child_origin = global_bind[1] * Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        CheckNear(child_origin.x, 1.0f, 1.0e-5f, "S2: child bind matrix carries the +1 X offset");

        AnimationSkinning::ComputeInverseBindMatrices(bones, inverse_bind);
        Check(inverse_bind.size() == 2u, "S2: one inverse bind matrix per bone");

        // global * inverse(global) == identity for every bone: this is the
        // property that makes "rest pose renders as the authored mesh" true.
        std::vector<Matrix4x4> palette_at_rest;
        AnimationSkinning::BuildSkinningPalette(global_bind, inverse_bind, palette_at_rest);
        bool identity_everywhere = true;
        for (const Matrix4x4& matrix : palette_at_rest)
        {
            const Vector4 transformed = matrix * Vector4(0.25f, -0.5f, 1.75f, 1.0f);
            if (std::fabs(transformed.x - 0.25f) > 1.0e-4f || std::fabs(transformed.y + 0.5f) > 1.0e-4f ||
                std::fabs(transformed.z - 1.75f) > 1.0e-4f)
            {
                identity_everywhere = false;
            }
        }
        Check(identity_everywhere, "S2: palette is the identity at rest");
    }

    // ---- S3: rest pose does not move vertices ----
    {
        const std::vector<RawBone> bones = MakeArmSkeleton();
        // Rest pose == the bind pose: local poses are absolute, not deltas.
        std::vector<BonePose> poses = {BonePose::Identity(), MakeBonePose(Vector3(1.0f, 0.0f, 0.0f))};
        std::vector<Matrix4x4> global;
        AnimationSkinning::ComputeGlobalMatrices(bones, poses, global);
        std::vector<Matrix4x4> palette;
        AnimationSkinning::BuildSkinningPalette(global, inverse_bind, palette);

        VertexInfluence influence {};
        influence.m_Indices[0] = 1;
        influence.m_Weights[0] = 1.0f;

        const Vector3 skinned = AnimationSkinning::SkinPosition(Vector3(1.0f, 0.0f, 0.0f), influence, palette.data(), palette.size());
        CheckVec(skinned, Vector3(1.0f, 0.0f, 0.0f), 1.0e-4f, "S3: rest pose leaves the vertex where it was authored");
    }

    // ---- S4: rotating the parent moves the child's vertices ----
    {
        const std::vector<RawBone> bones = MakeArmSkeleton();
        std::vector<BonePose> poses = {BonePose::Identity(), MakeBonePose(Vector3(1.0f, 0.0f, 0.0f))};
        // 90 degrees about +Z: the arm swings from +X to +Y.
        poses[0].m_Rotation = Quaternion(Radian(1.5707963267948966f), Vector3::UNIT_Z);

        std::vector<Matrix4x4> global;
        AnimationSkinning::ComputeGlobalMatrices(bones, poses, global);
        std::vector<Matrix4x4> palette;
        AnimationSkinning::BuildSkinningPalette(global, inverse_bind, palette);

        VertexInfluence influence {};
        influence.m_Indices[0] = 1;
        influence.m_Weights[0] = 1.0f;

        const Vector3 skinned = AnimationSkinning::SkinPosition(Vector3(1.0f, 0.0f, 0.0f), influence, palette.data(), palette.size());
        CheckVec(skinned, Vector3(0.0f, 1.0f, 0.0f), 1.0e-4f, "S4: rotating the root by 90 deg swings the tip to +Y");
    }

    // ---- S5: weighted blend between two bones ----
    {
        const std::vector<RawBone> bones = MakeArmSkeleton();
        std::vector<BonePose> poses = {BonePose::Identity(), MakeBonePose(Vector3(1.0f, 0.0f, 0.0f))};
        poses[1].m_Position = Vector3(2.0f, 2.0f, 0.0f);  // child stretches out

        std::vector<Matrix4x4> global;
        AnimationSkinning::ComputeGlobalMatrices(bones, poses, global);
        std::vector<Matrix4x4> palette;
        AnimationSkinning::BuildSkinningPalette(global, inverse_bind, palette);

        VertexInfluence influence {};
        influence.m_Indices[0] = 0;
        influence.m_Weights[0] = 0.5f;
        influence.m_Indices[1] = 1;
        influence.m_Weights[1] = 0.5f;

        // Bone 0 keeps the vertex at (1,0,0); bone 1 pushes it to (2,2,0).
        const Vector3 skinned = AnimationSkinning::SkinPosition(Vector3(1.0f, 0.0f, 0.0f), influence, palette.data(), palette.size());
        CheckVec(skinned, Vector3(1.5f, 1.0f, 0.0f), 1.0e-4f, "S5: 50/50 blend lands halfway between both poses");
    }

    // ---- S6: influence normalisation ----
    {
        VertexInfluence influence {};
        influence.m_Indices[0] = 0;
        influence.m_Weights[0] = 2.0f;
        influence.m_Indices[1] = 1;
        influence.m_Weights[1] = 2.0f;
        AnimationSkinning::NormalizeInfluence(influence);
        CheckNear(influence.m_Weights[0], 0.5f, 1.0e-6f, "S6: weights are rescaled to sum to 1");
        CheckNear(influence.m_Weights[1], 0.5f, 1.0e-6f, "S6: second weight matches");

        VertexInfluence partial {};
        partial.m_Indices[0] = 3;
        partial.m_Weights[0] = 1.0f;
        partial.m_Indices[1] = -1;
        partial.m_Weights[1] = 1.0f;  // belongs to a dropped index
        AnimationSkinning::NormalizeInfluence(partial);
        CheckNear(partial.m_Weights[1], 0.0f, 1.0e-6f, "S6: a negative bone index discards its weight");
        CheckNear(partial.m_Weights[0], 1.0f, 1.0e-6f, "S6: the surviving influence takes the whole weight");

        VertexInfluence unbound {};
        AnimationSkinning::NormalizeInfluence(unbound);
        Check(unbound.m_Indices[0] == 0 && std::fabs(unbound.m_Weights[0] - 1.0f) < 1.0e-6f,
              "S6: an unbound vertex is pinned to the root instead of collapsing");
    }

    // ---- S7: clip sampling and wrap modes ----
    {
        const AnimationClip clip = MakeTwoFrameClip(Vector3(0.0f, 0.0f, 0.0f), Vector3(10.0f, 0.0f, 0.0f));

        std::vector<BonePose> poses;
        AnimationSkinning::SampleClip(clip, 0.0f, WrapMode::Clamp, poses);
        CheckNear(poses[0].m_Position.x, 0.0f, 1.0e-5f, "S7: t=0 samples the first key");

        AnimationSkinning::SampleClip(clip, 1.0f, WrapMode::Clamp, poses);
        CheckNear(poses[0].m_Position.x, 10.0f, 1.0e-5f, "S7: t=1 samples the last key");

        AnimationSkinning::SampleClip(clip, 0.5f, WrapMode::Clamp, poses);
        CheckNear(poses[0].m_Position.x, 5.0f, 1.0e-5f, "S7: t=0.5 interpolates linearly");

        AnimationSkinning::SampleClip(clip, 5.0f, WrapMode::Clamp, poses);
        CheckNear(poses[0].m_Position.x, 10.0f, 1.0e-5f, "S7: clamp holds the last frame");

        AnimationSkinning::SampleClip(clip, 1.25f, WrapMode::Loop, poses);
        CheckNear(poses[0].m_Position.x, 2.5f, 1.0e-5f, "S7: loop wraps 1.25 to 0.25");

        AnimationSkinning::SampleClip(clip, 1.5f, WrapMode::PingPong, poses);
        CheckNear(poses[0].m_Position.x, 5.0f, 1.0e-5f, "S7: ping-pong mirrors 1.5 back to 0.5");

        // A channel missing rotation keys must not read out of bounds.
        AnimationClip sparse = clip;
        sparse.node_channels[0].rotation_keys.clear();
        AnimationSkinning::SampleClip(sparse, 0.5f, WrapMode::Loop, poses);
        Check(std::fabs(poses[0].m_Rotation.w - 1.0f) < 1.0e-5f, "S7: a missing curve falls back to identity");
    }

    // ---- S8: pose blending ----
    {
        BonePose a;
        a.m_Position = Vector3(0.0f, 0.0f, 0.0f);
        BonePose b;
        b.m_Position = Vector3(4.0f, 0.0f, 0.0f);

        BonePose blended;
        AnimationSkinning::BlendPose(a, b, 0.25f, blended);
        CheckNear(blended.m_Position.x, 1.0f, 1.0e-5f, "S8: binary blend respects the weight");

        BonePose accumulator;
        float total = 0.0f;
        AnimationSkinning::AccumulateBlend(a, 1.0f, accumulator, total);
        AnimationSkinning::AccumulateBlend(b, 3.0f, accumulator, total);
        AnimationSkinning::NormalizeBlendAccumulator(accumulator, total);
        CheckNear(accumulator.m_Position.x, 3.0f, 1.0e-5f, "S8: N-way blend is weight-normalised");
        CheckNear(accumulator.m_Rotation.length(), 1.0f, 1.0e-5f, "S8: blended rotation stays a unit quaternion");

        BonePose empty;
        float zero_total = 0.0f;
        AnimationSkinning::NormalizeBlendAccumulator(empty, zero_total);
        Check(std::fabs(empty.m_Rotation.w - 1.0f) < 1.0e-5f, "S8: a zero-weight blend degrades to identity");
    }

    // ---- S9: GPU palette layout ----
    {
        const std::vector<Matrix4x4> palette = {Matrix4x4::IDENTITY, Matrix4x4::IDENTITY};
        std::vector<Matrix4x4> gpu_palette;
        AnimationSkinning::BuildGpuPaletteWithRootSlot(palette, gpu_palette);

        Check(gpu_palette.size() == palette.size() + 1u, "S9: GPU palette has the extra root slot");
        const Vector4 root_slot = gpu_palette[0] * Vector4(1.0f, 2.0f, 3.0f, 1.0f);
        CheckNear(root_slot.x, 1.0f, 1.0e-5f, "S9: slot 0 is the identity");
        Check(gpu_palette[1] == palette[0] && gpu_palette[2] == palette[1], "S9: bone i lives at slot i + 1");
    }

    // ---- S10: normals ----
    {
        const std::vector<RawBone> bones = MakeArmSkeleton();
        std::vector<BonePose> poses = {BonePose::Identity(), MakeBonePose(Vector3(1.0f, 0.0f, 0.0f))};
        poses[0].m_Rotation = Quaternion(Radian(1.5707963267948966f), Vector3::UNIT_Z);

        std::vector<Matrix4x4> global;
        AnimationSkinning::ComputeGlobalMatrices(bones, poses, global);
        std::vector<Matrix4x4> palette;
        AnimationSkinning::BuildSkinningPalette(global, inverse_bind, palette);

        VertexInfluence influence {};
        influence.m_Indices[0] = 0;
        influence.m_Weights[0] = 1.0f;

        const Vector3 normal = AnimationSkinning::SkinDirection(Vector3(1.0f, 0.0f, 0.0f), influence, palette.data(), palette.size());
        CheckVec(normal, Vector3(0.0f, 1.0f, 0.0f), 1.0e-4f, "S10: normals rotate with the bone");
        CheckNear(std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z), 1.0f, 1.0e-5f,
                  "S10: skinned normals stay unit length");
    }

    // ---- S11: delta composition (the convention this engine's clips use) ----
    {
        const BonePose bind = MakeBonePose(Vector3(1.0f, 0.0f, 0.0f));
        BonePose delta;
        delta.m_Position = Vector3(0.0f, 0.5f, 0.0f);
        delta.m_Rotation = Quaternion(Radian(1.5707963267948966f), Vector3::UNIT_Z);
        delta.m_Scale = Vector3(2.0f, 2.0f, 2.0f);

        const BonePose composed = AnimationSkinning::ComposeDelta(bind, delta);
        CheckVec(composed.m_Position, Vector3(1.0f, 0.5f, 0.0f), 1.0e-5f,
                 "S11: a delta translation is added in parent space");
        CheckNear(composed.m_Scale.x, 2.0f, 1.0e-5f, "S11: a delta scale multiplies component-wise");
        CheckNear(composed.m_Rotation.length(), 1.0f, 1.0e-5f, "S11: a composed rotation stays normalised");

        // Rotating the composed result: the delta must be applied in parent space
        // (pre-multiplied), i.e. the bind translation must NOT rotate with it.
        const Vector3 rotated_axis = composed.m_Rotation * Vector3::UNIT_X;
        CheckVec(rotated_axis, Vector3(0.0f, 1.0f, 0.0f), 1.0e-4f, "S11: delta rotation pre-multiplies the bind pose");
    }

    // ---- S12: packing raw (bone, weight) pairs the way an importer must ----
    {
        using AnimationSkinning::RawInfluence;

        RawInfluence raw[6] = {{0, 0.4f}, {1, 0.1f}, {2, 0.25f}, {3, 0.05f}, {4, 0.15f}, {5, 0.05f}};
        VertexInfluence packed;
        Check(AnimationSkinning::PackVertexInfluence(raw, 6, packed), "S12: raw influences pack");

        // Slots are bone + 1: bone 0 -> slot 1, and only the four heaviest
        // (0.40 / 0.25 / 0.15 / 0.10) survive.
        Check(packed.m_Indices[0] == 1 && packed.m_Weights[0] > 0.0f, "S12: heaviest influence wins slot bone+1");
        float total = 0.0f;
        int kept = 0;
        for (uint32_t influence = 0; influence < AnimationSkinning::kMaxInfluences; ++influence)
        {
            if (packed.m_Weights[influence] > 0.0f)
            {
                ++kept;
                Check(packed.m_Indices[influence] > 0, "S12: every packed index is a valid palette slot");
            }
            total += packed.m_Weights[influence];
        }
        Check(kept == 4, "S12: exactly four influences are kept");
        CheckNear(total, 1.0f, 1.0e-5f, "S12: packed weights sum to 1 after dropping the tail");

        VertexInfluence empty;
        Check(!AnimationSkinning::PackVertexInfluence(nullptr, 0, empty), "S12: an empty influence list is rejected");
        RawInfluence all_zero[2] = {{0, 0.0f}, {1, -1.0f}};
        Check(!AnimationSkinning::PackVertexInfluence(all_zero, 2, empty), "S12: zero/negative weights are rejected");
    }

    std::printf("\nSkinning smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
