# ZEngine 资产管理系统设计指南

> ⚠️ **状态：早期设计草稿（pre-Route-B）**
>
> 本文档与同目录下的 `ASSET_MANAGEMENT_DETAILED_DESIGN.md` 都是 ZEngine
> 资产管线**最初的设计草稿**，记录的是设计意图与对 Unity / UE 的对比，
> **不是**当前代码实现。请勿当作 API 参考使用。
>
> 本文中描述的 `AssetFile` 类（含 `saveAsset` / `loadAsset` /
> `loadMetadata` / `isValidAssetFile` 等成员）**从未真正实现完成**：原型
> 期的模板体全部被注释掉、`writeMetadata` / `readMetadata` 都是
> `return true;` 的空实现。整套类已在 **P2 #9** 中删除。请参见
> `engine/Source/Runtime/asset/asset_file.h` 顶部注释。
>
> 当前的真实落地路径（**Route B**，详见 `doc/BINDLESS_TEXTURE_PATH.md`
> PR10）是：
>
> | 早期草稿描述 | 当前实现（Route B） |
> |---|---|
> | `AssetFile::saveAsset(asset, metadata, path)` | `AssetManager::WriteObjectToDiskThreadSafe(path, *object)` 走 `SerializedFile` |
> | `AssetFile::loadAsset<T>(path, out)` | `AssetBundle::Get<T>(path)` 经 `SerializedFile` 反序列化 |
> | `AssetFile::loadMetadata(path, out)` | `AssetRegistry::scanSingleAsset` 直接 `ifstream::read` 读 176 字节 `AssetFileHeader` |
> | `AssetFile` 的"元数据段"（导入设置、依赖、custom_metadata）| 当前 `AssetFileHeader::metadata_offset/size = 0`，**元数据段尚未启用**；如需重新引入，应在 `SerializedFile` 层而非新建 `AssetFile` |
>
> 本文档保留是为了记录设计动机与跨引擎对比；查阅当前实现请直接读
> 源码或 `doc/BINDLESS_TEXTURE_PATH.md` / `doc/asset_management/AUTO_IMPORT_AND_FILE_WATCHER.md`。

## 概述

本文档对比了 Unity 和 Unreal Engine 的资产管理系统，并基于 ZEngine 的当前架构提出资产管理系统的设计建议。

> **详细设计文档**：本文档提供了高层次的架构设计。如需查看详细的类设计、API 参考和实现细节，请参阅 [ASSET_MANAGEMENT_DETAILED_DESIGN.md](./ASSET_MANAGEMENT_DETAILED_DESIGN.md)。

---

## Unity 的 AssetDatabase 系统

### 核心特性

Unity 的 `AssetDatabase` 是编辑器专用的资产管理系统，主要特点包括：

1. **元数据管理**
   - 每个资产文件都有对应的 `.meta` 文件
   - `.meta` 文件存储 GUID、导入设置、依赖关系等信息
   - 通过 GUID 而非文件路径引用资产，确保资产重命名/移动后引用不丢失

2. **资产导入管道**
   - 自动检测文件系统变化（`AssetDatabase.Refresh()`）
   - 支持自定义导入器（`ScriptedImporter`）
   - 导入时生成运行时格式（如纹理压缩、模型优化）

3. **依赖关系追踪**
   - 自动追踪资产之间的引用关系
   - 支持查找资产的所有引用者（`AssetDatabase.GetDependencies()`）
   - 支持查找资产的所有依赖（`AssetDatabase.GetDependencies()`）

4. **资产数据库**
   - 维护所有资产的索引，支持快速查找
   - 提供 API 查询资产（`AssetDatabase.FindAssets()`）
   - 支持资产标签和分类

### Unity AssetDatabase 主要 API

```csharp
// 刷新资产数据库
AssetDatabase.Refresh();

// 加载资产
Object asset = AssetDatabase.LoadAssetAtPath<T>(path);

// 查找资产
string[] guids = AssetDatabase.FindAssets("t:Texture2D");

// 获取依赖关系
string[] dependencies = AssetDatabase.GetDependencies(path);

// 创建资产
AssetDatabase.CreateAsset(obj, path);

// 保存资产
AssetDatabase.SaveAssets();
```

---

## Unreal Engine 的资产管理系统

### 核心组件

