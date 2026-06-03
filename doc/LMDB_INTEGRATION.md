# LMDB 集成指南

## 概述

ZEngine 已集成 LMDB (Lightning Memory-Mapped Database) 作为派生数据缓存（DDC）系统的存储后端。LMDB 提供了高性能的键值存储，适用于资产导入结果、着色器编译缓存等场景。

## 安装 LMDB

### 方法 1：使用 Git Submodule（推荐）

LMDB 已添加到 `.gitmodules` 文件中。首次使用时需要初始化子模块：

```bash
# 初始化并更新所有子模块（包括 LMDB）
git submodule update --init --recursive

# 或者只初始化 LMDB
git submodule update --init engine/3rdparty/lmdb
```

### 方法 2：手动克隆

如果不想使用 git submodule，可以手动克隆 LMDB：

```bash
cd engine/3rdparty
git clone https://github.com/LMDB/lmdb.git
```

## 目录结构

LMDB 的目录结构可能因版本而异。CMake 配置会自动检测以下常见结构：

- `engine/3rdparty/lmdb/libraries/liblmdb/` (常见结构)
- `engine/3rdparty/lmdb/` (根目录)
- `engine/3rdparty/lmdb/libs/liblmdb/` (备选结构)

## 使用示例

### 初始化缓存系统

```cpp
#include "resource/cache/lmdb_derived_data_cache.h"

using namespace Z;

// 创建缓存实例
auto ddc = std::make_unique<LMDBDerivedDataCache>();

// 初始化（缓存路径，最大大小 MB）
std::filesystem::path cache_path = ".zengine/DDC";
if (!ddc->initialize(cache_path, 1024))  // 1GB
{
    // 初始化失败
    return;
}

// 使用缓存
DDCKey key;
key.cache_type = "Texture";
key.asset_guid = "texture_guid_12345";
key.cache_key = "compressed_format_bc7";

DDCValue value;
if (ddc->get(key, value))
{
    // 使用缓存的压缩数据
    loadCompressedTexture(value.data);
}
else
{
    // 生成新数据并缓存
    auto compressed_data = compressTexture(texture_data);
    
    value.data = compressed_data;
    value.timestamp = std::time(nullptr);
    value.version = 1;
    
    ddc->put(key, value);
}

// 关闭缓存
ddc->shutdown();
```

### 缓存键生成

```cpp
// 生成纹理缓存键（包含所有影响结果的参数）
std::string generateTextureCacheKey(const TextureImportSettings& settings)
{
    std::stringstream ss;
    ss << "format:" << settings.format
       << "_compression:" << settings.compression
       << "_mipmaps:" << settings.generate_mipmaps
       << "_srgb:" << settings.srgb
       << "_size:" << settings.max_size;
    
    // 计算哈希值（使用项目的哈希函数）
    return hashString(ss.str());
}
```

## 配置

可以在配置文件中设置缓存参数：

```ini
# engine/configs/development/ZEditor.ini
[DerivedDataCache]
Type=LMDB
Path=.zengine/DDC
MaxSizeMB=1024
```

## 性能特性

- **高性能**：基于内存映射，零拷贝访问
- **ACID 事务**：保证数据一致性
- **并发安全**：支持多读单写（MVCC）
- **跨平台**：支持 Windows、macOS、Linux

## 注意事项

1. **缓存版本管理**：当数据格式变化时，需要更新版本号，旧版本缓存会被自动忽略
2. **缓存清理**：定期调用 `cleanup()` 清理过期缓存
3. **错误处理**：缓存失败时应回退到重新生成数据
4. **路径权限**：确保缓存目录有读写权限

## 故障排除

### LMDB 源文件未找到

如果 CMake 配置时提示找不到 LMDB 源文件：

1. 确认 LMDB 已正确克隆到 `engine/3rdparty/lmdb/`
2. 检查目录结构是否匹配上述常见结构
3. 手动检查 `mdb.c` 和 `midl.c` 文件是否存在

### 编译错误

如果遇到编译错误：

1. 确认 LMDB 头文件 `lmdb.h` 在 include 路径中
2. 检查编译器是否支持 C99（LMDB 是 C 库）
3. 在 Windows 上，确保使用 `/TC` 标志编译 C 文件

## 参考资料

- [LMDB 官方文档](https://www.lmdb.tech/doc/)
- [LMDB GitHub 仓库](https://github.com/LMDB/lmdb)
- [ZEngine 缓存系统设计文档](CACHE_SYSTEM_DESIGN.md)

