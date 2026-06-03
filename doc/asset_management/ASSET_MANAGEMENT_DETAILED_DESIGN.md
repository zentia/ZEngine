# ZEngine 资源管理系统详细设计文档

> ⚠️ **状态：早期设计草稿（pre-Route-B）**
>
> 本文档（与 `ASSET_MANAGEMENT_GUIDE.md`）记录的是 ZEngine 资产管线**最初
> 的设计草稿**，不是当前代码实现。请勿据此调用 API。
>
> 文中所有 `AssetFile`（`saveAsset` / `loadAsset` / `loadMetadata` /
> `isValidAssetFile` 等）成员均**从未实现完成**——模板体在原型期被全部
> 注释掉，`writeMetadata` / `readMetadata` 是 `return true;` 空实现，
> `isValidAssetFile` / `loadHeader` 零调用方。整个类已在 **P2 #9** 中删除。
>
> 当前真实路径（**Route B**，详见 `doc/BINDLESS_TEXTURE_PATH.md` PR10）：
>
> | 本文档描述 | 实际实现 |
> |---|---|
> | `AssetFile::saveAsset(...)` | `AssetManager::WriteObjectToDiskThreadSafe(path, *object)` → `SerializedFile` |
> | `AssetFile::loadAsset<T>(...)` | `AssetBundle::Get<T>(path)` → `SerializedFile` |
> | `AssetFile::loadMetadata(...)` | `AssetRegistry::scanSingleAsset` 直接 `ifstream::read(AssetFileHeader)` |
> | "元数据段"（导入设置 / 依赖 / custom_metadata） | 当前 `AssetFileHeader.metadata_offset/size = 0`；元数据段尚未启用，等真要落地时应在 `SerializedFile` 层扩展，而不是恢复 `AssetFile` |
>
> 本文档保留作为设计意图与跨引擎对比的存档；查阅当前实现请直接读源码、
> `doc/BINDLESS_TEXTURE_PATH.md`、`doc/asset_management/AUTO_IMPORT_AND_FILE_WATCHER.md`，
> 或 `engine/Source/Runtime/asset/asset_file.h` 顶部的注释块。

## 目录

