#include "DerivedDataCache.h"

namespace Runtime
{
    bool DDCKey::operator==(const DDCKey& other) const
    {
        return cache_type == other.cache_type &&
               asset_guid == other.asset_guid &&
               cache_key == other.cache_key;
    }

    std::string DDCKey::ToString() const
    {
        return cache_type + ":" + asset_guid + ":" + cache_key;
    }
}  // namespace Runtime