Unreal Engine 的资产管理系统比 Unity 更复杂，主要包含以下组件：

#### 1. **AssetRegistry（资产注册表）**

- **功能**：在编辑器启动时扫描所有资产，构建资产索引
- **特点**：
  - 不加载实际资产数据，只存储元数据（路径、类型、依赖等）
  - 支持异步加载，不阻塞编辑器
  - 提供快速查询接口（按类型、标签、路径等）

```cpp
// Unreal 风格的资产查询
FAssetRegistryModule& AssetRegistryModule = 
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
TArray<FAssetData> AssetDataList;
AssetRegistryModule.Get().GetAssetsByClass(UStaticMesh::StaticClass(), AssetDataList);
```

#### 2. **Content Browser（内容浏览器）**

- 可视化资产浏览器
- 支持文件夹结构、搜索、筛选
- 支持资产标签和收藏夹

#### 3. **UAsset 系统**

- 每个资产都是 `UObject` 的派生类
- 资产文件包含序列化的对象数据
- 支持资产引用（通过路径或 GUID）

#### 4. **资产导入管道**

- **Factory 系统**：不同类型的资产有不同的 Factory（如 `FTextureFactory`、`FStaticMeshFactory`）
- **Import Settings**：导入时配置资产参数
- **Reimport**：支持重新导入资产

#### 5. **资产元数据（Metadata）**

- 支持自定义元数据标签
- 用于筛选、搜索和组织资产
- 可在内容浏览器中显示

### Unreal 资产管理特点

1. **基于对象系统**：所有资产都是 UObject，支持反射和序列化
2. **引用系统**：支持软引用（路径）和硬引用（直接指针）
3. **异步加载**：大量使用异步加载，不阻塞主线程
4. **模块化**：资产系统高度模块化，易于扩展

---

## ZEngine 资产管理建议

基于 ZEngine 的当前架构（已有 `AssetManager`、`EditorFileService`、`ProjectBrowser`），**完全按照 Unreal Engine 的设计思路**，采用以下设计：

### 核心设计理念：原始资产 vs 导入资产

**关键原则**：ZEngine **不直接管理原始美术资产**（png、fbx、obj 等），而是通过导入管道将原始资产转换为 ZEngine 的资产格式（`.zasset`），元数据直接存储在资产文件中。

#### 资产类型划分

1. **原始资产（Source Assets）**
   - 美术人员创建的文件：`.png`、`.jpg`、`.fbx`、`.obj`、`.gltf` 等
   - 存储在 `Content/Source/` 目录下（可选，也可以放在项目外）
   - **ZEngine 不直接管理这些文件**，只作为导入源

2. **导入资产（Imported Assets）**
   - 通过导入管道生成的 `.zasset` 文件
   - 存储在 `Content/` 目录下
   - **这是 ZEngine 实际管理的资产**
   - 包含元数据和序列化的资产数据（类似 Unreal 的 `.uasset`）

#### 导入流程

```
原始资产（texture.png）
    ↓
导入管道（TextureImporter）
    ↓
生成 .zasset 文件（texture.zasset）
    ├── 元数据（GUID、导入设置、依赖关系等）
    └── 序列化的资产数据（压缩后的纹理数据）
```

### 1. 资产注册表（AssetRegistry）

**目的**：在编辑器启动时快速加载资产索引，**只存储轻量级索引信息**，不存储完整元数据。

#### 核心设计理念（完全借鉴 Unreal Engine）

**关键优化**：
- AssetRegistry 只存储**最小索引信息**（路径、类型、GUID、时间戳）
- **完整元数据存储在 .zasset 文件中**，按需加载
- 缓存文件很小（< 10 MB，即使 100,000 个资产），可以加入版本控制

#### 设计要点：

