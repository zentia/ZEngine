// =============================================================================
// Animation sampling-pipeline smoke test (P0.2, doc/animation/P0_2_NO_PER_FRAME_CLIP_COPY.md).
//
// Exercises the REAL per-frame path that AnimationComponent::Tick runs:
//
//   BlendState --AnimationSystem--> BlendStateWithClipData
//              --Skeleton::ApplyAnimation--> bone pose
//              --Skeleton::OutputAnimationResult--> skinning palette
//
// with assets that go through the real AssetManager (written to a temp
// directory, then loaded by path -- the same way AnimatorSmokeTest A11 does it).
// No window, no GPU.
//
//   P1  single clip, analytic bone positions at three phases
//   P2  two clips at 0.5 / 0.5
//   P3  per-clip bone masks
//   P4  a missing clip is skipped, not dereferenced
//   P7  A/B pose hash: an AnimatorInstance crossfade (pipeline level) and an
//       AnimationComponent (component level) over 200 frames. The printed
//       hashes must be IDENTICAL before and after the P0.2 refactor -- same
//       compiler, same config, so a bitwise comparison is legitimate.
//
//   --bench [--iters N]   Release timing of one pipeline evaluation for
//                         30 / 120 / 600-frame clips (60 bones, 2 clips).
//                         Not run by ctest.
//
//   cmake -B build -DZENGINE_BUILD_ANIMATION_PIPELINE_SMOKE_TEST=ON
//   cmake --build build --target AnimationPipelineSmokeTest
// =============================================================================

#include "RegisterRuntime.h"

