# ZEngine 缓存系统设计文档

## 概述

本文档对比了 Unity 和 Unreal Engine 的缓存系统实现，并为 ZEngine 的缓存系统设计提供建议。

---

## 主流引擎缓存系统对比

### Unity 缓存系统

**实现方式**：使用 **LMDB (Lightning Memory-Mapped Database)**

#### 核心特性

1. **LMDB 优势**：
   - ✅ **高性能**：基于内存映射，读写速度极快
   - ✅ **ACID 事务支持**：保证数据一致性
   - ✅ **零拷贝**：内存映射直接访问磁盘数据
   - ✅ **轻量级**：嵌入式数据库，无需独立服务
   - ✅ **跨平台**：支持 Windows、macOS、Linux
   - ✅ **并发安全**：支持多读单写（MVCC）

2. **Unity 使用场景**：
   - 资产元数据缓存（Library/metadata）
   - 资产导入结果缓存
   - 着色器编译缓存
   - 资源依赖关系缓存

3. **存储位置**：
   - `Library/metadata/` - 元数据数据库
   - `Library/Artifacts/` - 导入结果缓存

#### LMDB 技术细节

```cpp
// LMDB 基本使用示例
MDB_env* env;
mdb_env_create(&env);
mdb_env_set_mapsize(env, 1024 * 1024 * 1024); // 1GB
mdb_env_open(env, "./cache", 0, 0664);

MDB_txn* txn;
MDB_dbi dbi;
mdb_txn_begin(env, NULL, 0, &txn);
mdb_dbi_open(txn, NULL, 0, &dbi);

// 写入
MDB_val key, data;
key.mv_size = sizeof(uint64_t);
key.mv_data = &asset_guid;
data.mv_size = metadata.size();
data.mv_data = metadata.data();
mdb_put(txn, dbi, &key, &data, 0);
mdb_txn_commit(txn);
```

---

### Unreal Engine 缓存系统

**实现方式**：**Derived Data Cache (DDC)** 系统

#### 核心特性

1. **UE4 及更早版本**：
   - 使用**文件系统**作为存储后端
   - 基于文件路径的键值存储
   - 支持本地 DDC 和共享 DDC（网络）

2. **UE5 默认实现**：
   - **Zen 存储**（Zen Storage）作为本地 DDC 的默认实现
   - Zen 是 Epic Games 专门为 UE5 开发的高性能存储系统
   - 针对资源编译和加载进行了深度优化

3. **DDC 存储内容**：
   - 纹理压缩结果
   - 着色器编译缓存
   - 静态网格体构建数据
   - 音效压缩结果
   - 其他派生数据（Derived Data）

4. **存储位置**：
   - 本地 DDC：`%LOCALAPPDATA%/UnrealEngine/Common/DerivedDataCache/`
   - 项目 DDC：`<Project>/Saved/DerivedDataCache/`
   - 共享 DDC：可配置网络路径

#### Zen 存储技术特点

- **高性能**：针对大规模资源加载优化
- **压缩支持**：内置压缩算法
- **增量更新**：只更新变化的数据
- **并发访问**：支持多进程/多线程访问
- **版本管理**：自动处理缓存版本和失效

#### DDC 配置示例

```ini
# BaseEngine.ini
[DerivedDataCache]
Type=FileSystem
Path=%ENGINEDIR%DerivedDataCache
MaxCacheSize=512
MaxFileSize=64
```

---

## ZEngine 缓存系统设计建议

### 设计原则

1. **性能优先**：快速读写，低延迟
2. **可扩展性**：支持大规模项目（100万+ 资产）
3. **数据一致性**：保证缓存数据的正确性
4. **跨平台**：支持 Windows、macOS、Linux
5. **易于维护**：代码简洁，易于调试

### 推荐方案：混合架构

基于 ZEngine 的当前架构和需求，建议采用**混合架构**：

#### 1. 轻量级索引缓存（类似 UE 的 AssetRegistry）

**存储方式**：**二进制文件**（当前已实现）

**用途**：
- AssetRegistry 索引缓存
- 资产路径到 GUID 映射
- 资产类型和基本元数据

**实现**：
- 文件位置：`.zengine/AssetRegistry.cache`
- 格式：自定义二进制格式
- 大小：通常 < 20 MB（100,000 资产）

**优势**：
- ✅ 简单直接，无需额外依赖
- ✅ 快速加载（< 100ms）
- ✅ 版本控制友好
- ✅ 适合小规模数据

#### 2. 派生数据缓存（类似 UE 的 DDC）

**存储方式**：**LMDB**（推荐）或 **文件系统**

**用途**：
- 纹理压缩结果
- 着色器编译缓存
- 模型优化数据
- 音效压缩结果
- 其他派生数据

