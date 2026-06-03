# ZEngine LLM 内存追踪器设计文档

## 概述

本文档描述了 Unreal Engine (UE) 的 LLM (Low-Level Memory) 追踪数据展示方式，以及 ZEngine 应该如何设计类似的功能。

## UE 的 LLM 数据展示方式

### 1. 控制台命令展示

UE 主要通过控制台命令来展示 LLM 数据：

- **`stat llm`**: 显示简化的 LLM 统计信息，包括主要内存标签的使用情况
- **`stat llmfull`**: 显示完整的 LLM 统计信息，包括所有标记的内存使用情况
- **`stat LLMPlatform`**: 显示平台级别的内存统计信息
- **`stat LLMOverhead`**: 显示 LLM 系统本身的开销统计信息

### 2. 数据格式

UE 的 LLM 统计信息通常以表格形式显示，包含以下列：
- **Tag Name**: 内存标签名称（如 "Rendering", "Physics", "Audio" 等）
- **Current**: 当前分配的内存大小
- **Peak**: 峰值内存使用量
- **Allocations**: 分配次数
- **Size**: 每次分配的平均大小

### 3. CSV 导出功能

UE 支持通过 `-LLMCSV` 命令行参数将内存使用数据导出为 CSV 格式：
- 数据保存在 `Saved/Profiling/LLM/` 目录
- 默认每 5 秒记录一次（可通过 `LLM.LLMWriteInterval` 配置）
- 便于后续分析和可视化

### 4. 实时更新

UE 的 LLM 统计信息在游戏运行时实时更新，开发者可以随时查看当前的内存使用情况。

## ZEngine 的设计方案

### 1. 编辑器窗口展示（主要方式）

ZEngine 应该提供一个专门的编辑器窗口来展示 LLM 内存追踪数据，类似于其他编辑器窗口（如 ConsoleWindow、InspectorWindow）。

#### 窗口功能特性

1. **实时数据表格**
   - 使用 ImGui 的表格组件显示所有内存标签的统计信息
   - 支持按不同列排序（当前内存、峰值内存、分配次数等）
   - 支持搜索/过滤功能，快速定位特定标签

2. **数据列**
   - **Tag Name**: 标签名称
   - **Current (MB)**: 当前内存使用量（MB）
   - **Peak (MB)**: 峰值内存使用量（MB）
   - **Total Allocated (MB)**: 累计分配总量
   - **Total Freed (MB)**: 累计释放总量
   - **Allocations**: 分配次数
   - **Deallocations**: 释放次数
   - **Avg Size (KB)**: 平均分配大小

3. **可视化元素**
   - 进度条显示每个标签的内存使用占比（相对于总内存）
   - 颜色编码：根据内存使用量使用不同颜色（绿色=低，黄色=中，红色=高）
   - 可选的图表视图（折线图显示内存使用趋势）

4. **控制功能**
   - **刷新按钮**: 手动刷新数据
   - **自动刷新**: 可配置的自动刷新间隔（如每 0.5 秒）
   - **清除统计**: 重置所有统计数据
   - **启用/禁用追踪**: 动态开启或关闭内存追踪

5. **导出功能**
   - **导出为 CSV**: 将当前统计数据导出为 CSV 文件
   - **导出为 JSON**: 导出为 JSON 格式，便于程序化处理

6. **总览信息**
   - 窗口顶部显示总体统计：
     - 总当前内存使用量
     - 总峰值内存使用量
     - 活跃标签数量
     - 总分配次数

### 2. 控制台命令支持（可选）

为了与 UE 保持一致，也可以添加控制台命令支持：
- `stat llm`: 在控制台窗口显示简化的 LLM 统计
- `stat llmfull`: 显示完整的 LLM 统计

### 3. 实现细节

#### 窗口类设计

```cpp
class LLMMemoryTrackerWindow : public EditorWindow
{
public:
    explicit LLMMemoryTrackerWindow(EditorUI* editor_ui);
    virtual void onGUI() override;

private:
    void drawOverviewSection();
    void drawTableSection();
    void drawControlsSection();
    void exportToCSV();
    void exportToJSON();
    
    // UI 状态
    bool m_auto_refresh {true};
    float m_refresh_interval {0.5f}; // 秒
    float m_last_refresh_time {0.0f};
    char m_search_filter[256];
    int m_sort_column {1}; // 默认按当前内存排序
    bool m_sort_descending {true};
    
    // 缓存的统计数据
    std::vector<LLMTagStats> m_cached_stats;
    size_t m_cached_total_current {0};
    size_t m_cached_total_peak {0};
};
```

#### 数据格式化工具

需要提供工具函数来格式化内存大小：
- `formatBytes(size_t bytes)`: 将字节数格式化为人类可读的字符串（B, KB, MB, GB）
- `bytesToMB(size_t bytes)`: 转换为 MB
- `bytesToKB(size_t bytes)`: 转换为 KB

#### 窗口注册

在 `EditorUI::registerAllEditorWindow()` 中注册新窗口：
```cpp
registerEditorWindow<LLMMemoryTrackerWindow>();
```

### 4. 性能考虑

1. **数据缓存**: 避免每帧都调用 `getAllTagStats()`，使用缓存机制
2. **异步更新**: 可以考虑在后台线程更新统计数据，避免阻塞主线程
3. **按需刷新**: 只在窗口可见时更新数据

### 5. 用户体验优化

1. **搜索高亮**: 搜索时高亮匹配的标签名称
2. **列宽自适应**: 根据内容自动调整列宽
3. **列排序指示**: 显示当前排序的列和方向（↑↓）
4. **工具提示**: 鼠标悬停时显示详细信息
5. **键盘快捷键**: 支持快捷键操作（如 F5 刷新）

## 与 UE 的对比

| 特性 | UE | ZEngine |
|------|-----|---------|
| 主要展示方式 | 控制台命令 | 编辑器窗口 |
| 实时更新 | ✅ | ✅ |
| 数据表格 | ✅ | ✅ |
| CSV 导出 | ✅ | ✅ |
| 可视化图表 | ❌ | ✅ (计划) |
| 搜索过滤 | ❌ | ✅ |
| 排序功能 | ❌ | ✅ |

## 总结

ZEngine 的 LLM 内存追踪器设计在借鉴 UE 的基础上，提供了更现代化的编辑器窗口界面，具有更好的交互性和可视化效果。通过 ImGui 的强大功能，可以实现比 UE 控制台命令更友好的用户体验。