#include "Runtime/BaseClasses/ObjectManager.h"
#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Core/Math/Math.h"
#include "Runtime/Function/Animation/AnimationSystem.h"
#include "Runtime/Function/Animation/AnimatorInstance.h"
#include "Runtime/Function/Animation/Skeleton.h"
#include "Runtime/Function/Framework/Component/Animation/AnimationComponent.h"
#include "Runtime/Resource/Asset/AssetManager.h"
#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/AnimatorController.h"
#include "Runtime/Resource/ResType/Data/BlendState.h"
#include "Runtime/Resource/ResType/Data/BoneBlendMask.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

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
        const bool ok = std::fabs(in_actual.x - in_expected.x) <= in_tolerance &&
                        std::fabs(in_actual.y - in_expected.y) <= in_tolerance &&
                        std::fabs(in_actual.z - in_expected.z) <= in_tolerance;
        if (!ok)
        {
            std::printf("       (got %.6f %.6f %.6f, want %.6f %.6f %.6f)\n",
                        static_cast<double>(in_actual.x), static_cast<double>(in_actual.y),
                        static_cast<double>(in_actual.z), static_cast<double>(in_expected.x),
                        static_cast<double>(in_expected.y), static_cast<double>(in_expected.z));
        }
        Check(ok, in_message);
    }

    // ---- hashing (P7) ---------------------------------------------------------

    struct Fnv1a64
    {
        uint64_t value {1469598103934665603ull};

        void Bytes(const void* in_data, size_t in_size)
        {
            const auto* bytes = static_cast<const unsigned char*>(in_data);
            for (size_t i = 0; i < in_size; ++i)
            {
                value ^= bytes[i];
                value *= 1099511628211ull;
            }
        }
    };

    static_assert(sizeof(Matrix4x4_) == 16 * sizeof(float), "palette hashing assumes 16 packed floats");

    void HashResult(Fnv1a64& in_out_hash, const AnimationResult& in_result)
    {
        for (const AnimationResultElement& element : in_result.node)
        {
            in_out_hash.Bytes(&element.index, sizeof(element.index));
            in_out_hash.Bytes(&element.transform, sizeof(Matrix4x4_));
        }
    }

    // ---- assets ---------------------------------------------------------------

    std::filesystem::path g_TempDir;
    std::vector<Object*> g_Produced;

    template<typename T>
    T* Produce()
    {
        Object* object = GET_SYSTEM(ObjectManager)->Produce(TypeOf<T>(), 0);
        if (object != nullptr)
        {
            g_Produced.push_back(object);
        }
        return static_cast<T*>(object);
    }

    /// Writes `object` to <temp>/<file_name> and returns the absolute path the
    /// runtime will be asked to load ("" on failure).
    eastl::string WriteAsset(Object& object, const char* file_name)
    {
        const std::filesystem::path path = g_TempDir / file_name;
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!GET_SYSTEM(AssetManager)->WriteObjectToDiskThreadSafe(path, object))
        {
            std::printf("FATAL: cannot write %s\n", path.generic_string().c_str());
            return eastl::string();
        }
        return eastl::string(path.generic_string().c_str());
    }

    RawBone MakeBone(int in_index, int in_parent, const Vector3& in_position, const char* in_name)
    {
        RawBone bone;
        bone.name = in_name;
        bone.index = in_index;
        bone.parent_index = in_parent;
        bone.binding_pose = TransformTRS(in_position, Quaternion::IDENTITY, Vector3::UNIT_SCALE);
        return bone;
    }

    /// A chain of `in_bone_count` bones, each one unit along +X from its parent.
    SkeletonData* MakeChainSkeleton(int in_bone_count)
    {
        SkeletonData* skeleton = Produce<SkeletonData>();
        for (int i = 0; i < in_bone_count; ++i)
        {
            const std::string name = "bone" + std::to_string(i);
            skeleton->bones_map.push_back(
                MakeBone(i, i - 1, i == 0 ? Vector3::ZERO : Vector3(1.0f, 0.0f, 0.0f), name.c_str()));
        }
        skeleton->is_flat = true;
        skeleton->root_index = 0;
        skeleton->in_topological_order = true;
        return skeleton;
    }

    AnimSkelMap* MakeIdentityMap(int in_bone_count)
    {
        AnimSkelMap* map = Produce<AnimSkelMap>();
        for (int i = 0; i < in_bone_count; ++i)
        {
            map->convert.push_back(i);
        }
        return map;
    }

    Quaternion RotZ(float in_degrees)
    {
        Quaternion rotation;
        rotation.FromAngleAxis(Radian(Degree(in_degrees)), Vector3::UNIT_Z);
        return rotation;
    }

    AnimationChannel MakeStillChannel(int in_frames)
    {
        AnimationChannel channel;
        channel.position_keys.assign(static_cast<size_t>(in_frames), Vector3::ZERO);
        channel.rotation_keys.assign(static_cast<size_t>(in_frames), Quaternion::IDENTITY);
        channel.scaling_keys.assign(static_cast<size_t>(in_frames), Vector3::UNIT_SCALE);
        return channel;
    }

    AnimationAsset* MakeClipAsset(std::vector<AnimationChannel> in_channels, int in_frames)
    {
        AnimationAsset* asset = Produce<AnimationAsset>();
        asset->clip_data.total_frame = in_frames;
        asset->clip_data.node_count = static_cast<int>(in_channels.size());
        asset->clip_data.node_channels = std::move(in_channels);
        return asset;
    }

    BlendState MakeBlendState(const std::vector<eastl::string>& in_clips,
                              const eastl::string& in_map,
                              const std::vector<float>& in_weights,
                              const std::vector<float>& in_phases,
                              const std::vector<eastl::string>& in_masks = {})
    {
        BlendState state;
        state.clip_count = static_cast<int>(in_clips.size());
        state.blend_clip_file_path = in_clips;
        state.blend_clip_file_length.assign(in_clips.size(), 1.0f);
        state.blend_anim_skel_map_path.assign(in_clips.size(), in_map);
        state.blend_weight = in_weights;
        state.blend_ratio = in_phases;
        state.blend_mask_file_path = in_masks;
        return state;
    }

    // ---- the pipeline under test ----------------------------------------------
    //
    // The ONLY place this test touches the pipeline's API, so the refactor that
    // changes that API changes this function and nothing else.

    /// Mirrors AnimationComponent::Tick: the caller owns the resolve buffer and
    /// the result, exactly as the component owns m_ClipDataScratch and
    /// m_AnimationRes.animation_result.
    BlendStateWithClipData g_ClipDataScratch;

    void EvaluatePipeline(const BlendState& in_state, Skeleton& in_out_skeleton, AnimationResult& out_result)
    {
        AnimationSystem::BuildBlendStateWithClipData(in_state, g_ClipDataScratch);
        in_out_skeleton.ApplyAnimation(g_ClipDataScratch);
        in_out_skeleton.OutputAnimationResult(out_result);
    }

    Vector3 BonePosition(const Skeleton& in_skeleton, int in_bone)
    {
        return in_skeleton.GetBones()[in_bone]._getDerivedPosition();
    }

    // ---- bench ----------------------------------------------------------------

    int RunBench(int in_iterations)
    {
        constexpr int kBones = 60;
        const int frame_counts[] = {30, 120, 600};

        SkeletonData* skeleton_data = MakeChainSkeleton(kBones);
        const eastl::string map_path = WriteAsset(*MakeIdentityMap(kBones), "bench.animskelmap.zasset");

        for (const int frames : frame_counts)
        {
            std::vector<eastl::string> clip_paths;
            for (int clip = 0; clip < 2; ++clip)
            {
                std::vector<AnimationChannel> channels;
                for (int bone = 0; bone < kBones; ++bone)
                {
                    AnimationChannel channel = MakeStillChannel(frames);
                    for (int frame = 0; frame < frames; ++frame)
                    {
                        const float t = static_cast<float>(frame) / static_cast<float>(frames - 1);
                        channel.position_keys[static_cast<size_t>(frame)] =
                            Vector3(0.01f * t * static_cast<float>(clip + 1), 0.0f, 0.0f);
                        channel.rotation_keys[static_cast<size_t>(frame)] =
                            RotZ(10.0f * t * static_cast<float>(bone % 3) - 5.0f * static_cast<float>(clip));
                    }
                    channels.push_back(std::move(channel));
                }
                const std::string name = "bench_f" + std::to_string(frames) + "_c" + std::to_string(clip) + ".anim.zasset";
                clip_paths.push_back(WriteAsset(*MakeClipAsset(std::move(channels), frames), name.c_str()));
            }

            Skeleton skeleton;
            skeleton.BuildSkeleton(*skeleton_data);
            AnimationResult result;
            Fnv1a64 sink;

            BlendState state = MakeBlendState(clip_paths, map_path, {0.5f, 0.5f}, {0.0f, 0.0f});
            for (int warm = 0; warm < 50; ++warm)
            {
                EvaluatePipeline(state, skeleton, result);
            }

            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < in_iterations; ++i)
            {
                const float phase = static_cast<float>(i % 97) / 97.0f;
                state.blend_ratio[0] = phase;
                state.blend_ratio[1] = 1.0f - phase;
                EvaluatePipeline(state, skeleton, result);
                sink.Bytes(&result.node.back().transform, sizeof(float));  // keep the work observable
            }
            const auto stop = std::chrono::steady_clock::now();
            const double microseconds = std::chrono::duration<double, std::micro>(stop - start).count();
            std::printf("BENCH bones=%d clips=2 frames=%d iters=%d us_per_iter=%.3f (sink %016llx)\n",
                        kBones, frames, in_iterations, microseconds / static_cast<double>(in_iterations),
                        static_cast<unsigned long long>(sink.value));
        }
        return 0;
    }
}  // namespace