1. [系统架构概览](#系统架构概览)
2. [核心组件详细设计](#核心组件详细设计)
3. [数据流和交互](#数据流和交互)
4. [文件格式规范](#文件格式规范)
5. [线程安全和并发](#线程安全和并发)
6. [错误处理和恢复](#错误处理和恢复)
7. [性能优化策略](#性能优化策略)
8. [API 参考](#api-参考)
9. [实现示例](#实现示例)

---

## 系统架构概览

### 整体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      Editor Layer                            │
├─────────────────────────────────────────────────────────────┤
│  ProjectBrowser  │  Inspector  │  AssetImporter UI          │
└──────────┬───────────┬───────────┬──────────────────────────┘
           │           │           │
           └───────────┴───────────┘
                      │
           ┌──────────▼──────────┐
           │ EditorAssetService   │  ← 统一编辑器资产 API
           └──────────┬───────────┘
                      │
        ┌─────────────┼─────────────┐
        │             │             │
┌───────▼──────┐ ┌───▼──────┐ ┌───▼──────────┐
│AssetRegistry │ │AssetImport│ │FileSystem    │
│              │ │Manager    │ │Watcher       │
└──────┬───────┘ └──────────┘ └───────────────┘
       │
       │
┌──────▼──────────────────────────────────────┐
│          Runtime Layer                      │
├─────────────────────────────────────────────┤
│  AssetManager  │  AssetFile                 │
│  (运行时加载)   │  (.zasset 读写)            │
└─────────────────────────────────────────────┘
```

### 核心设计原则

1. **分离关注点**：
   - **编辑器层**：资产管理、导入、元数据编辑
   - **运行时层**：资产加载、序列化/反序列化

2. **轻量级索引**：
   - AssetRegistry 只存储最小索引信息
   - 完整元数据存储在 .zasset 文件中

3. **按需加载**：
   - 启动时只加载索引
   - 元数据和资产数据按需从文件加载

4. **异步优先**：
   - 资产扫描在后台线程执行
   - 不阻塞编辑器 UI

---

## 核心组件详细设计

### 1. AssetFile（资产文件读写）

**职责**：提供 .zasset 文件的读写接口，处理文件格式的序列化/反序列化。

#### 类定义

```cpp
// engine/source/runtime/asset/asset_file.h
namespace Z
{
    // .zasset 文件魔数
    constexpr uint32_t k_zasset_magic = 0x5A415353; // "ZASS"
    constexpr uint32_t k_zasset_version = 1;

    // 文件头结构（固定大小，便于快速读取）
    struct AssetFileHeader
    {
        uint32_t magic;              // 文件魔数 "ZASS"
        uint32_t version;            // 文件版本号
        char     guid[37];           // GUID (36 字符 + '\0')
        char     asset_type[64];     // 资产类型名称（固定长度）
        uint64_t metadata_offset;    // 元数据在文件中的偏移
        uint64_t metadata_size;       // 元数据大小（字节）
        uint64_t data_offset;        // 数据在文件中的偏移
        uint64_t data_size;          // 数据大小（字节）
        uint64_t reserved[4];        // 保留字段，用于未来扩展
    };
    static_assert(sizeof(AssetFileHeader) == 256, "AssetFileHeader must be 256 bytes");

    // 元数据结构
    struct AssetMetadata
    {
        std::string                                  guid;                 // GUID（冗余，方便访问）
        std::string                                  source_file_path;     // 原始资产路径（可选）
        std::filesystem::file_time_type              source_file_time;    // 原始文件时间戳
        Json                                         import_settings;      // 导入设置（JSON 格式）
        std::vector<std::string>                     dependencies;        // 依赖的其他资产 GUID
        std::unordered_map<std::string, std::string> custom_metadata;     // 自定义元数据（标签、作者等）
        
        // 序列化支持
        void serialize(Json& json) const;
        void deserialize(const Json& json);
    };

    class AssetFile
    {
    public:
        // 从 .zasset 文件加载元数据（不加载实际数据）
        // 性能：只读取文件头 + 元数据部分，不读取数据部分
        bool loadMetadata(const std::filesystem::path& asset_path, AssetMetadata& out_metadata);

        // 从 .zasset 文件加载完整资产（包括数据）
        template<typename AssetType>
        bool loadAsset(const std::filesystem::path& asset_path, AssetType& out_asset);

        // 保存资产到 .zasset 文件
        template<typename AssetType>
        bool saveAsset(const AssetType& asset, 
                      const AssetMetadata& metadata, 
                      const std::filesystem::path& asset_path);

        // 检查文件是否为有效的 .zasset 文件
        static bool isValidAssetFile(const std::filesystem::path& asset_path);

        // 获取文件头（不加载完整元数据）
        static bool loadHeader(const std::filesystem::path& asset_path, AssetFileHeader& out_header);

    private:
        // 内部辅助方法
        bool readHeader(std::ifstream& file, AssetFileHeader& header);
        bool writeHeader(std::ofstream& file, const AssetFileHeader& header);
        bool readMetadata(std::ifstream& file, const AssetFileHeader& header, AssetMetadata& metadata);
        bool writeMetadata(std::ofstream& file, const AssetMetadata& metadata);
    };
}
```

#### 文件格式布局

```
.zasset 文件结构：
┌─────────────────────────────────┐
│  AssetFileHeader (256 bytes)        │ ← 固定大小，快速读取
│  - magic, version, guid, type        │
│  - metadata_offset, metadata_size    │
│  - data_offset, data_size            │
├─────────────────────────────────┤
│  Metadata Section (JSON)             │ ← 可变大小
│  - GUID, source_file_path            │
│  - import_settings                   │
│  - dependencies (GUID list)            │
│  - custom_metadata                    │
├─────────────────────────────────┤
│  Asset Data Section (Binary)         │ ← 可变大小
│  - 序列化的资产数据                   │
│  - 压缩后的纹理、模型等                │
└─────────────────────────────────┘
```

#### 实现要点

1. **快速元数据读取**：
   - 文件头固定大小（256 字节），可以快速读取
   - 元数据部分使用 JSON 格式，便于调试和版本控制
   - 只读取需要的部分，不加载完整文件

2. **数据部分压缩**：
   - 资产数据可以使用压缩（如 zstd）
   - 压缩信息存储在元数据中

3. **版本兼容性**：
   - 文件头包含版本号
   - 支持向后兼容的元数据迁移

---

### 2. AssetRegistry（资产注册表）

**职责**：维护资产索引，提供快速查询接口。

#### 类定义

```cpp
// engine/source/editor/asset_registry/asset_registry.h
namespace Z
{
    // 资产索引条目（轻量级，只存储索引信息）
    struct AssetIndexEntry
    {
        std::string                      asset_path;      // .zasset 文件路径（相对路径）
        std::string                      asset_type;      // 资产类型（通过反射获取）
        std::string                      guid;            // 唯一标识符
        std::filesystem::file_time_type  last_modified;   // 最后修改时间
        uint64_t                         file_size;       // 文件大小（字节）
        
        // 序列化支持（用于缓存文件）
        void serialize(Json& json) const;
        void deserialize(const Json& json);
    };

    // 扫描状态
    enum class ScanStatus
    {
        Idle,           // 空闲
        Scanning,        // 正在扫描
        Completed,      // 扫描完成
        Failed          // 扫描失败
    };

    // 扫描进度回调
    using ScanProgressCallback = std::function<void(float progress, size_t scanned, size_t total)>;

    class AssetRegistry
    {
    public:
        // 初始化：从缓存文件加载，然后增量更新
        // asset_folder: 资产文件夹路径（如 "Content/"）
        // cache_path: 缓存文件路径（如 ".zengine/AssetRegistry.cache"）
        void initialize(const std::filesystem::path& asset_folder, 
                       const std::filesystem::path& cache_path);

        // 同步扫描所有 .zasset 文件（阻塞调用）
        void scanAssets(const std::filesystem::path& asset_folder);

        // 异步扫描所有 .zasset 文件（非阻塞）
        void scanAssetsAsync(const std::filesystem::path& asset_folder);

        // 增量更新：只扫描变化的 .zasset 文件
        void incrementalUpdate(const std::filesystem::path& asset_folder);

        // 保存缓存到磁盘
        void saveCache(const std::filesystem::path& cache_path);

        // 从缓存文件加载
        bool loadCache(const std::filesystem::path& cache_path);

        // 查询资产
        std::vector<AssetIndexEntry> findAssets(const std::string& filter) const;
        AssetIndexEntry* getAssetIndex(const std::string& asset_path) const;
        AssetIndexEntry* getAssetIndexByGUID(const std::string& guid) const;

        // 获取依赖关系（需要从 .zasset 文件加载元数据）
        std::vector<std::string> getDependencies(const std::string& asset_path) const;
        std::vector<std::string> getReferencers(const std::string& asset_path) const;

        // 资产变化处理
        void refreshAsset(const std::string& asset_path);
        void removeAsset(const std::string& asset_path);
        void addAsset(const std::string& asset_path);

        // 扫描状态
        ScanStatus getScanStatus() const { return m_scan_status; }
        bool isScanning() const { return m_scan_status == ScanStatus::Scanning; }
        float getScanProgress() const { return m_scan_progress; }
        size_t getScannedCount() const { return m_scanned_count; }
        size_t getTotalCount() const { return m_total_count; }

        // 设置扫描进度回调
        void setScanProgressCallback(ScanProgressCallback callback);

        // 等待扫描完成
        void waitForScanComplete();

        // 线程安全访问
        std::shared_lock<std::shared_mutex> acquireReadLock() const;
        std::unique_lock<std::shared_mutex> acquireWriteLock();

    private:
        // 内部扫描方法
        void scanAssetsInternal(const std::filesystem::path& asset_folder, bool incremental);
        AssetIndexEntry scanSingleAsset(const std::filesystem::path& asset_path);

        // 索引存储
        std::unordered_map<std::string, AssetIndexEntry>     m_asset_map;      // path -> index
        std::unordered_map<std::string, std::string>        m_guid_to_path;   // guid -> path
        std::unordered_map<std::string, std::vector<std::string>> m_dependency_map;  // path -> dependencies (GUIDs)
        std::unordered_map<std::string, std::vector<std::string>> m_referencer_map;  // path -> referencers (paths)

        // 扫描状态
        ScanStatus              m_scan_status = ScanStatus::Idle;
        float                   m_scan_progress = 0.0f;
        size_t                  m_scanned_count = 0;
        size_t                  m_total_count = 0;
        std::thread             m_scan_thread;
        ScanProgressCallback    m_progress_callback;

        // 线程安全
        mutable std::shared_mutex m_mutex;  // 读写锁，支持并发读取
    };
}
```

#### 缓存文件格式

```cpp
// AssetRegistry.cache 文件格式（JSON，便于调试和版本控制）
{
    "version": 1,
    "cache_time": "2024-01-01T00:00:00Z",
    "asset_count": 100000,
    "assets": [
        {
            "path": "Content/texture.zasset",
            "type": "Texture2D",
            "guid": "12345678-1234-1234-1234-123456789abc",
            "last_modified": "2024-01-01T00:00:00Z",
            "file_size": 1024000
        },
        // ... 更多资产
    ]
}
```

#### 增量更新算法

```cpp
void AssetRegistry::incrementalUpdate(const std::filesystem::path& asset_folder)
{
    auto lock = acquireWriteLock();
    
    // 1. 收集所有 .zasset 文件
    std::vector<std::filesystem::path> asset_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(asset_folder))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".zasset")
        {
            asset_files.push_back(entry.path());
        }
    }
    
    m_total_count = asset_files.size();
    m_scanned_count = 0;
    
    // 2. 检查每个文件
    for (const auto& asset_path : asset_files)
    {
        std::string relative_path = getRelativePath(asset_folder, asset_path);
        
        // 检查是否已存在
        auto it = m_asset_map.find(relative_path);
        if (it != m_asset_map.end())
        {
            // 检查时间戳是否变化
            auto current_time = std::filesystem::last_write_time(asset_path);
            if (current_time == it->second.last_modified)
            {
                // 未变化，跳过
                m_scanned_count++;
                continue;
            }
        }
        
        // 文件新增或变化，重新扫描
        AssetIndexEntry entry = scanSingleAsset(asset_path);
        if (!entry.guid.empty())
        {
            m_asset_map[relative_path] = entry;
            m_guid_to_path[entry.guid] = relative_path;
        }
        
        m_scanned_count++;
        m_scan_progress = static_cast<float>(m_scanned_count) / m_total_count;
        
        if (m_progress_callback)
        {
            m_progress_callback(m_scan_progress, m_scanned_count, m_total_count);
        }
    }
    
    // 3. 检查已删除的文件
    std::vector<std::string> to_remove;
    for (const auto& [path, entry] : m_asset_map)
    {
        std::filesystem::path full_path = asset_folder / path;
        if (!std::filesystem::exists(full_path))
        {
            to_remove.push_back(path);
        }
    }
    
    for (const auto& path : to_remove)
    {
        removeAsset(path);
    }
    
    m_scan_status = ScanStatus::Completed;
}
```

---

### 3. AssetImportManager（资产导入管理器）

**职责**：管理资产导入器，将原始资产转换为 .zasset 文件。

#### 类定义

```cpp
// engine/source/editor/asset_pipeline/asset_importer.h
namespace Z
{
    // 导入设置基类
    struct AssetImporterSettings
    {
        bool generate_guid = true;  // 是否生成新 GUID
        std::string custom_guid;    // 自定义 GUID（可选）
        
        virtual Json toJson() const = 0;
        virtual void fromJson(const Json& json) = 0;
    };

    // 资产导入器接口
    class AssetImporter
    {
    public:
        virtual ~AssetImporter() = default;
        
        // 检查是否可以导入该文件类型
        virtual bool canImport(const std::filesystem::path& file_path) const = 0;
        
        // 获取支持的文件扩展名
        virtual std::vector<std::string> getSupportedExtensions() const = 0;
        
        // 导入原始资产，生成 .zasset 文件
        virtual bool import(const std::filesystem::path& source_path,
                           const std::filesystem::path& output_path,
                           const AssetImporterSettings& import_settings,
                           AssetMetadata& out_metadata) = 0;
        
        // 重新导入（源文件变化时）
        virtual bool reimport(const std::filesystem::path& zasset_path,
                             const AssetImporterSettings& import_settings) = 0;
        
        // 获取默认导入设置
        virtual std::unique_ptr<AssetImporterSettings> getDefaultSettings() const = 0;
    };

    // 导入管理器
    class AssetImportManager
    {
    public:
        // 注册导入器
        void registerImporter(std::shared_ptr<AssetImporter> importer);
        
        // 根据文件扩展名查找导入器
        std::shared_ptr<AssetImporter> findImporter(const std::filesystem::path& file_path) const;
        
        // 导入原始资产，生成 .zasset 文件
        // source_path: 原始资产路径（如 texture.png）
        // output_path: 输出的 .zasset 文件路径（如 texture.zasset）
        // import_settings: 导入设置（可选，使用默认设置）
        bool importAsset(const std::filesystem::path& source_path,
                        const std::filesystem::path& output_path,
                        const AssetImporterSettings* import_settings = nullptr);
        
        // 自动检测原始资产并导入（扫描 Source 目录）
        void importSourceAssets(const std::filesystem::path& source_folder,
                               const std::filesystem::path& output_folder);
        
        // 批量导入
        bool importAssets(const std::vector<std::filesystem::path>& source_paths,
                         const std::filesystem::path& output_folder,
                         const AssetImporterSettings* import_settings = nullptr);
        
        // 获取所有注册的导入器
        std::vector<std::shared_ptr<AssetImporter>> getAllImporters() const;

    private:
        std::vector<std::shared_ptr<AssetImporter>> m_importers;
        std::mutex m_mutex;  // 保护导入器列表
    };
}
```

#### 导入器示例：TextureImporter

```cpp
// engine/source/editor/asset_pipeline/texture_importer.h
namespace Z
{
    // 纹理导入设置
    struct TextureImporterSettings : public AssetImporterSettings
    {
        enum class Format
        {
            RGBA8,
            RGB8,
            RGBA16F,
            BC7,      // 压缩格式
            BC1,      // DXT1
            BC3       // DXT5
        };
        
        Format format = Format::RGBA8;
        bool generate_mipmaps = true;
        bool sRGB = true;
        int max_size = 4096;  // 最大尺寸
        
        Json toJson() const override;
        void fromJson(const Json& json) override;
    };

    class TextureImporter : public AssetImporter
    {
    public:
        bool canImport(const std::filesystem::path& file_path) const override;
        std::vector<std::string> getSupportedExtensions() const override;
        bool import(const std::filesystem::path& source_path,
                   const std::filesystem::path& output_path,
                   const AssetImporterSettings& import_settings,
                   AssetMetadata& out_metadata) override;
        bool reimport(const std::filesystem::path& zasset_path,
                     const AssetImporterSettings& import_settings) override;
        std::unique_ptr<AssetImporterSettings> getDefaultSettings() const override;

    private:
        bool processTexture(const Image& source_image,
                           const TextureImporterSettings& settings,
                           Image& out_image);
    };
}
```

---

### 4. EditorAssetService（编辑器资产服务）

**职责**：提供统一的编辑器资产 API，连接各个组件。

#### 类定义

```cpp
// engine/source/editor/editor_asset_service/editor_asset_service.h
namespace Z
{
    class EditorAssetService
    {
    public:
        // 初始化（在编辑器启动时调用）
        void initialize();
        
        // 关闭（在编辑器关闭时调用）
        void shutdown();
        
        // ========== 资产查询（使用 AssetRegistry） ==========
        
        // 查找资产（只返回索引信息，不加载完整元数据）
        std::vector<AssetIndexEntry> findAssets(const std::string& filter) const;
        AssetIndexEntry* getAssetIndex(const std::string& asset_path) const;
        AssetIndexEntry* getAssetIndexByGUID(const std::string& guid) const;
        
        // 加载资产元数据（从 .zasset 文件加载，不加载实际数据）
        bool loadAssetMetadata(const std::string& asset_path, AssetMetadata& out_metadata) const;
        
        // ========== 资产加载（使用 AssetManager） ==========
        
        // 加载资产（加载完整资产数据）
        template<typename AssetType>
        bool loadAsset(const std::string& asset_path, AssetType& out_asset) const
        {
            return g_runtime_global_context.m_asset_manager->loadAsset(asset_path, out_asset);
        }
        
        // ========== 资产保存 ==========
        
        // 保存资产到 .zasset 文件
        template<typename AssetType>
        bool saveAsset(const AssetType& asset, 
                      const AssetMetadata& metadata, 
                      const std::string& asset_path) const
        {
            AssetFile asset_file;
            bool success = asset_file.saveAsset(asset, metadata, asset_path);
            if (success)
            {
                m_asset_registry.refreshAsset(asset_path);
            }
            return success;
        }
        
        // ========== 资产导入 ==========
        
        // 导入原始资产（生成 .zasset 文件）
        bool importSourceAsset(const std::filesystem::path& source_path,
                              const std::filesystem::path& output_path,
                              const AssetImporterSettings* import_settings = nullptr);
        
        // 重新导入资产（源文件变化时）
        bool reimportAsset(const std::string& asset_path, 
                          const AssetImporterSettings* import_settings = nullptr);
        
        // ========== 资产创建/删除/移动 ==========
        
        // 创建资产（直接创建 .zasset 文件，不经过导入）
        template<typename AssetType>
        bool createAsset(const AssetType& asset, 
                        const AssetMetadata& metadata, 
                        const std::string& asset_path) const;
        
        // 删除资产
        bool deleteAsset(const std::string& asset_path);
        
        // 重命名/移动资产
        bool moveAsset(const std::string& old_path, const std::string& new_path);
        
        // ========== 依赖关系 ==========
        
        // 获取依赖关系（从 .zasset 文件加载元数据）
        std::vector<std::string> getDependencies(const std::string& asset_path) const;
        std::vector<std::string> getReferencers(const std::string& asset_path) const;
        
        // ========== 刷新 ==========
        
        // 刷新所有资产
        void refreshAssets();
        
        // 刷新单个资产
        void refreshAsset(const std::string& asset_path);
        
        // ========== 访问内部组件 ==========
        
        AssetRegistry& getAssetRegistry() { return m_asset_registry; }
        AssetImportManager& getImportManager() { return m_import_manager; }
        FileSystemWatcher& getFileSystemWatcher() { return m_file_watcher; }

    private:
        AssetRegistry      m_asset_registry;
        AssetImportManager m_import_manager;
        FileSystemWatcher  m_file_watcher;
        
        // 文件系统监听回调
        void onFileCreated(const std::filesystem::path& path);
        void onFileChanged(const std::filesystem::path& path);
        void onFileDeleted(const std::filesystem::path& path);
    };
}
```

---

### 5. FileSystemWatcher（文件系统监听器）

**职责**：监听文件系统变化，自动更新 AssetRegistry。

#### 类定义

```cpp
// engine/source/editor/file_system_watcher/file_system_watcher.h
namespace Z
{
    class FileSystemWatcher
    {
    public:
        FileSystemWatcher();
        ~FileSystemWatcher();
        
        // 监听目录（只监听 .zasset 文件）
        void watchDirectory(const std::filesystem::path& directory);
        
        // 停止监听
        void stopWatching();
        
        // 设置回调函数
        void setOnFileChanged(std::function<void(const std::filesystem::path&)> callback);
        void setOnFileCreated(std::function<void(const std::filesystem::path&)> callback);
        void setOnFileDeleted(std::function<void(const std::filesystem::path&)> callback);
        
        // 更新（在主循环中调用，处理文件系统事件）
        void update();
        
        // 检查是否正在监听
        bool isWatching() const { return m_is_watching; }

    private:
        // 平台特定实现
        #ifdef _WIN32
        void* m_handle = nullptr;  // Windows: HANDLE
        #elif defined(__linux__)
        int m_inotify_fd = -1;
        #elif defined(__APPLE__)
        void* m_stream = nullptr;  // macOS: FSEventStreamRef
        #endif
        
        std::filesystem::path m_watched_directory;
        bool m_is_watching = false;
        
        std::function<void(const std::filesystem::path&)> m_on_file_changed;
        std::function<void(const std::filesystem::path&)> m_on_file_created;
        std::function<void(const std::filesystem::path&)> m_on_file_deleted;
        
        // 事件队列（用于线程安全）
        std::queue<std::filesystem::path> m_file_changed_queue;
        std::queue<std::filesystem::path> m_file_created_queue;
        std::queue<std::filesystem::path> m_file_deleted_queue;
        std::mutex m_queue_mutex;
    };
}
```

---

## 数据流和交互

### 编辑器启动流程

```
1. EditorAssetService::initialize()
   │
   ├─> AssetRegistry::initialize()
   │   │
   │   ├─> 尝试加载 AssetRegistry.cache
   │   │   └─> 如果成功：加载索引到内存
   │   │
   │   └─> AssetRegistry::scanAssetsAsync()
   │       │
   │       ├─> 如果缓存存在：增量更新（只扫描变化的文件）
   │       └─> 如果缓存不存在：全量扫描
   │
   ├─> FileSystemWatcher::watchDirectory("Content/")
   │   └─> 开始监听 .zasset 文件变化
   │
   └─> AssetImportManager::registerImporter()
       ├─> 注册 TextureImporter
       ├─> 注册 MeshImporter
       └─> 注册其他导入器
```

### 资产导入流程

```
1. 用户选择 texture.png
   │
   ├─> EditorAssetService::importSourceAsset()
   │   │
   │   ├─> AssetImportManager::findImporter("texture.png")
   │   │   └─> 返回 TextureImporter
   │   │
   │   ├─> TextureImporter::import()
   │   │   │
   │   │   ├─> 读取 texture.png
   │   │   ├─> 应用导入设置（压缩、格式转换等）
   │   │   ├─> 生成 GUID
   │   │   ├─> 创建 AssetMetadata
   │   │   └─> AssetFile::saveAsset()
   │   │       │
   │   │       └─> 写入 texture.zasset
   │   │           ├─> 文件头（GUID、类型等）
   │   │           ├─> 元数据（导入设置、依赖等）
   │   │           └─> 数据（压缩后的纹理）
   │   │
   │   └─> AssetRegistry::addAsset("texture.zasset")
   │       └─> 更新索引
   │
   └─> FileSystemWatcher 检测到新文件
       └─> AssetRegistry::refreshAsset("texture.zasset")
```

### 资产加载流程

```
1. 用户点击资产（如 texture.zasset）
   │
   ├─> EditorAssetService::loadAssetMetadata()
   │   │
   │   └─> AssetFile::loadMetadata()
   │       │
   │       ├─> 读取文件头（256 字节）
   │       ├─> 定位元数据偏移
   │       └─> 读取元数据部分（JSON）
   │
   └─> 如果需要加载完整资产数据
       │
       └─> EditorAssetService::loadAsset<Texture2D>()
           │
           └─> AssetManager::loadAsset()
               │
               └─> AssetFile::loadAsset()
                   │
                   ├─> 读取文件头
                   ├─> 读取元数据
                   └─> 读取数据部分
                       └─> 反序列化为 Texture2D 对象
```

---

## 文件格式规范

### .zasset 文件格式详细规范

#### 文件头（AssetFileHeader）

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0x00 | 4 | magic | 文件魔数 "ZASS" (0x5A415353) |
| 0x04 | 4 | version | 文件版本号（当前为 1） |
| 0x08 | 37 | guid | GUID 字符串（36 字符 + '\0'） |
| 0x2D | 64 | asset_type | 资产类型名称（如 "Texture2D"） |
| 0x6D | 8 | metadata_offset | 元数据在文件中的偏移（字节） |
| 0x75 | 8 | metadata_size | 元数据大小（字节） |
| 0x7D | 8 | data_offset | 数据在文件中的偏移（字节） |
| 0x85 | 8 | data_size | 数据大小（字节） |
| 0x8D | 32 | reserved | 保留字段（用于未来扩展） |

**总大小**：256 字节（固定）

#### 元数据部分（JSON 格式）

```json
{
    "guid": "12345678-1234-1234-1234-123456789abc",
    "source_file_path": "Content/Source/texture.png",
    "source_file_time": "2024-01-01T00:00:00Z",
    "import_settings": {
        "format": "BC7",
        "generate_mipmaps": true,
        "sRGB": true,
        "max_size": 4096
    },
    "dependencies": [
        "87654321-4321-4321-4321-cba987654321"
    ],
    "custom_metadata": {
        "tags": "diffuse,albedo",
        "author": "Artist Name",
        "version": "1.0"
    }
}
```

#### 数据部分（二进制）

- 格式：由资产类型决定
- 压缩：可选（压缩信息在元数据中）
- 对齐：建议 16 字节对齐，便于 SIMD 操作

---

## 线程安全和并发

### 线程安全策略

1. **AssetRegistry**：
   - 使用 `std::shared_mutex`（读写锁）
   - 读取操作：共享锁（允许多个线程同时读取）
   - 写入操作：独占锁（互斥）

2. **AssetFile**：
   - 无状态类，线程安全
   - 文件操作本身不是线程安全的，需要外部同步

3. **AssetImportManager**：
   - 导入器列表使用 `std::mutex` 保护
   - 导入操作本身可以并行执行（不同文件）

4. **FileSystemWatcher**：
   - 事件队列使用 `std::mutex` 保护
   - 回调在主线程执行

### 并发读取示例

```cpp
// 多个线程可以同时读取索引
void thread1()
{
    auto lock = m_asset_registry.acquireReadLock();  // 共享锁
    auto assets = m_asset_registry.findAssets("Texture2D");
}

void thread2()
{
    auto lock = m_asset_registry.acquireReadLock();  // 共享锁
    auto entry = m_asset_registry.getAssetIndex("texture.zasset");
}
```

---

## 错误处理和恢复

### 错误类型

1. **文件不存在**：返回 false，记录错误日志
2. **文件格式错误**：返回 false，记录错误日志
3. **GUID 冲突**：生成新 GUID，记录警告
4. **依赖缺失**：记录警告，但不阻止加载
5. **缓存损坏**：删除缓存，重新扫描

### 恢复策略

```cpp
bool AssetRegistry::loadCache(const std::filesystem::path& cache_path)
{
    try
    {
        // 尝试加载缓存
        std::ifstream file(cache_path);
        if (!file)
        {
            LOG_WARN("Cache file not found, will perform full scan");
            return false;
        }
        
        Json json = Json::parse(file);
        
        // 验证版本
        if (json["version"] != k_cache_version)
        {
            LOG_WARN("Cache version mismatch, will perform full scan");
            return false;
        }
        
        // 加载资产索引
        for (const auto& asset_json : json["assets"])
        {
            AssetIndexEntry entry;
            entry.deserialize(asset_json);
            m_asset_map[entry.asset_path] = entry;
            m_guid_to_path[entry.guid] = entry.asset_path;
        }
        
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to load cache: {}", e.what());
        // 删除损坏的缓存文件
        std::filesystem::remove(cache_path);
        return false;
    }
}
```

---

## 性能优化策略

### 1. 快速启动优化

- **缓存文件小**：只存储索引，不存储完整元数据
- **增量更新**：只扫描变化的文件
- **异步扫描**：不阻塞编辑器启动

### 2. 内存优化

- **按需加载**：元数据和资产数据按需从文件加载
- **索引轻量级**：每个索引条目约 150 字节
- **共享读取**：使用读写锁，支持并发读取

### 3. I/O 优化

- **文件头固定大小**：快速读取（256 字节）
- **元数据 JSON**：便于调试，但可以优化为二进制格式
- **数据压缩**：可选压缩，减少 I/O

### 4. 查询优化

- **哈希表索引**：O(1) 查找
- **GUID 映射**：快速 GUID 到路径转换
- **依赖关系缓存**：避免重复加载元数据

---

## API 参考

### EditorAssetService 主要 API

```cpp
// 初始化
void initialize();

// 资产查询
std::vector<AssetIndexEntry> findAssets(const std::string& filter) const;
AssetIndexEntry* getAssetIndex(const std::string& asset_path) const;

// 资产加载
template<typename AssetType>
bool loadAsset(const std::string& asset_path, AssetType& out_asset) const;

// 资产保存
template<typename AssetType>
bool saveAsset(const AssetType& asset, 
              const AssetMetadata& metadata, 
              const std::string& asset_path) const;

// 资产导入
bool importSourceAsset(const std::filesystem::path& source_path,
                      const std::filesystem::path& output_path,
                      const AssetImporterSettings* import_settings = nullptr);

// 依赖关系
std::vector<std::string> getDependencies(const std::string& asset_path) const;
std::vector<std::string> getReferencers(const std::string& asset_path) const;
```

---

## 实现示例

### 示例 1：导入纹理

```cpp
// 用户选择 texture.png，点击导入
void onImportTexture(const std::filesystem::path& source_path)
{
    EditorAssetService& asset_service = g_editor_global_context.m_asset_service;
    
    // 设置导入选项
    TextureImporterSettings settings;
    settings.format = TextureImporterSettings::Format::BC7;
    settings.generate_mipmaps = true;
    settings.sRGB = true;
    
    // 确定输出路径
    std::filesystem::path output_path = source_path;
    output_path.replace_extension(".zasset");
    
    // 导入
    bool success = asset_service.importSourceAsset(source_path, output_path, &settings);
    if (success)
    {
        LOG_INFO("Texture imported successfully: {}", output_path);
    }
}
```

### 示例 2：加载资产元数据

```cpp
// 在 Inspector 中显示资产信息
void showAssetInspector(const std::string& asset_path)
{
    EditorAssetService& asset_service = g_editor_global_context.m_asset_service;
    
    // 获取索引（快速）
    auto* index = asset_service.getAssetIndex(asset_path);
    if (!index)
    {
        LOG_ERROR("Asset not found: {}", asset_path);
        return;
    }
    
    // 加载完整元数据（按需）
    AssetMetadata metadata;
    if (asset_service.loadAssetMetadata(asset_path, metadata))
    {
        // 显示元数据
        ImGui::Text("GUID: %s", metadata.guid.c_str());
        ImGui::Text("Type: %s", index->asset_type.c_str());
        ImGui::Text("Source: %s", metadata.source_file_path.c_str());
        
        // 显示依赖
        auto dependencies = asset_service.getDependencies(asset_path);
        ImGui::Text("Dependencies: %zu", dependencies.size());
    }
}
```

### 示例 3：批量导入

```cpp
// 批量导入 Source 目录下的所有纹理
void batchImportTextures()
{
    EditorAssetService& asset_service = g_editor_global_context.m_asset_service;
    
    std::filesystem::path source_folder = "Content/Source";
    std::filesystem::path output_folder = "Content";
    
    // 收集所有纹理文件
    std::vector<std::filesystem::path> texture_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source_folder))
    {
        if (entry.is_regular_file())
        {
            auto ext = entry.path().extension().string();
            if (ext == ".png" || ext == ".jpg" || ext == ".tga")
            {
                texture_files.push_back(entry.path());
            }
        }
    }
    
    // 批量导入
    TextureImporterSettings settings;
    settings.format = TextureImporterSettings::Format::BC7;
    
    for (const auto& source_path : texture_files)
    {
        std::filesystem::path output_path = output_folder / source_path.filename();
        output_path.replace_extension(".zasset");
        
        asset_service.importSourceAsset(source_path, output_path, &settings);
    }
}
```

---

## 总结

本设计文档详细描述了 ZEngine 资源管理系统的各个组件，包括：

1. **完整的类设计**：所有核心类的详细接口定义
2. **数据流图**：系统各组件之间的交互流程
3. **文件格式规范**：.zasset 文件的详细格式
4. **线程安全策略**：并发访问的安全保证
5. **错误处理**：错误类型和恢复策略
6. **性能优化**：各种优化策略
7. **实现示例**：实际使用场景的代码示例

该设计完全遵循 Unreal Engine 的设计思路，采用轻量级索引 + 元数据存储在资产文件中的架构，能够很好地扩展到超大型项目。

