#include "StreamedBinaryWrite.h"

CachedWriter& StreamedBinaryWrite::Init()
{
    return m_Cache;
}

CachedWriter& StreamedBinaryWrite::Init(const CachedWriter& cachedWriter)
{
    m_Cache = cachedWriter;
    return m_Cache;
}

void StreamedBinaryWrite::Align()
{
    m_Cache.Align4Write();
}