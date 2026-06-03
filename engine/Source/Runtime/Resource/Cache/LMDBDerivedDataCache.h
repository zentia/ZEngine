#pragma once

#include "DerivedDataCache.h"

#include <filesystem>
#include <mutex>

// Forward declaration for LMDB types
struct MDB_env;
struct MDB_txn;
typedef unsigned int MDB_dbi;

namespace Runtime
{
    // LMDB 实现的派生数据缓存
    class LMDBDerivedDataCache : public IDerivedDataCache
    {
    public:
        LMDBDerivedDataCache();
        ~LMDBDerivedDataCache();

        // 禁止拷贝和赋值
        LMDBDerivedDataCache(const LMDBDerivedDataCache&) = delete;
        LMDBDerivedDataCache& operator=(const LMDBDerivedDataCache&) = delete;

        // 初始化缓存系统
        // cache_path: 缓存数据库文件路径
        // max_size_mb: 最大缓存大小（MB）
        bool Initialize(const std::filesystem::path& cache_path, size_t max_size_mb);

        // 关闭缓存系统
        void Shutdown();

        // IDerivedDataCache 接口实现
        bool get(const DDCKey& key, DDCValue& out_value) override;
        bool Put(const DDCKey& key, const DDCValue& value) override;
        bool remove(const DDCKey& key) override;
        bool exists(const DDCKey& key) override;
        void Cleanup() override;
        CacheStats GetStats() const override;

    private:
        // 序列化 DDCKey 为字符串
        std::string SerializeKey(const DDCKey& key) const;

        // 反序列化字符串为 DDCKey
        bool DeserializeKey(const std::string& key_str, DDCKey& out_key) const;

        // 序列化 DDCValue 为字节数组
        std::vector<uint8_t> SerializeValue(const DDCValue& value) const;

        // 反序列化字节数组为 DDCValue
        bool DeserializeValue(const std::vector<uint8_t>& data, DDCValue& out_value) const;

        MDB_env* m_Env = nullptr;
        MDB_dbi m_Dbi = 0;
        std::filesystem::path m_CachePath;
        bool m_Initialized = false;
        mutable std::mutex m_Mutex;  // 保护并发访问
        mutable CacheStats m_Stats;
    };
}  // namespace Runtime
