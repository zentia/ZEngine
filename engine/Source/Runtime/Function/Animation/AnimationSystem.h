#pragma once

#include "Runtime/Resource/ResType/Data/AnimSkelMap.h"
#include "Runtime/Resource/ResType/Data/AnimationClip.h"
#include "Runtime/Resource/ResType/Data/BlendState.h"
#include "Runtime/Resource/ResType/Data/BoneBlendMask.h"
#include "Runtime/Resource/ResType/Data/SkeletonData.h"

#include <map>
#include <memory>

class AnimationSystem
{
private:
    static std::map<eastl::string, SkeletonData*> m_SkeletonDefinitionCache;
    static std::map<eastl::string, std::shared_ptr<AnimationClip>> m_AnimationDataCache;
    static std::map<eastl::string, PPtr<AnimSkelMap>> m_AnimationSkeletonMapCache;
    static std::map<eastl::string, BoneBlendMask*> m_SkeletonMaskCache;

    // ---- single-lookup cache accessors (P0.2) -------------------------------
    //
    // The `TryLoad*` functions below are the public, by-value-returning API.
    // GetBlendStateWithClipData used to call one of them to prime the cache and
    // then `find()` the same key again, i.e. two std::map<eastl::string>
    // lookups per path per frame. These helpers do it in one, and -- like
    // `TryLoad*` -- cache a failed load so a missing asset is not re-loaded
    // every frame.
    static const AnimationClip* FindOrLoadClip(const eastl::string& file_path);
    static const PPtr<AnimSkelMap>& FindOrLoadSkelMap(const eastl::string& file_path);
    static BoneBlendMask* FindOrLoadMask(const eastl::string& file_path);
    static SkeletonData* FindOrLoadSkeleton(const eastl::string& file_path);

public:
    // Taken by const reference: these are called per clip per frame, and an
    // eastl::string by value is a heap allocation once it outgrows its SSO
    // buffer (asset paths always do).
    static SkeletonData* TryLoadSkeleton(const eastl::string& file_path);
    static std::shared_ptr<AnimationClip> TryLoadAnimation(const eastl::string& file_path);
    static AnimSkelMap* TryLoadAnimationSkeletonMap(const eastl::string& file_path);
    static BoneBlendMask* TryLoadSkeletonMask(const eastl::string& file_path);
    static BlendStateWithClipData GetBlendStateWithClipData(const BlendState& blend_state);

    AnimationSystem() = default;
};