int main(int argc, char** argv)
{
    // Unbuffered: in a Release build every printf below used to vanish. The
    // engine's shutdown path does not flush the CRT's stdio buffer, so the
    // ~2 KB of results sat in it and died with the process -- the test still
    // gated correctly by exit code, but `ctest --output-on-failure` printed
    // nothing, which makes a CI failure undiagnosable. CI builds Release.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool bench = false;
    int iterations = 2000;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--bench") == 0)
        {
            bench = true;
        }
        else if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
        {
            iterations = std::max(1, std::atoi(argv[++i]));
        }
    }

    RegisterCore();
    RegisterPlatform();
    if (!SystemRegistry::GetInstance().InitializeAll(false))
    {
        std::printf("FATAL: system bring-up failed\n");
        return 77;
    }
    if (GET_SYSTEM(AssetManager) == nullptr || GET_SYSTEM(ObjectManager) == nullptr)
    {
        std::printf("FATAL: AssetManager / ObjectManager unavailable\n");
        return 77;
    }

    g_TempDir = std::filesystem::temp_directory_path() / "zengine_anim_pipeline_smoke";
    std::error_code ec;
    std::filesystem::create_directories(g_TempDir, ec);

    int exit_code = 0;
    if (bench)
    {
        exit_code = RunBench(iterations);
    }
    else
    {
        // ---- fixture: 3-bone chain along +X, two 3-frame clips -------------
        //   clip A: bone0 slides +X by 0..2, bone1 turns 0..90 deg about Z
        //   clip B: bone0 slides +Y by 0..2, nothing else moves
        // Keys are chosen so every sampled phase used below lands on a key or
        // on an exact key midpoint (nlerp at 0.5 == slerp), so the expected
        // positions are closed-form.
        constexpr int kBones = 3;
        constexpr int kFrames = 3;
        SkeletonData* skeleton_data = MakeChainSkeleton(kBones);
        const eastl::string skeleton_path = WriteAsset(*skeleton_data, "pipeline.skeleton.zasset");
        const eastl::string map_path = WriteAsset(*MakeIdentityMap(kBones), "pipeline.animskelmap.zasset");

        std::vector<AnimationChannel> channels_a(kBones, MakeStillChannel(kFrames));
        channels_a[0].position_keys = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(2, 0, 0)};
        channels_a[1].rotation_keys = {Quaternion::IDENTITY, RotZ(45.0f), RotZ(90.0f)};
        const eastl::string clip_a = WriteAsset(*MakeClipAsset(channels_a, kFrames), "pipeline_a.anim.zasset");

        std::vector<AnimationChannel> channels_b(kBones, MakeStillChannel(kFrames));
        channels_b[0].position_keys = {Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(0, 2, 0)};
        const eastl::string clip_b = WriteAsset(*MakeClipAsset(channels_b, kFrames), "pipeline_b.anim.zasset");

        BoneBlendMask* mask_a = Produce<BoneBlendMask>();
        mask_a->skeleton_file_path = skeleton_path;
        mask_a->enabled = {0, 1, 0};
        const eastl::string mask_a_path = WriteAsset(*mask_a, "pipeline_a.mask.zasset");
        BoneBlendMask* mask_b = Produce<BoneBlendMask>();
        mask_b->skeleton_file_path = skeleton_path;
        mask_b->enabled = {1, 0, 1};
        const eastl::string mask_b_path = WriteAsset(*mask_b, "pipeline_b.mask.zasset");

        if (skeleton_path.empty() || map_path.empty() || clip_a.empty() || clip_b.empty() || mask_a_path.empty() ||
            mask_b_path.empty())
        {
            std::printf("FATAL: fixture assets could not be written\n");
            return 77;
        }

        Skeleton skeleton;
        skeleton.BuildSkeleton(*skeleton_data);
        Check(skeleton.GetBonesCount() == kBones, "setup: skeleton builds");
        AnimationResult result;

        // ---- P0: pin the pre-existing scale-doubling bug --------------------
        //
        // Skeleton::ApplyAnimation seeds the per-bone blend accumulator with
        // BonePose::Identity() (scale = 1) instead of a zero scale, then
        // AccumulateBlend ADDS each clip's scale on top and
        // NormalizeBlendAccumulator divides by total_weight (= 1 for a single
        // clip). Position starts at 0 and rotation self-corrects via the
        // normalise, so only SCALE is wrong: a single clip yields scale 2.
        //
        // P0.2 must NOT change this (its contract is "pose bit-identical");
        // this check documents it so a future fix is a deliberate, visible
        // change rather than an accidental drift.
        EvaluatePipeline(MakeBlendState({clip_a}, map_path, {1.0f}, {0.0f}), skeleton, result);
        CheckVec(skeleton.GetBones()[0].GetScale(), Vector3(2.0f, 2.0f, 2.0f), 1.0e-5f,
                 "P0: single clip doubles the root scale (known bug, see note)");
        CheckVec(BonePosition(skeleton, 1), Vector3(2.0f, 0, 0), 1.0e-5f, "P0: the scale bug propagates to child offsets");

        // ---- P1: single clip, root bone slide -------------------------------
        {
            const float phases[] = {0.0f, 0.25f, 0.5f};
            for (int i = 0; i < 3; ++i)
            {
                const float p = phases[i];
                EvaluatePipeline(MakeBlendState({clip_a}, map_path, {1.0f}, {p}), skeleton, result);
                CheckVec(BonePosition(skeleton, 0), Vector3(2.0f * p, 0, 0), 1.0e-5f, "P1: root bone slides +X");
            }
            Check(result.node.size() == static_cast<size_t>(kBones), "P1: one palette entry per bone");
        }

        // ---- P2: two clips, 0.5 / 0.5, root bone averages -------------------
        {
            const float p = 0.5f;
            EvaluatePipeline(MakeBlendState({clip_a, clip_b}, map_path, {0.5f, 0.5f}, {p, p}), skeleton, result);
            CheckVec(BonePosition(skeleton, 0), Vector3(p, p, 0), 1.0e-5f, "P2: root bone averages both slides");
        }

        // ---- P3: masks -------------------------------------------------------
        {
            const float p = 0.5f;
            EvaluatePipeline(MakeBlendState({clip_a, clip_b}, map_path, {1.0f, 1.0f}, {p, p}, {mask_a_path, mask_b_path}),
                             skeleton, result);
            CheckVec(BonePosition(skeleton, 0), Vector3(0, 2.0f * p, 0), 1.0e-5f, "P3: masked root takes only clip B's slide");
        }

        // ---- P5: the pipeline BORROWS clips, it does not copy them -----------
        //
        // The structural proof that the per-frame deep copy is gone. Counting
        // heap allocations instead is not an option here: this test must link
        // ZRuntime (it needs AssetManager), and an executable that overrides
        // global new cannot link ZRuntime (LNK2005 on the allocator), while
        // MSVC Debug containers add _Container_proxy noise on top.
        {
            static_assert(std::is_same_v<decltype(BlendStateWithClipData::blend_clip),
                                         std::vector<const AnimationClip*>>,
                          "blend_clip must borrow clips, not own copies");

            BlendStateWithClipData resolved;
            AnimationSystem::BuildBlendStateWithClipData(
                MakeBlendState({clip_a, clip_b}, map_path, {0.5f, 0.5f}, {0.0f, 0.0f}), resolved);
            Check(resolved.clip_count == 2, "P5: both clips resolve");
            Check(resolved.blend_clip.size() == 2u && resolved.blend_clip[0] != nullptr && resolved.blend_clip[1] != nullptr,
                  "P5: both clip pointers are non-null");
            Check(resolved.blend_clip[0] == AnimationSystem::TryLoadAnimation(clip_a).get(),
                  "P5: clip 0 points AT the cached clip (no copy)");
            Check(resolved.blend_clip[1] == AnimationSystem::TryLoadAnimation(clip_b).get(),
                  "P5: clip 1 points AT the cached clip (no copy)");
            Check(resolved.blend_clip[0] != resolved.blend_clip[1], "P5: two different clips stay distinct");

            // Resolving again must hand back the SAME addresses: a fresh copy
            // (or a reloaded clip) every frame is exactly what P0.2 removes.
            const void* first_0 = resolved.blend_clip[0];
            const void* first_1 = resolved.blend_clip[1];
            AnimationSystem::BuildBlendStateWithClipData(
                MakeBlendState({clip_a, clip_b}, map_path, {0.5f, 0.5f}, {0.5f, 0.5f}), resolved);
            Check(resolved.blend_clip[0] == first_0 && resolved.blend_clip[1] == first_1,
                  "P5: a second resolve reuses the same cached clips");
        }

        // ---- P6: the resolve buffer is reusable and shrinks correctly --------
        {
            BlendStateWithClipData reused;
            AnimationSystem::BuildBlendStateWithClipData(
                MakeBlendState({clip_a, clip_b}, map_path, {0.5f, 0.5f}, {0.0f, 0.0f}), reused);
            Check(reused.clip_count == 2 && reused.blend_clip.size() == 2u, "P6: two clips fill two slots");

            // Same buffer, fewer clips: nothing from the previous resolve may
            // survive, or ApplyAnimation would sample a stale clip.
            AnimationSystem::BuildBlendStateWithClipData(MakeBlendState({clip_a}, map_path, {1.0f}, {0.0f}), reused);
            Check(reused.clip_count == 1, "P6: clip_count shrinks to one");
            Check(reused.blend_clip.size() == 1u && reused.blend_anim_skel_map.size() == 1u &&
                      reused.blend_ratio.size() == 1u && reused.blend_weight.size() == 1u,
                  "P6: every parallel array shrinks with it");
            Check(reused.blend_clip[0] == AnimationSystem::TryLoadAnimation(clip_a).get(),
                  "P6: the surviving slot holds the right clip");
        }

        // ---- B3: steady state does not grow its buffers ----------------------
        //
        // The other half of "no per-frame allocation". Capacity can only grow,
        // so a capacity that stops changing after warm-up means the frame loop
        // stopped allocating -- which is checkable without overriding new.
        {
            BlendStateWithClipData steady;
            const BlendState state = MakeBlendState({clip_a, clip_b}, map_path, {0.5f, 0.5f}, {0.25f, 0.75f});
            Skeleton steady_skeleton;
            steady_skeleton.BuildSkeleton(*skeleton_data);
            AnimationResult steady_result;

            for (int warm = 0; warm < 10; ++warm)
            {
                AnimationSystem::BuildBlendStateWithClipData(state, steady);
                steady_skeleton.ApplyAnimation(steady);
                steady_skeleton.OutputAnimationResult(steady_result);
            }

            const size_t clip_capacity = steady.blend_clip.capacity();
            const size_t map_capacity = steady.blend_anim_skel_map.capacity();
            const size_t weight_capacity = steady.blend_weight.capacity();
            const size_t inner_capacity_0 = steady.blend_weight[0].blend_weight.capacity();
            const size_t inner_capacity_1 = steady.blend_weight[1].blend_weight.capacity();
            const size_t result_capacity = steady_result.node.capacity();

            for (int frame = 0; frame < 1000; ++frame)
            {
                AnimationSystem::BuildBlendStateWithClipData(state, steady);
                steady_skeleton.ApplyAnimation(steady);
                steady_skeleton.OutputAnimationResult(steady_result);
            }

            Check(steady.blend_clip.capacity() == clip_capacity, "B3: blend_clip capacity is stable over 1000 frames");
            Check(steady.blend_anim_skel_map.capacity() == map_capacity, "B3: bone-map capacity is stable");
            Check(steady.blend_weight.capacity() == weight_capacity, "B3: blend_weight capacity is stable");
            Check(steady.blend_weight[0].blend_weight.capacity() == inner_capacity_0 &&
                      steady.blend_weight[1].blend_weight.capacity() == inner_capacity_1,
                  "B3: per-clip bone-weight buffers keep their capacity (resize, not assign)");
            Check(steady_result.node.capacity() == result_capacity, "B3: the palette buffer is stable");
        }

        // ---- P4: a missing clip ----------------------------------------------
        {
            const eastl::string missing = eastl::string((g_TempDir / "does_not_exist.anim.zasset").generic_string().c_str());
            EvaluatePipeline(MakeBlendState({missing, clip_a}, map_path, {1.0f, 1.0f}, {0.0f, 0.0f}), skeleton, result);
            Check(result.node.size() == static_cast<size_t>(kBones), "P4: a missing clip is skipped without crashing");
        }

        // ---- P7a: A/B hash, pipeline level (AnimatorInstance crossfade) -------
        AnimatorController* controller = Produce<AnimatorController>();
        {
            AnimatorParameter speed;
            speed.name = "speed";
            speed.type = static_cast<int>(AnimatorParameterType::Float);
            controller->parameters.push_back(speed);

            AnimatorState idle;
            idle.name = "idle";
            idle.clip_file_path = clip_a;
            idle.anim_skel_map_path = map_path;
            idle.clip_length = 1.0f;
            controller->states.push_back(idle);

            AnimatorState run;
            run.name = "run";
            run.clip_file_path = clip_b;
            run.anim_skel_map_path = map_path;
            run.clip_length = 0.5f;
            run.speed = 1.5f;
            controller->states.push_back(run);

            AnimatorTransition to_run;
            to_run.from_state = 0;
            to_run.to_state = 1;
            AnimatorCondition fast;
            fast.parameter = "speed";
            fast.mode = static_cast<int>(AnimatorConditionMode::Greater);
            fast.threshold = 0.5f;
            to_run.conditions.push_back(fast);
            to_run.duration = 0.25f;
            controller->transitions.push_back(to_run);
        }
        constexpr int kHashFrames = 200;
        constexpr float kDt = 1.0f / 60.0f;
        {
            AnimatorInstance animator;
            std::string error;
            Check(animator.Initialize(*controller, error), "P7a: animator initializes");

            Skeleton hashed_skeleton;
            hashed_skeleton.BuildSkeleton(*skeleton_data);
            Fnv1a64 hash;
            bool saw_crossfade = false;
            for (int frame = 0; frame < kHashFrames; ++frame)
            {
                if (frame == 30)
                {
                    animator.SetFloat("speed", 1.0f);
                }
                animator.Tick(kDt);
                saw_crossfade = saw_crossfade || animator.GetBlendState().clip_count == 2;
                EvaluatePipeline(animator.GetBlendState(), hashed_skeleton, result);
                HashResult(hash, result);
            }
            Check(saw_crossfade, "P7a: the sequence includes a two-clip crossfade");
            std::printf("P7a pipeline hash: %016llx\n", static_cast<unsigned long long>(hash.value));
        }

        // ---- P7b: A/B hash, component level (AnimationComponent::Tick) -------
        {
            const eastl::string controller_path = WriteAsset(*controller, "pipeline.controller.zasset");
            AnimationComponent* component = Produce<AnimationComponent>();
            component->SetSkeletonPath(skeleton_path.c_str());
            component->SetControllerPath(controller_path.c_str());
            component->PostLoadResource(nullptr);
            Check(component->HasController(), "P7b: component loads its controller from disk");

            Fnv1a64 hash;
            for (int frame = 0; frame < kHashFrames; ++frame)
            {
                if (frame == 30)
                {
                    component->SetFloat("speed", 1.0f);
                }
                component->Tick(kDt);
                HashResult(hash, component->GetResult());
            }
            Check(component->GetResult().node.size() == static_cast<size_t>(kBones), "P7b: component produces a palette");
            std::printf("P7b component hash: %016llx\n", static_cast<unsigned long long>(hash.value));
        }

        std::printf("\nAnimation pipeline smoke test: %d check(s), %d failure(s)\n", g_Checks, g_Failures);
        exit_code = g_Failures == 0 ? 0 : 1;
    }

    // Produce() is `new T()` => ObjectManager::Destroy. Assets loaded back through
    // AssetManager are owned by it and are not ours to free.
    for (auto it = g_Produced.rbegin(); it != g_Produced.rend(); ++it)
    {
        GET_SYSTEM(ObjectManager)->Destroy(*it);
    }
    std::filesystem::remove_all(g_TempDir, ec);
    return exit_code;
}
