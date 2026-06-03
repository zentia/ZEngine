#include "LocalFileSystem.h"

void LocalFileSystemHandler::InitializeFileEntry(FileEntryData& data)
{
    data.accessorHandler = this;
}