**推荐实现：LMDB**

**理由**：
1. **性能优异**：内存映射，零拷贝访问
2. **成熟稳定**：Unity 已验证，社区支持好
3. **轻量级**：嵌入式，无需独立服务
4. **事务支持**：保证数据一致性
5. **跨平台**：支持所有主流平台

**备选方案：文件系统**
- 如果不想引入 LMDB 依赖
- 适合简单的键值存储需求
- 实现更简单，但性能略低

#### 3. 运行时缓存（内存缓存）

**存储方式**：**内存中的数据结构**

**用途**：
- 已加载的资产对象
- 渲染资源缓存（纹理、网格等）
- Lumen 表面缓存
- Virtual Texture 页缓存

**实现**：
- 使用 `std::unordered_map` 或自定义哈希表
- LRU 缓存策略
- 内存预算管理

---

## 详细设计

### 1. Derived Data Cache (DDC) 系统

#### 架构设计

```cpp
// engine/source/runtime/resource/cache/derived_data_cache.h
namespace Z
{
    // DDC 键类型
    struct DDCKey
    {
        std::string cache_type;  // "Texture", "Shader", "Mesh", etc.
        std::string asset_guid;  // 资产 GUID
        std::string cache_key;   // 缓存键（包含导入设置哈希）
        
        bool operator==(const DDCKey& other) const;
        std::string toString() const;
    };

    // DDC 值类型
    struct DDCValue
    {
        std::vector<uint8_t> data;
        std::time_t timestamp;
        uint32_t version;
    };

    // DDC 接口
    class IDerivedDataCache
    {
    public:
        virtual ~IDerivedDataCache() = default;
        
        // 获取缓存
        virtual bool get(const DDCKey& key, DDCValue& out_value) = 0;
        
        // 设置缓存
        virtual bool put(const DDCKey& key, const DDCValue& value) = 0;
        
        // 删除缓存
        virtual bool remove(const DDCKey& key) = 0;
        
        // 检查缓存是否存在
        virtual bool exists(const DDCKey& key) = 0;
        
        // 清理过期缓存
        virtual void cleanup() = 0;
        
        // 获取缓存统计信息
        virtual CacheStats getStats() const = 0;
    };

    // LMDB 实现
    class LMDBDerivedDataCache : public IDerivedDataCache
    {
    public:
        bool initialize(const std::filesystem::path& cache_path, size_t max_size_mb);
        void shutdown();
        
        bool get(const DDCKey& key, DDCValue& out_value) override;
        bool put(const DDCKey& key, const DDCValue& value) override;
        bool remove(const DDCKey& key) override;
        bool exists(const DDCKey& key) override;
        void cleanup() override;
        CacheStats getStats() const override;
        
    private:
        MDB_env* m_env = nullptr;
        MDB_dbi m_dbi = 0;
        std::filesystem::path m_cache_path;
    };

    // 文件系统实现（备选）
    class FileSystemDerivedDataCache : public IDerivedDataCache
    {
    public:
        bool initialize(const std::filesystem::path& cache_path);
        
        bool get(const DDCKey& key, DDCValue& out_value) override;
        bool put(const DDCKey& key, const DDCValue& value) override;
        bool remove(const DDCKey& key) override;
        bool exists(const DDCKey& key) override;
        void cleanup() override;
        CacheStats getStats() const override;
        
    private:
        std::filesystem::path getCacheFilePath(const DDCKey& key) const;
        std::filesystem::path m_cache_root;
    };
}
```

#### 使用示例

```cpp
// 初始化 DDC
auto ddc = std::make_unique<LMDBDerivedDataCache>();
ddc->initialize(".zengine/DDC", 1024); // 1GB 最大大小

// 缓存纹理压缩结果
DDCKey key;
key.cache_type = "Texture";
key.asset_guid = texture_asset->getGUID();
key.cache_key = generateCacheKey(texture_import_settings);

DDCValue value;
if (ddc->get(key, value))
{
    // 使用缓存的压缩数据
    loadCompressedTexture(value.data);
}
else
{
    // 压缩纹理
    auto compressed_data = compressTexture(texture_data, import_settings);
    
    // 保存到缓存
    value.data = compressed_data;
    value.timestamp = std::time(nullptr);
    value.version = TEXTURE_CACHE_VERSION;
    ddc->put(key, value);
}
```

### 2. 缓存键生成策略

