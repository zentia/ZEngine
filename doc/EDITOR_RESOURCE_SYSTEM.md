# ZEngine 编辑器资源系统设计文档

## 概述

本文档描述 ZEngine 编辑器资源加载系统的设计，参考 Unreal Engine 的编辑器资源管理机制。

---

## Unreal Engine 编辑器资源加载机制

### 核心特性

1. **资源路径系统（Slate Style）**
   - UE 使用 **Slate UI 框架**，资源通过路径系统管理
   - 资源路径格式：`/Engine/Editor/Icons/IconName.png`
   - 使用 `FSlateStyleSet` 管理资源集合
   - 支持资源别名和路径映射

2. **资源加载方式**
   - **编译时嵌入**：资源可以编译到引擎二进制文件中
   - **运行时加载**：从文件系统按需加载
   - **资源缓存**：加载的资源会被缓存，避免重复加载

3. **资源类型**
   - **图标（Icons）**：PNG、SVG 格式
   - **字体（Fonts）**：TTF、OTF 格式
   - **样式（Styles）**：颜色、尺寸等 UI 样式定义
   - **纹理（Textures）**：UI 背景、按钮等

4. **资源组织**
   ```
   Engine/
   ├── Content/
   │   └── Editor/
   │       ├── Icons/
   │       ├── Fonts/
   │       └── Styles/
   └── Slate/
       └── Editor/
           └── Styles/
   ```

5. **使用示例**
   ```cpp
   // UE 中加载图标
   FSlateIcon Icon = FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.Viewports");
   
   // 在 UI 中使用
   SNew(SButton)
       .ButtonStyle(FEditorStyle::Get(), "PropertyEditor.AssetComboStyle")
       .ContentPadding(2.0f)
   ```

### UE 资源系统优势

- ✅ **统一管理**：所有编辑器资源通过统一接口访问
- ✅ **路径抽象**：使用逻辑路径而非物理路径
- ✅ **资源缓存**：自动缓存，提高性能
- ✅ **热重载支持**：支持运行时重新加载资源
- ✅ **多分辨率支持**：自动处理 @2x、@3x 高分辨率资源

---

## ZEngine 编辑器资源系统设计

### 设计原则

1. **简单高效**：适合 ZEngine 的规模
2. **易于使用**：提供简洁的 API
3. **性能优先**：资源缓存，避免重复加载
4. **可扩展性**：支持未来添加更多资源类型

### 架构设计

#### 1. 资源路径系统

**资源路径格式**：`/Editor/Icons/IconName` 或 `Editor.Icons.IconName`

**资源目录结构**：
```
engine/source/editor/resource/
├── icons/
│   ├── play.png
│   ├── pause.png
│   ├── stop.png
│   └── ...
├── fonts/
│   └── ZEngineEditorFont.TTF
└── styles/
    └── default_style.json
```

#### 2. 核心类设计

```cpp
// engine/source/editor/resource/editor_resource_manager.h
namespace Z
{
    // 资源类型
    enum class EditorResourceType
    {
        Icon,
        Font,
        Texture,
        Style
    };

    // 资源句柄（用于 ImGui）
    struct EditorResourceHandle
    {
        ImTextureID texture_id = nullptr;  // ImGui 纹理 ID
        int width = 0;
        int height = 0;
        bool is_valid = false;
    };

    // 编辑器资源管理器
    class EditorResourceManager
    {
    public:
        static EditorResourceManager& getInstance();
        
        // 初始化资源系统
        bool initialize(const std::filesystem::path& resource_root);
        void shutdown();
        
        // 加载图标（返回 ImGui 纹理 ID）
        ImTextureID loadIcon(const std::string& resource_path);
        EditorResourceHandle loadIconHandle(const std::string& resource_path);
        
        // 加载字体
        ImFont* loadFont(const std::string& resource_path, float size_pixels);
        
        // 检查资源是否存在
        bool resourceExists(const std::string& resource_path) const;
        
        // 获取资源完整路径
        std::filesystem::path getResourcePath(const std::string& resource_path) const;
        
        // 清理缓存
        void clearCache();
        
        // 获取资源统计信息
        struct ResourceStats
        {
            size_t loaded_icons_count = 0;
            size_t loaded_fonts_count = 0;
            size_t total_memory_usage = 0;
        };
        ResourceStats getStats() const;

    private:
        // 资源缓存
        struct CachedIcon
        {
            ImTextureID texture_id = nullptr;
            int width = 0;
            int height = 0;
            std::time_t load_time = 0;
        };
        
        std::unordered_map<std::string, CachedIcon> m_icon_cache;
        std::unordered_map<std::string, ImFont*> m_font_cache;
        
        std::filesystem::path m_resource_root;
        
        // 将资源路径转换为文件系统路径
        std::filesystem::path resolveResourcePath(const std::string& resource_path) const;
        
        // 创建 ImGui 纹理
        ImTextureID createImGuiTexture(const std::vector<uint8_t>& image_data, int width, int height);
        
        // 释放 ImGui 纹理
        void releaseImGuiTexture(ImTextureID texture_id);
    };
}
```

