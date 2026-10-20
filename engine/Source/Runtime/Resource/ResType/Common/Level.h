#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Level.generated.h"

class GameObject;

// Unity-1:1: a scene asset carries only scene settings. The objects live in
// the serialized file's flat object table as siblings (LevelRes is fileID 1),
// NOT in a nested array owned by the header. Roots are recovered from
// Transform.m_Father == 0, exactly like Unity. See PPTR_DESIGN.md.
ZCLASS()
class LevelRes : public Object
{
    ZGENERATED_BODY(LevelRes)

public:
    ZPROPERTY()
    Vector3 m_Gravity {0.f, 0.f, -9.8f};
    ZPROPERTY()
    eastl::string m_CharacterName;
};