```cpp
// engine/source/editor/asset_registry/asset_registry.h
namespace Z
{
    // AssetRegistry 只存储轻量级索引（类似 Unreal 的 FAssetData）
    struct AssetIndexEntry
    {
        string asset_path;           // .zasset 文件路径（相对路径）
        string asset_type;           // 资产类型（通过反射获取）
        string guid;                 // 唯一标识符（存储在 .zasset 文件中）
        std::filesystem::file_time_type last_modified;  // 最后修改时间
        uint64_t file_size;          // 文件大小（用于快速检查变化）
    };

    class AssetRegistry
    {
    public:
        // 初始化：从缓存文件加载，然后增量更新
        // 启动流程：
        // 1. 尝试加载 .zengine/AssetRegistry.cache（二进制格式）
        // 2. 如果缓存不存在或版本不匹配，扫描所有 .zasset 文件
        // 3. 如果缓存存在，比较文件时间戳，只扫描变化的文件
        // 4. 异步执行，不阻塞编辑器启动
        void initialize(const std::filesystem::path& asset_folder);
        
        // 异步扫描资产文件夹（后台线程，只扫描 .zasset 文件）
        void scanAssetsAsync(const std::filesystem::path& asset_folder);
        
        // 增量更新：只扫描变化的 .zasset 文件
        void incrementalUpdate(const std::filesystem::path& asset_folder);
        
        // 保存缓存到磁盘
        void saveCache(const std::filesystem::path& cache_path);
        
        // 从缓存文件加载
        bool loadCache(const std::filesystem::path& cache_path);
        
        // 查询资产（只返回索引信息，不加载完整元数据）
        std::vector<AssetIndexEntry> findAssets(const string& filter) const;
        AssetIndexEntry* getAssetIndex(const string& asset_path) const;
        AssetIndexEntry* getAssetIndexByGUID(const string& guid) const;
        
        // 获取依赖关系（需要从 .zasset 文件加载元数据）
        std::vector<string> getDependencies(const string& asset_path) const;
        std::vector<string> getReferencers(const string& asset_path) const;
        
        // 监听文件系统变化
        void refreshAsset(const string& asset_path);
        void removeAsset(const string& asset_path);
        
        // 检查扫描状态
        bool isScanning() const { return m_is_scanning; }
        float getScanProgress() const { return m_scan_progress; }
        
    private:
        std::unordered_map<string, AssetIndexEntry> m_asset_index;  // path -> index
        std::unordered_map<string, string> m_guid_to_path;         // guid -> path
        
        bool m_is_scanning = false;
        float m_scan_progress = 0.0f;
        std::thread m_scan_thread;
    };
}
```

#### 缓存文件格式

**位置**：`.zengine/AssetRegistry.cache`（项目根目录下的隐藏文件夹）

**格式**：二进制格式，包含：
- 版本号（用于兼容性检查）
- 资产索引列表（路径、类型、GUID、时间戳、文件大小）
- GUID 到路径的映射（用于快速查找）

**文件大小估算**：
- 每个索引条目：约 150-200 字节（路径、类型、GUID、时间戳、文件大小）
- 100,000 个资产 ≈ 15-20 MB（完全可以加入版本控制）
- 1,000,000 个资产 ≈ 150-200 MB（仍可接受，但可能需要 Git LFS）

**优势**：
- ✅ **轻量级**：只存储索引信息，文件很小
- ✅ **快速加载**：通常 < 100ms，即使有数十万个资产
- ✅ **版本控制友好**：文件小，可以加入版本控制
- ✅ **增量更新**：只扫描变化的 .zasset 文件
- ✅ **不阻塞启动**：异步扫描，编辑器立即可用

### 2. 元数据存储策略（完全按照 Unreal Engine）

**核心设计**：**元数据直接存储在 .zasset 文件中**，类似 Unreal 的 `.uasset` 文件结构。

#### .zasset 文件格式

每个 `.zasset` 文件包含两部分：

1. **文件头（Asset Header）**
   - 文件版本号
   - GUID（唯一标识符）
   - 资产类型
   - 元数据大小
   - 数据大小

2. **元数据部分（Asset Metadata）**
   - 导入设置（Import Settings）
   - 依赖关系（Dependencies，存储其他资产的 GUID）
   - 自定义元数据（Custom Metadata，标签、作者等）
   - 源文件信息（Source File Path，指向原始资产）

3. **数据部分（Asset Data）**
   - 序列化的资产数据（压缩后的纹理、模型等）

#### 设计要点：