#### 3. 资源路径解析

**支持的路径格式**：
- `/Editor/Icons/Play` → `resource/icons/play.png`
- `Editor.Icons.Play` → `resource/icons/play.png`
- `icons/play` → `resource/icons/play.png`（相对路径）

**自动查找规则**：
1. 首先尝试精确路径
2. 如果不存在，尝试添加 `.png` 扩展名
3. 支持高分辨率资源（`play@2x.png`）

#### 4. 使用示例

**在 EditorUI 中初始化**：
```cpp
// editor_ui.cpp
#include "editor/resource/editor_resource_manager.h"

void EditorUI::initialize(WindowUIInitInfo init_info)
{
    // ... 现有代码 ...
    
    // 初始化编辑器资源管理器
    auto& resource_manager = EditorResourceManager::getInstance();
    std::filesystem::path resource_root = config_manager->getRootFolder() / "resource";
    resource_manager.initialize(resource_root);
    
    // 使用资源管理器加载字体（替代原来的方式）
    ImFont* editor_font = resource_manager.loadFont(
        "Editor.Fonts.ZEngineEditorFont", 
        content_scale * 16.0f
    );
    if (editor_font == nullptr)
    {
        // 回退到原来的方式
        io.Fonts->AddFontFromFileTTF(
            config_manager->getEditorFontPath().generic_string().data(), 
            content_scale * 16, nullptr, nullptr);
    }
    
    // ... 其他初始化代码 ...
}
```

**在编辑器窗口中使用图标**：
```cpp
// 在某个编辑器窗口中
void MyEditorWindow::render()
{
    auto& resource_manager = EditorResourceManager::getInstance();
    
    // 加载图标
    ImTextureID play_icon = resource_manager.loadIcon("Editor.Icons.Play");
    if (play_icon)
    {
        if (ImGui::ImageButton(play_icon, ImVec2(16, 16)))
        {
            // 处理点击
        }
        ImGui::SameLine();
    }
    
    // 或使用句柄获取尺寸信息
    auto icon_handle = resource_manager.loadIconHandle("Editor.Icons.Pause");
    if (icon_handle.is_valid)
    {
        ImGui::Image(icon_handle.texture_id, 
                     ImVec2(icon_handle.width, icon_handle.height));
    }
}
```

**支持的资源路径格式**：
```cpp
// 以下路径格式都支持，会解析到 resource/icons/play.png
resource_manager.loadIcon("/Editor/Icons/Play");
resource_manager.loadIcon("Editor.Icons.Play");
resource_manager.loadIcon("icons/play");
resource_manager.loadIcon("icons/Play");  // 自动转换为小写
```

---

## 实现细节

### 1. 资源路径映射

