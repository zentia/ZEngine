#include "LMDBDerivedDataCache.h"

#include <algorithm>
#include <cstring>
#include <lmdb.h>
#include <sstream>

namespace Runtime
{
    LMDBDerivedDataCache::LMDBDerivedDataCache()
        : m_Env(nullptr), m_Dbi(0), m_Initialized(false)
    {
    }

    LMDBDerivedDataCache::~LMDBDerivedDataCache()
    {
        Shutdown();
    }

    bool LMDBDerivedDataCache::Initialize(const std::filesystem::path& cache_path, size_t max_size_mb)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (m_Initialized)
        {
            return true;
        }

        m_CachePath = cache_path;

        // 创建缓存目录（如果不存在）
        try
        {
            std::filesystem::create_directories(cache_path);
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return false;
        }

        // 创建 LMDB 环境
        int rc = mdb_env_create(&m_Env);
        if (rc != 0)
        {
            return false;
        }

        // 设置最大数据库大小（MB 转字节）
        size_t max_size_bytes = max_size_mb * 1024 * 1024;
        rc = mdb_env_set_mapsize(m_Env, max_size_bytes);
        if (rc != 0)
        {
            mdb_env_close(m_Env);
            m_Env = nullptr;
            return false;
        }

        // 设置最大数据库数量
        rc = mdb_env_set_maxdbs(m_Env, 1);
        if (rc != 0)
        {
            mdb_env_close(m_Env);
            m_Env = nullptr;
            return false;
        }

        // 打开环境
        std::string path_str = cache_path.string();
        rc = mdb_env_open(m_Env, path_str.c_str(), 0, 0664);
        if (rc != 0)
        {
            mdb_env_close(m_Env);
            m_Env = nullptr;
            return false;
        }

        // 打开数据库
        MDB_txn* txn = nullptr;
        rc = mdb_txn_begin(m_Env, nullptr, 0, &txn);
        if (rc != 0)
        {
            mdb_env_close(m_Env);
            m_Env = nullptr;
            return false;
        }

        rc = mdb_dbi_open(txn, nullptr, 0, &m_Dbi);
        if (rc != 0)
        {
            mdb_txn_abort(txn);
            mdb_env_close(m_Env);
            m_Env = nullptr;
            return false;
        }

        rc = mdb_txn_commit(txn);
        if (rc != 0)
        {
            mdb_dbi_close(m_Env, m_Dbi);
            mdb_env_close(m_Env);
            m_Env = nullptr;
            m_Dbi = 0;
            return false;
        }

