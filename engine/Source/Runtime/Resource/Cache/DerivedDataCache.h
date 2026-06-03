#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace Runtime
{
    // DDC 键类型
    struct DDCKey
    {
        std::string cache_type;  // "Texture", "Shader", "Mesh", etc.
        std::string asset_guid;  // 资产 GUID
        std::string cache_key;   // 缓存键（包含导入设置哈希）

        bool operator==(const DDCKey& other) const;
        std::string ToString() const;
    };

    // DDC 值类型
    struct DDCValue
    {
        std::vector<uint8_t> data;
        std::time_t timestamp;
        uint32_t version;
    };

    // 缓存统计信息
    struct CacheStats
    {
        size_t total_entries = 0;
        size_t total_size_bytes = 0;
        size_t hit_count = 0;
        size_t miss_count = 0;
    };

    // DDC 接口
    class IDerivedDataCache
    {
    public:
        virtual ~IDerivedDataCache() = default;

        // 获取缓存
        virtual bool get(const DDCKey& key, DDCValue& out_value) = 0;

        // 设置缓存
        virtual bool Put(const DDCKey& key, const DDCValue& value) = 0;

        // 删除缓存
        virtual bool remove(const DDCKey& key) = 0;

        // 检查缓存是否存在
        virtual bool exists(const DDCKey& key) = 0;

        // 清理过期缓存
        virtual void Cleanup() = 0;

        // 获取缓存统计信息
        virtual CacheStats GetStats() const = 0;
    };
}  // namespace Runtime