```cpp
namespace Z
{
    // 生成缓存键（包含所有影响结果的参数）
    std::string generateTextureCacheKey(const TextureImportSettings& settings)
    {
        std::stringstream ss;
        ss << "format:" << settings.format
           << "_compression:" << settings.compression
           << "_mipmaps:" << settings.generate_mipmaps
           << "_srgb:" << settings.srgb
           << "_size:" << settings.max_size;
        
        // 计算哈希值
        return hashString(ss.str());
    }
    
    std::string generateShaderCacheKey(const ShaderCompileSettings& settings)
    {
        std::stringstream ss;
        ss << "source_hash:" << settings.source_hash
           << "_defines:" << settings.defines
           << "_target:" << settings.target;
        
        return hashString(ss.str());
    }
}
```

### 3. 缓存版本管理

```cpp
namespace Z
{
    // 缓存版本常量
    constexpr uint32_t TEXTURE_CACHE_VERSION = 1;
    constexpr uint32_t SHADER_CACHE_VERSION = 2;
    constexpr uint32_t MESH_CACHE_VERSION = 1;
    
    // 检查缓存版本
    bool isCacheValid(const DDCValue& value, uint32_t expected_version)
    {
        return value.version == expected_version;
    }
    
    // 自动清理过期缓存
    void DerivedDataCache::cleanup()
    {
        // 清理版本不匹配的缓存
        // 清理过期的缓存（超过一定时间未使用）
        // 清理超过大小限制的缓存
    }
}
```

### 4. 缓存配置

```cpp
// engine/configs/development/ZEditor.ini
[DerivedDataCache]
Type=LMDB
Path=.zengine/DDC
MaxSizeMB=1024
EnableCompression=true
CleanupDays=30

[AssetRegistry]
CachePath=.zengine/AssetRegistry.cache
EnableIncrementalUpdate=true
```

---

## 实现建议

### 阶段 1：基础 DDC 系统（文件系统实现）

**目标**：快速实现，验证架构

1. 实现 `FileSystemDerivedDataCache`
2. 支持基本的 get/put/remove 操作
3. 简单的缓存键生成
4. 集成到资产导入管道

**优势**：
- 无需外部依赖
- 实现简单
- 易于调试

### 阶段 2：LMDB 集成（性能优化）

**目标**：提升性能，支持大规模项目

1. 集成 LMDB 库（作为第三方依赖）
2. 实现 `LMDBDerivedDataCache`
3. 性能测试和优化
4. 支持事务和并发访问

**优势**：
- 高性能
- 事务支持
- 适合大规模项目

### 阶段 3：高级特性

**目标**：完善功能，提升用户体验

1. 缓存压缩
2. 增量更新
3. 缓存统计和监控
4. 自动清理策略
5. 共享 DDC 支持（网络）

---

## 性能对比

| 特性 | 文件系统 | LMDB | Zen Storage |
|------|---------|------|-------------|
| **读取性能** | 中等 | 高 | 极高 |
| **写入性能** | 中等 | 高 | 极高 |
| **并发访问** | 有限 | 优秀 | 优秀 |
| **事务支持** | 无 | 有 | 有 |
| **实现复杂度** | 低 | 中 | 高 |
| **依赖** | 无 | LMDB | 内置 |
| **适用规模** | 小-中 | 中-大 | 超大 |

---

## 总结和建议

### 推荐方案

1. **AssetRegistry 缓存**：继续使用二进制文件（当前实现）
   - 简单高效，满足需求

2. **派生数据缓存（DDC）**：采用 **LMDB**
   - 性能优异，Unity 已验证
   - 适合 ZEngine 的规模
   - 实现复杂度适中

3. **运行时缓存**：使用内存数据结构
   - 已有实现，继续优化

### 实施步骤

1. **短期**（1-2 周）：
   - 实现文件系统版本的 DDC
   - 集成到纹理导入管道
   - 验证架构设计

2. **中期**（1-2 月）：
   - 集成 LMDB
   - 实现完整的 DDC 系统
   - 性能测试和优化

3. **长期**（3-6 月）：
   - 添加高级特性（压缩、统计等）
   - 支持共享 DDC
   - 完善文档和工具

### 注意事项

1. **缓存失效**：确保缓存键包含所有影响结果的参数
2. **版本管理**：当数据格式变化时，需要更新版本号
3. **清理策略**：定期清理过期和无效缓存
4. **错误处理**：缓存失败时应该回退到重新生成
5. **跨平台**：确保 LMDB 在所有目标平台正常工作

---

## 参考资料

- [LMDB 官方文档](https://www.lmdb.tech/doc/)
- [Unity AssetDatabase 文档](https://docs.unity3d.com/ScriptReference/AssetDatabase.html)
- [Unreal Engine DDC 文档](https://docs.unrealengine.com/en-US/Engine/Basics/DerivedDataCache/)
- [Zen Storage 介绍](https://dev.epicgames.com/documentation/en-us/unreal-engine/zen-storage-in-unreal-engine)

