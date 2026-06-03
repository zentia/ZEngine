#pragma once

#include "Runtime/Resource/Asset/VolumeSerializedFileIndex.h"

#include <functional>
#include <stdint.h>

class SerializedObjectIdentifier
{
public:
    uint64_t serializedFileIndex;
    int64_t localIdentifierInFile;

    bool operator==(const SerializedObjectIdentifier& other) const
    {
        return serializedFileIndex == other.serializedFileIndex &&
               localIdentifierInFile == other.localIdentifierInFile;
    }
};

namespace std
{
    template<>
    struct hash<SerializedObjectIdentifier>
    {
        size_t operator()(const SerializedObjectIdentifier& identifier) const
        {
            size_t h1 = std::hash<uint64_t> {}(identifier.serializedFileIndex);
            size_t h2 = std::hash<int64_t> {}(identifier.localIdentifierInFile);
            return h1 ^ (h2 << 1);
        }
    };
}  // namespace std