```cpp
// engine/source/runtime/asset/asset_file.h
namespace Z
{
    // .zasset 文件结构
    struct AssetFileHeader
    {
        uint32_t magic;              // 文件魔数（"ZASS"）
        uint32_t version;            // 文件版本
        string guid;                 // 资产 GUID
        string asset_type;           // 资产类型
        uint64_t metadata_offset;    // 元数据偏移
        uint64_t metadata_size;      // 元数据大小
        uint64_t data_offset;        // 数据偏移
        uint64_t data_size;          // 数据大小
    };

    struct AssetMetadata
    {
        string guid;                 // GUID（冗余，方便访问）
        string source_file_path;     // 原始资产路径（可选）
        std::filesystem::file_time_type source_file_time;  // 原始文件时间戳
        
        // 导入设置
        Json import_settings;        // 例如：{"format": "RGBA8", "generate_mipmaps": true, "compression": "BC7"}
        
        // 依赖关系（存储其他资产的 GUID）
        std::vector<string> dependencies;  // 依赖的其他资产 GUID
        
        // 自定义元数据
        std::unordered_map<string, string> custom_metadata;  // 例如：{"tags": "diffuse,albedo", "author": "Artist Name"}
    };

    class AssetFile
    {
    public:
        // 从 .zasset 文件加载元数据（不加载实际数据）
        bool loadMetadata(const std::filesystem::path& asset_path, AssetMetadata& out_metadata);
        
        // 从 .zasset 文件加载完整资产（包括数据）
        template<typename AssetType>
        bool loadAsset(const std::filesystem::path& asset_path, AssetType& out_asset);
        
        // 保存资产到 .zasset 文件
        template<typename AssetType>
        bool saveAsset(const AssetType& asset, const AssetMetadata& metadata, const std::filesystem::path& asset_path);
    };
}
```

#### 优势

- ✅ **完全一致**：所有资产使用相同的元数据存储方式（存储在 .zasset 文件中）
- ✅ **版本控制友好**：.zasset 文件可以加入版本控制，元数据不会丢失
- ✅ **分布式存储**：元数据分散在各个资产文件中，不会集中在一个大文件
- ✅ **按需加载**：AssetRegistry 只加载索引，完整元数据按需从 .zasset 文件加载
- ✅ **性能优秀**：不需要为每个资产创建额外的元数据文件
- ✅ **易于迁移**：资产文件自包含，易于备份和迁移

#### 与 Unreal Engine 的对比

| 特性 | Unreal Engine | ZEngine（设计） |
|------|---------------|-----------------|
| **原始资产** | 不直接管理 | 不直接管理 |
| **导入资产** | .uasset 文件 | .zasset 文件 |
| **元数据存储** | 存储在 .uasset 文件中 | 存储在 .zasset 文件中 |
| **AssetRegistry** | 轻量级索引（AssetRegistry.bin） | 轻量级索引（AssetRegistry.cache） |
| **版本控制** | .uasset 可以加入版本控制 | .zasset 可以加入版本控制 |

### 3. 资产导入管道（Asset Import Pipeline）

**目的**：将原始资产（png、fbx 等）导入并转换为 `.zasset` 文件，元数据直接写入资产文件。

#### 设计要点：

```cpp
// engine/source/editor/asset_pipeline/asset_importer.h
namespace Z
{
    class AssetImporter
    {
    public:
        virtual ~AssetImporter() = default;
        
        // 检查是否可以导入该文件类型
        virtual bool canImport(const std::filesystem::path& file_path) const = 0;
        
        // 导入原始资产，生成 .zasset 文件
        // source_path: 原始资产路径（如 texture.png）
        // output_path: 输出的 .zasset 文件路径（如 texture.zasset）
        // import_settings: 导入设置（格式、压缩等）
        // 返回：生成的 .zasset 文件路径
        virtual bool import(const std::filesystem::path& source_path, 
                           const std::filesystem::path& output_path,
                           const Json& import_settings,
                           AssetMetadata& out_metadata) = 0;
        
        // 重新导入（源文件变化时）
        virtual bool reimport(const std::filesystem::path& zasset_path,
                             const Json& import_settings) = 0;
    };

    class AssetImportManager
    {
    public:
        void registerImporter(std::shared_ptr<AssetImporter> importer);
        
        // 导入原始资产，生成 .zasset 文件
        // source_path: 原始资产路径（如 Content/Source/texture.png）
        // output_path: 输出的 .zasset 文件路径（如 Content/texture.zasset）
        // import_settings: 导入设置（可选，使用默认设置）
        bool importAsset(const std::filesystem::path& source_path,
                        const std::filesystem::path& output_path,
                        const Json& import_settings = {});
        
        // 自动检测原始资产并导入（扫描 Source 目录）
        void importSourceAssets(const std::filesystem::path& source_folder,
                               const std::filesystem::path& output_folder);
        
    private:
        std::vector<std::shared_ptr<AssetImporter>> m_importers;
    };
}
```