```cpp
std::filesystem::path EditorResourceManager::resolveResourcePath(
    const std::string& resource_path) const
{
    std::string normalized_path = resource_path;
    
    // 标准化路径格式
    // "/Editor/Icons/Play" -> "icons/play"
    // "Editor.Icons.Play" -> "icons/play"
    
    // 移除前缀
    if (normalized_path.starts_with("/Editor/"))
    {
        normalized_path = normalized_path.substr(8);
    }
    else if (normalized_path.starts_with("Editor."))
    {
        normalized_path = normalized_path.substr(7);
        // 替换点号为斜杠
        std::replace(normalized_path.begin(), normalized_path.end(), '.', '/');
    }
    
    // 转换为小写
    std::transform(normalized_path.begin(), normalized_path.end(), 
                   normalized_path.begin(), ::tolower);
    
    // 构建完整路径
    std::filesystem::path full_path = m_resource_root / normalized_path;
    
    // 尝试添加扩展名
    if (!std::filesystem::exists(full_path))
    {
        full_path = full_path.parent_path() / 
                   (full_path.filename().string() + ".png");
    }
    
    return full_path;
}
```

### 2. ImGui 纹理创建

```cpp
ImTextureID EditorResourceManager::createImGuiTexture(
    const std::vector<uint8_t>& image_data, int width, int height)
{
    // 需要与渲染后端集成
    // 这里需要调用 RenderSystem 的接口创建纹理
    // 返回 ImTextureID（通常是 void* 或 uint64_t）
    
    // 示例（需要根据实际渲染后端实现）：
    // return render_system->createImGuiTexture(image_data.data(), width, height);
    return nullptr;
}
```

### 3. 资源缓存策略

- **LRU 缓存**：最近最少使用的资源会被优先清理
- **内存限制**：设置最大缓存大小，超出时清理
- **按需加载**：资源在首次使用时加载
- **延迟释放**：资源在不再使用时延迟释放（避免频繁加载/卸载）

---

## 与现有系统集成

### 1. 替换现有资源加载

**当前方式**（`editor_ui.cpp`）：
```cpp
std::string big_icon_path = config_manager->getEditorBigIconPath().generic_string();
window_icon[0].pixels = stbi_load(big_icon_path.data(), ...);
```

**新方式**：
```cpp
auto& resource_manager = EditorResourceManager::getInstance();
auto icon_handle = resource_manager.loadIconHandle("Editor.Icons.WindowBig");
// 使用 icon_handle.texture_id
```

### 2. 配置文件集成

在 `ZEditor.ini` 中添加：
```ini
[EditorResources]
ResourceRoot=resource
EnableCache=true
MaxCacheSizeMB=64
```

### 3. 与 ConfigManager 集成

```cpp
// ConfigManager 中添加
const std::filesystem::path& getEditorResourceRoot() const;
```

---

## 实施计划

### 阶段 1：基础实现（已完成）

1. ✅ 创建 `EditorResourceManager` 类
2. ✅ 实现资源路径解析
3. ✅ 实现图标加载和缓存
4. ✅ 实现字体加载支持
5. ⏳ 集成到 `EditorUI` 初始化流程（待完成）

### 阶段 2：功能完善（1 周）

1. ✅ 添加字体加载支持
2. ✅ 实现资源统计和监控
3. ✅ 添加资源热重载支持（可选）

### 阶段 3：优化和扩展（1 周）

1. ✅ 性能优化（批量加载、异步加载）
2. ✅ 支持 SVG 图标（可选）
3. ✅ 资源打包工具（可选）

---

## 优势对比

| 特性 | 当前实现 | 新设计 |
|------|---------|--------|
| **资源管理** | 分散，通过 ConfigManager | 统一管理 |
| **资源路径** | 物理路径 | 逻辑路径 |
| **资源缓存** | 无 | 有 |
| **易用性** | 需要知道文件路径 | 使用资源名称 |
| **可扩展性** | 低 | 高 |
| **性能** | 每次加载 | 缓存复用 |

---

## 注意事项

1. **ImGui 纹理创建**：需要与渲染后端（Vulkan）集成，确保正确创建和释放纹理
2. **资源路径**：保持路径命名规范，避免冲突
3. **内存管理**：注意资源缓存的内存占用，及时释放不再使用的资源
4. **跨平台**：确保资源路径在不同平台上正确解析
5. **资源打包**：未来可以考虑将资源打包到二进制文件中，减少文件依赖

---

## 参考资料

- [Unreal Engine Slate UI Framework](https://docs.unrealengine.com/en-US/Programming/Slate/)
- [ImGui Texture Loading](https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples)
- [UE4 Editor Style Guide](https://docs.unrealengine.com/en-US/Programming/Slate/StyleGuide/)

