#include "Remapper.h"

#include "SerializedObjectIdentifier.h"

int32_t Remapper::GetOrGenerateInstanceID(const SerializedObjectIdentifier& identifier)
{
    if (identifier.serializedFileIndex == -1)
        return 0;
    auto&& inserted = m_SerializedObjectToInstanceID.insert({identifier, 0});
    if (inserted.second)
    {
        IncreaseHighestInstanceID(2);
        inserted.first->second = m_HighestInstanceID;
        m_InstanceIDToSerializedObject.insert({m_HighestInstanceID, identifier});
        return m_HighestInstanceID;
    }

    return inserted.first->second;
}

bool Remapper::InstanceIDToSerializedObjectIdentifier(int32_t instanceID, SerializedObjectIdentifier& identifier)
{
    auto&& i = m_InstanceIDToSerializedObject.find(instanceID);
    if (i == m_InstanceIDToSerializedObject.end())
    {
        identifier.serializedFileIndex = -1;
        identifier.localIdentifierInFile = 0;
        return false;
    }
    identifier = i->second;
    return true;
}

void Remapper::IncreaseHighestInstanceID(int increment)
{
    m_HighestInstanceID += increment;
}