#### 导入流程示例

```cpp
// 示例：导入纹理
// 1. 用户选择 texture.png
// 2. AssetImportManager 找到 TextureImporter
// 3. TextureImporter 执行导入：
//    - 读取 texture.png
//    - 应用导入设置（压缩、格式转换等）
//    - 生成 texture.zasset 文件：
//      - 文件头（GUID、类型等）
//      - 元数据（导入设置、源文件路径等）
//      - 数据（压缩后的纹理数据）
// 4. AssetRegistry 自动检测新的 .zasset 文件并更新索引
```

#### 导入器示例

```cpp
// engine/source/editor/asset_pipeline/texture_importer.h
class TextureImporter : public AssetImporter
{
public:
    bool canImport(const std::filesystem::path& file_path) const override
    {
        auto ext = file_path.extension().string();
        return ext == ".png" || ext == ".jpg" || ext == ".tga" || ext == ".dds";
    }
    
    bool import(const std::filesystem::path& source_path,
               const std::filesystem::path& output_path,
               const Json& import_settings,
               AssetMetadata& out_metadata) override
    {
        // 1. 读取原始纹理
        Image source_image = loadImage(source_path);
        
        // 2. 应用导入设置（压缩、格式转换等）
        Image processed_image = processImage(source_image, import_settings);
        
        // 3. 生成 GUID
        out_metadata.guid = generateGUID(source_path);
        out_metadata.source_file_path = source_path.string();
        out_metadata.import_settings = import_settings;
        
        // 4. 保存为 .zasset 文件
        AssetFile asset_file;
        return asset_file.saveAsset(processed_image, out_metadata, output_path);
    }
};
```

### 4. 编辑器资产服务（EditorAssetService）

**目的**：在编辑器中提供资产管理的统一接口，连接 AssetRegistry、AssetImportManager 和 AssetManager。

#### 设计要点：

```cpp
// engine/source/editor/editor_asset_service/editor_asset_service.h
namespace Z
{
    class EditorAssetService
    {
    public:
        // 初始化（在编辑器启动时调用）
        void initialize();
        
        // 资产查询（使用 AssetRegistry，只返回索引信息，不加载完整元数据）
        std::vector<AssetIndexEntry> findAssets(const string& filter) const;
        AssetIndexEntry* getAssetIndex(const string& asset_path) const;
        
        // 加载资产元数据（从 .zasset 文件加载，不加载实际数据）
        bool loadAssetMetadata(const string& asset_path, AssetMetadata& out_metadata) const;
        
        // 资产加载（使用 AssetManager，加载完整资产数据）
        template<typename AssetType>
        bool loadAsset(const string& asset_path, AssetType& out_asset) const
        {
            return g_runtime_global_context.m_asset_manager->loadAsset(asset_path, out_asset);
        }
        
        // 资产保存（保存到 .zasset 文件）
        template<typename AssetType>
        bool saveAsset(const AssetType& asset, const AssetMetadata& metadata, const string& asset_path) const
        {
            AssetFile asset_file;
            bool success = asset_file.saveAsset(asset, metadata, asset_path);
            if (success)
            {
                m_asset_registry.refreshAsset(asset_path);
            }
            return success;
        }
        
        // 导入原始资产（生成 .zasset 文件）
        bool importSourceAsset(const std::filesystem::path& source_path,
                              const std::filesystem::path& output_path,
                              const Json& import_settings = {});
        
        // 重新导入资产（源文件变化时）
        bool reimportAsset(const string& asset_path, const Json& import_settings = {});
        
        // 创建资产（直接创建 .zasset 文件，不经过导入）
        template<typename AssetType>
        bool createAsset(const AssetType& asset, const AssetMetadata& metadata, const string& asset_path) const;
        
        // 删除资产
        bool deleteAsset(const string& asset_path);
        
        // 重命名/移动资产
        bool moveAsset(const string& old_path, const string& new_path);
        
        // 获取依赖关系（从 .zasset 文件加载元数据）
        std::vector<string> getDependencies(const string& asset_path) const;
        std::vector<string> getReferencers(const string& asset_path) const;
        
        // 刷新资产（文件系统变化时调用）
        void refreshAssets();
        void refreshAsset(const string& asset_path);
        
    private:
        AssetRegistry m_asset_registry;
        AssetImportManager m_import_manager;
    };
}
```

