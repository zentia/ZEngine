#pragma once

#include <stdint.h>

class VolumeSerializedFileIndex
{
public:
    VolumeSerializedFileIndex(const uint16_t volumeID, uint64_t fileIndex)
        : assetsVolumeID(volumeID), serializedFileIndex(fileIndex)
    {
    }
    uint16_t assetsVolumeID;
    uint64_t serializedFileIndex;
};