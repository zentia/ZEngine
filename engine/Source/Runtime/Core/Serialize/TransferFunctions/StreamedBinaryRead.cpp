#include "StreamedBinaryRead.h"

void StreamedBinaryRead::ReadDirect(void* data, int byteSize)
{
    m_Cache.Read(data, byteSize);
}

void StreamedBinaryRead::Align()
{
    m_Cache.Align4Read();
}