### 5. 文件系统监听（FileSystemWatcher）

**目的**：自动检测资产文件夹的变化，更新 AssetRegistry。

#### 设计要点：

```cpp
// engine/source/editor/file_system_watcher/file_system_watcher.h
namespace Z
{
    class FileSystemWatcher
    {
    public:
        void watchDirectory(const std::filesystem::path& directory);
        void setOnFileChanged(std::function<void(const std::filesystem::path&)> callback);
        void setOnFileCreated(std::function<void(const std::filesystem::path&)> callback);
        void setOnFileDeleted(std::function<void(const std::filesystem::path&)> callback);
        
        void update();  // 在主循环中调用
    };
}
```

### 6. 集成到现有系统

#### 修改 EditorFileService

```cpp
// 在 EditorFileService 中集成 AssetRegistry
class EditorFileService
{
public:
    void buildEngineFileTree()
    {
        // 使用 AssetRegistry 获取资产列表，而不是直接扫描文件系统
        auto assets = g_editor_global_context.m_asset_service->findAssets("");
        // ... 构建文件树
    }
};
```

#### 修改 ProjectBrowser

```cpp
// 在 ProjectBrowser 中显示资产元数据
class ProjectBrowser
{
    void onFileContentItemClicked(EditorFileNode* node)
    {
        // 获取资产元数据
        auto* metadata = g_editor_global_context.m_asset_service->getAssetMetadata(node->m_file_path);
        if (metadata)
        {
            // 显示资产信息
            // 可以选择加载资产到 Inspector
        }
    }
};
```

---

## 性能优化策略

### 问题：避免 Unity 风格的 Library 目录问题

**Unity 的问题**：
- 每个资产都有 `.meta` 文件，文件系统开销大
- 初次加载需要扫描所有资产并生成 `.meta` 文件
- 大型项目（数万个资产）初次加载可能需要数分钟
- 元数据集中在缓存文件中，超大型项目缓存文件过大，无法加入版本控制

### ZEngine 的解决方案（完全按照 Unreal Engine）

#### 1. **轻量级索引缓存**（类似 Unreal 的 AssetRegistry.bin）

- 单个二进制缓存文件：`.zengine/AssetRegistry.cache`
- **只存储索引信息**（路径、类型、GUID、时间戳），不存储完整元数据
- 启动时快速加载（通常 < 100ms，即使有数十万个资产）
- 文件大小：100,000 个资产 ≈ 15-20 MB（完全可以加入版本控制）

#### 2. **元数据存储在资产文件中**

- **完整元数据存储在 .zasset 文件中**，类似 Unreal 的 .uasset
- 元数据分散存储，不会集中在一个大文件
- 按需加载，不阻塞启动
- 版本控制友好：.zasset 文件可以加入版本控制

#### 3. **增量更新机制**

```
启动流程：
1. 加载 AssetRegistry.cache（如果存在）
2. 比较缓存中的文件时间戳与磁盘上的 .zasset 文件时间戳
3. 只扫描变化的 .zasset 文件（新增、修改、删除）
4. 更新缓存并保存
```

**性能对比**：
- **全量扫描**（无缓存）：100,000 个 .zasset 文件 ≈ 10-20 秒
- **增量更新**（有缓存）：100,000 个资产，100 个变化 ≈ 0.5-1 秒

#### 4. **异步加载**

- 编辑器启动时立即显示 UI
- 资产扫描在后台线程执行（只扫描 .zasset 文件，不扫描原始资产）
- 显示扫描进度条
- 支持在扫描完成前使用已缓存的资产

#### 5. **智能 GUID 生成**

- 基于文件路径和内容哈希生成稳定的 GUID
- GUID 存储在 .zasset 文件中，即使缓存丢失也能从资产文件恢复
- 支持在 .zasset 文件中手动覆盖 GUID（用于特殊需求，如资产迁移）

#### 6. **原始资产不参与扫描**

- AssetRegistry 只扫描 `.zasset` 文件，不扫描原始资产（png、fbx 等）
- 原始资产只在导入时使用，不参与索引构建
- 大幅减少扫描时间（只扫描已导入的资产）

