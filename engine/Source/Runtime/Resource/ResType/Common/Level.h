#pragma once
#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Math/Vector3.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"

class GameObject;

class LevelRes : public Object
{
    REGISTER_CLASS(LevelRes);
    DECLARE_OBJECT_SERIALIZE();

public:
    Vector3 m_Gravity {0.f, 0.f, -9.8f};
    eastl::string m_CharacterName;

    std::vector<GameObject*> m_Objects;
};