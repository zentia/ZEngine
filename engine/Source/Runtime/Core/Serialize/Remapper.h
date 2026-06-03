#pragma once

#include "SerializedObjectIdentifier.h"

#include <stdint.h>
#include <unordered_map>
class Remapper
{
public:
    int32_t GetOrGenerateInstanceID(const SerializedObjectIdentifier& identifier);
    bool InstanceIDToSerializedObjectIdentifier(int32_t instanceID, SerializedObjectIdentifier& identifier);
    void IncreaseHighestInstanceID(int increment);

private:
    std::unordered_map<SerializedObjectIdentifier, int32_t> m_SerializedObjectToInstanceID;
    std::unordered_map<int32_t, SerializedObjectIdentifier> m_InstanceIDToSerializedObject;
    int m_HighestInstanceID;
};