### 超大型项目的优势

#### 文件大小对比

**旧方案（元数据在缓存中）**：
- 每个资产的完整元数据：约 1000-2000 字节
- 100,000 个资产 ≈ 100-200 MB（无法加入版本控制）

**新方案（元数据在 .zasset 文件中）**：
- AssetRegistry.cache（只存储索引）：约 150 字节/资产 ≈ 15 MB（100,000 个资产）
- 元数据存储在各自的 .zasset 文件中，分散存储
- **缓存文件小，可以加入版本控制**

#### 性能优势

1. **快速启动**：
   - 缓存文件小（15-20 MB），加载时间 < 100ms
   - 元数据按需从 .zasset 文件加载，不阻塞启动

2. **版本控制友好**：
   - 缓存文件小，可以加入版本控制
   - .zasset 文件可以加入版本控制，元数据不会丢失
   - 不需要依赖本地构建缓存

3. **分布式存储**：
   - 元数据分散在各个资产文件中
   - 不会出现单个大文件的问题
   - 易于并行处理和增量同步

4. **可扩展性**：
   - 支持任意规模的项目（100 万+ 资产）
   - 缓存文件大小线性增长，但始终很小
   - 元数据存储不受项目规模限制

## 实施建议

### 阶段 1：.zasset 文件格式 + AssetFile 类

1. 定义 `.zasset` 文件格式（文件头、元数据、数据）
2. 实现 `AssetFile` 类（读写 .zasset 文件）
3. 实现元数据序列化/反序列化
4. 实现资产数据序列化/反序列化
5. 编写单元测试验证文件格式

### 阶段 2：基础 AssetRegistry + 轻量级缓存

1. 实现 `AssetRegistry` 类（只存储索引信息）
2. **实现轻量级缓存文件读写**（只存储路径、类型、GUID、时间戳）
3. 实现资产扫描功能（只扫描 .zasset 文件，同步版本）
4. 实现基本的查询接口
5. 集成到 `EditorGlobalContext`

### 阶段 3：增量更新 + 异步加载

1. **实现增量更新机制**（比较 .zasset 文件时间戳）
2. **实现异步扫描**（后台线程，只扫描 .zasset 文件）
3. **实现扫描进度显示**
4. 实现 GUID 生成和管理（基于路径+内容哈希，存储在 .zasset 文件中）

### 阶段 4：资产导入管道

1. 实现 `AssetImporter` 基类
2. 实现 `TextureImporter`（导入 png/jpg 等，生成 .zasset）
3. 实现 `AssetImportManager`
4. 实现重新导入功能（检测源文件变化）

### 阶段 5：文件系统监听

1. 实现 `FileSystemWatcher`（只监听 .zasset 文件）
2. 集成到编辑器主循环
3. 实现自动刷新机制（与增量更新结合）

### 阶段 6：编辑器集成

1. 实现 `EditorAssetService`
2. 更新 `EditorFileService` 和 `ProjectBrowser`（只显示 .zasset 文件）
3. 添加资产导入/删除/重命名功能
4. 实现依赖关系追踪（从 .zasset 文件加载元数据）

---

## 与现有系统的对比

### 当前 ZEngine 架构

- ✅ **AssetManager**：运行时资产加载/保存
- ✅ **EditorFileService**：文件树构建
- ✅ **ProjectBrowser**：资产浏览器 UI
- ❌ **AssetRegistry**：资产索引和查询（缺失）
- ❌ **.zasset 文件格式**：统一的资产文件格式（缺失）
- ❌ **元数据系统**：GUID 和依赖追踪（缺失）
- ❌ **资产导入管道**：原始资产导入（缺失）
- ❌ **文件系统监听**：自动刷新（缺失）
- ❌ **统一编辑器资产服务**：编辑器专用 API（缺失）

### 建议的改进

1. **分离编辑器和运行时**：
   - `AssetManager` 用于运行时加载 `.zasset` 文件
   - `EditorAssetService` 用于编辑器（导入、创建、删除资产）

2. **添加资产索引**：
   - `AssetRegistry` 提供快速查询，只存储轻量级索引
   - 完整元数据存储在 `.zasset` 文件中，按需加载

3. **元数据管理**：
   - 所有元数据（GUID、导入设置、依赖关系）存储在 `.zasset` 文件中
   - 缓存文件只存储索引信息，文件小，可以加入版本控制