        m_Initialized = true;
        return true;
    }

    void LMDBDerivedDataCache::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Initialized)
        {
            return;
        }

        if (m_Env)
        {
            if (m_Dbi != 0)
            {
                mdb_dbi_close(m_Env, m_Dbi);
                m_Dbi = 0;
            }
            mdb_env_close(m_Env);
            m_Env = nullptr;
        }

        m_Initialized = false;
    }

    std::string LMDBDerivedDataCache::SerializeKey(const DDCKey& key) const
    {
        return key.ToString();
    }

    bool LMDBDerivedDataCache::DeserializeKey(const std::string& key_str, DDCKey& out_key) const
    {
        // 解析格式: "cache_type:asset_guid:cache_key"
        size_t pos1 = key_str.find(':');
        if (pos1 == std::string::npos)
        {
            return false;
        }

        size_t pos2 = key_str.find(':', pos1 + 1);
        if (pos2 == std::string::npos)
        {
            return false;
        }

        out_key.cache_type = key_str.substr(0, pos1);
        out_key.asset_guid = key_str.substr(pos1 + 1, pos2 - pos1 - 1);
        out_key.cache_key = key_str.substr(pos2 + 1);
        return true;
    }

    std::vector<uint8_t> LMDBDerivedDataCache::SerializeValue(const DDCValue& value) const
    {
        std::vector<uint8_t> result;

        // 序列化格式: [version(4 bytes)][timestamp(8 bytes)][data_size(8 bytes)][data...]
        uint32_t version = value.version;
        std::time_t timestamp = value.timestamp;
        size_t data_size = value.data.size();

        result.resize(sizeof(version) + sizeof(timestamp) + sizeof(data_size) + data_size);

        size_t offset = 0;
        std::memcpy(result.data() + offset, &version, sizeof(version));
        offset += sizeof(version);

        std::memcpy(result.data() + offset, &timestamp, sizeof(timestamp));
        offset += sizeof(timestamp);

        std::memcpy(result.data() + offset, &data_size, sizeof(data_size));
        offset += sizeof(data_size);

        if (data_size > 0)
        {
            std::memcpy(result.data() + offset, value.data.data(), data_size);
        }

        return result;
    }

    bool LMDBDerivedDataCache::DeserializeValue(const std::vector<uint8_t>& data, DDCValue& out_value) const
    {
        if (data.size() < sizeof(uint32_t) + sizeof(std::time_t) + sizeof(size_t))
        {
            return false;
        }

        size_t offset = 0;

        std::memcpy(&out_value.version, data.data() + offset, sizeof(out_value.version));
        offset += sizeof(out_value.version);

        std::memcpy(&out_value.timestamp, data.data() + offset, sizeof(out_value.timestamp));
        offset += sizeof(out_value.timestamp);

        size_t data_size = 0;
        std::memcpy(&data_size, data.data() + offset, sizeof(data_size));
        offset += sizeof(data_size);

        if (data.size() < offset + data_size)
        {
            return false;
        }

        out_value.data.resize(data_size);
        if (data_size > 0)
        {
            std::memcpy(out_value.data.data(), data.data() + offset, data_size);
        }

        return true;
    }

    bool LMDBDerivedDataCache::get(const DDCKey& key, DDCValue& out_value)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Initialized)
        {
            m_Stats.miss_count++;
            return false;
        }

        std::string key_str = SerializeKey(key);

        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(m_Env, nullptr, MDB_RDONLY, &txn);
        if (rc != 0)
        {
            m_Stats.miss_count++;
            return false;
        }

        MDB_val mdb_key, mdb_data;
        mdb_key.mv_size = key_str.size();
        mdb_key.mv_data = const_cast<char*>(key_str.c_str());

        rc = mdb_get(txn, m_Dbi, &mdb_key, &mdb_data);
        mdb_txn_abort(txn);

        if (rc != 0)
        {
            m_Stats.miss_count++;
            return false;
        }

        // 反序列化数据
        std::vector<uint8_t> value_data(
            static_cast<const uint8_t*>(mdb_data.mv_data),
            static_cast<const uint8_t*>(mdb_data.mv_data) + mdb_data.mv_size);

        if (!DeserializeValue(value_data, out_value))
        {
            m_Stats.miss_count++;
            return false;
        }

        m_Stats.hit_count++;
        return true;
    }

    bool LMDBDerivedDataCache::Put(const DDCKey& key, const DDCValue& value)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Initialized)
        {
            return false;
        }

        std::string key_str = SerializeKey(key);
        std::vector<uint8_t> value_data = SerializeValue(value);

        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(m_Env, nullptr, 0, &txn);
        if (rc != 0)
        {
            return false;
        }

        MDB_val mdb_key, mdb_data;
        mdb_key.mv_size = key_str.size();
        mdb_key.mv_data = const_cast<char*>(key_str.c_str());
        mdb_data.mv_size = value_data.size();
        mdb_data.mv_data = value_data.data();

        rc = mdb_put(txn, m_Dbi, &mdb_key, &mdb_data, 0);
        if (rc != 0)
        {
            mdb_txn_abort(txn);
            return false;
        }

        rc = mdb_txn_commit(txn);
        if (rc != 0)
        {
            return false;
        }

        m_Stats.total_entries++;
        m_Stats.total_size_bytes += value_data.size();
        return true;
    }

    bool LMDBDerivedDataCache::remove(const DDCKey& key)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Initialized)
        {
            return false;
        }

        std::string key_str = SerializeKey(key);

        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(m_Env, nullptr, 0, &txn);
        if (rc != 0)
        {
            return false;
        }

        MDB_val mdb_key;
        mdb_key.mv_size = key_str.size();
        mdb_key.mv_data = const_cast<char*>(key_str.c_str());

        rc = mdb_del(txn, m_Dbi, &mdb_key, nullptr);
        if (rc != 0)
        {
            mdb_txn_abort(txn);
            return false;
        }

        rc = mdb_txn_commit(txn);
        if (rc != 0)
        {
            return false;
        }

        return true;
    }

    bool LMDBDerivedDataCache::exists(const DDCKey& key)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Initialized)
        {
            return false;
        }

        std::string key_str = SerializeKey(key);

        MDB_txn* txn = nullptr;
        int rc = mdb_txn_begin(m_Env, nullptr, MDB_RDONLY, &txn);
        if (rc != 0)
        {
            return false;
        }

        MDB_val mdb_key, mdb_data;
        mdb_key.mv_size = key_str.size();
        mdb_key.mv_data = const_cast<char*>(key_str.c_str());

        rc = mdb_get(txn, m_Dbi, &mdb_key, &mdb_data);
        mdb_txn_abort(txn);

        return rc == 0;
    }

    void LMDBDerivedDataCache::Cleanup()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);

        if (!m_Initialized)
        {
            return;
        }

        // TODO: 实现清理过期缓存的逻辑
        // 可以遍历所有键值对，检查时间戳，删除过期项
    }

    CacheStats LMDBDerivedDataCache::GetStats() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Stats;
    }
}  // namespace Runtime
