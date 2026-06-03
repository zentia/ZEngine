#pragma once

enum SerializedFileLoadError
{
    kSerializedFileLoadError_None = 0,
    kSerializedFileLoadError_InvalidHeader,
    kSerializedFileLoadError_InvalidMetadata,
};