4. **资产导入**：
   - 原始资产（png、fbx 等）通过导入管道转换为 `.zasset` 文件
   - ZEngine 只管理 `.zasset` 文件，不直接管理原始资产

5. **自动同步**：
   - 文件系统监听自动检测 `.zasset` 文件变化，更新索引

---

## 总结

ZEngine **完全按照 Unreal Engine 的设计思路**，采用 **AssetRegistry + .zasset 文件** 架构：

### 核心设计原则

1. **原始资产 vs 导入资产**：
   - ZEngine **不直接管理原始美术资产**（png、fbx、obj 等）
   - 原始资产通过导入管道转换为 `.zasset` 文件
   - ZEngine 只管理 `.zasset` 文件（类似 Unreal 的 .uasset）

2. **AssetRegistry + 轻量级索引**：
   - AssetRegistry 只存储**最小索引信息**（路径、类型、GUID、时间戳）
   - 缓存文件很小（100,000 个资产 ≈ 15-20 MB），可以加入版本控制
   - 增量更新机制，只扫描变化的 .zasset 文件
   - 异步扫描，不阻塞编辑器启动

3. **元数据存储在资产文件中**：
   - **完整元数据存储在 .zasset 文件中**（类似 Unreal 的 .uasset）
   - 元数据分散存储，不会集中在一个大文件
   - 按需加载，不阻塞启动
   - 版本控制友好：.zasset 文件可以加入版本控制

4. **EditorAssetService**：统一的编辑器资产 API

5. **FileSystemWatcher**：自动检测 .zasset 文件变化，触发增量更新

6. **AssetImportManager**：统一的资产导入管道，将原始资产转换为 .zasset 文件

### 性能优势

- ✅ **快速启动**：
  - 缓存文件小（15-20 MB），加载时间 < 100ms（即使 100,000 个资产）
  - 元数据按需从 .zasset 文件加载，不阻塞启动
- ✅ **增量更新**：只扫描变化的 .zasset 文件，大幅减少扫描时间
- ✅ **异步加载**：不阻塞编辑器启动
- ✅ **可扩展性**：支持任意规模的项目（100 万+ 资产）
- ✅ **项目整洁**：资产文件夹中只有 .zasset 文件，没有额外的元数据文件
- ✅ **版本控制友好**：
  - 缓存文件小，可以加入版本控制
  - .zasset 文件可以加入版本控制，元数据不会丢失
  - 不需要依赖本地构建缓存

### 与 Unreal Engine 的对比

| 特性 | Unreal Engine | ZEngine（设计） |
|------|---------------|-----------------|
| **原始资产** | 不直接管理 | 不直接管理 |
| **导入资产** | .uasset 文件 | .zasset 文件 |
| **元数据存储** | 存储在 .uasset 文件中 | 存储在 .zasset 文件中 |
| **AssetRegistry** | 轻量级索引（AssetRegistry.bin） | 轻量级索引（AssetRegistry.cache） |
| **启动速度** | 快速（AssetRegistry.bin） | 快速（AssetRegistry.cache） |
| **版本控制** | .uasset 可以加入版本控制 | .zasset 可以加入版本控制 |
| **大型项目** | 支持超大型项目 | 支持超大型项目（100 万+ 资产） |

### 关键优势

1. **解决缓存文件过大的问题**：
   - 旧方案：元数据在缓存中，100,000 个资产 ≈ 100-200 MB（无法加入版本控制）
   - 新方案：元数据在 .zasset 文件中，缓存只有 15-20 MB（可以加入版本控制）

2. **版本控制友好**：
   - 缓存文件小，可以加入版本控制
   - .zasset 文件可以加入版本控制，元数据不会丢失
   - 不需要依赖本地构建缓存，团队成员拉取代码后即可使用

3. **可扩展性**：
   - 支持任意规模的项目（100 万+ 资产）
   - 缓存文件大小线性增长，但始终很小
   - 元数据存储不受项目规模限制（分散在各个 .zasset 文件中）

这样的设计既保持了运行时 `AssetManager` 的简洁性，又为编辑器提供了强大的资产管理能力，同时完全解决了缓存文件过大的问题，并且能够很好地扩展到超大型项目（如《黑神话：悟空》规模）。

