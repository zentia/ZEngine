#pragma once
#include "VirtualFileSystemTypes.h"

class LocalFileSystemHandler : public FileSystemHandler
{
public:
    virtual void InitializeFileEntry(FileEntryData& data);
};