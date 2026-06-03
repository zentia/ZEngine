#pragma once

#include "Runtime/Core/meta/reflection/Reflection.h"
#include "Runtime/Function/Gas/GameplayAbility.h"
#include "Runtime/Function/Gas/GameplayEffect.h"
#include "Runtime/Function/Gas/GasTypes.h"

// ============================================================================
// GameplayAbilityAsset - 能力资源定义
// ============================================================================

class GameplayAbilityAsset
{
public:
    std::string asset_name;
    Reflection::ReflectionPtr<GameplayAbility> ability;
};

// ============================================================================
// GameplayEffectAsset - 效果资源定义
// ============================================================================

class GameplayEffectAsset
{
public:
    std::string asset_name;
    Reflection::ReflectionPtr<GameplayEffect> effect;
};