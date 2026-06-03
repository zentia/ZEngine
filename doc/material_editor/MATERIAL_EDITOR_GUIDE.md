# ZEngine 材质编辑器使用指南

## 概述

ZEngine 材质编辑器是一个基于节点图的材质编辑工具，参考了 Unity Shader Graph 和 Unreal Engine 材质编辑器的设计理念。它允许用户通过可视化节点图的方式创建和编辑材质，而无需直接编写着色器代码。

## 功能特性

### 核心功能

1. **节点图编辑器**
   - 可视化的节点连接界面
   - 支持拖拽节点和连接
   - 网格背景和缩放支持
   - 中键拖拽画布

2. **节点类型**
   - **输入节点**：Texture2D, TextureCube, Constant, Constant2/3/4, Time, UV
   - **数学运算节点**：Add, Subtract, Multiply, Divide, Lerp, Clamp, Saturate 等
   - **向量运算节点**：Combine, Split, Swizzle
   - **纹理采样节点**：Sample Texture2D, Sample TextureCube
   - **材质输出节点**：Base Color, Metallic, Roughness, Normal, Emissive, Occlusion, Alpha

3. **数据类型**
   - Float（浮点数）
   - Float2（2D向量）
   - Float3（3D向量，如RGB、XYZ）
   - Float4（4D向量，如RGBA、XYZW）
   - Texture2D（2D纹理）
   - TextureCube（立方体贴图）
   - Bool（布尔值）
   - Int（整数）

4. **属性面板**
   - 选中节点后显示可编辑属性
   - 支持实时编辑常量值
   - 纹理路径输入

5. **预览面板**
   - 材质实时预览（待实现）
   - 可调节预览大小

## 使用方法

### 打开材质编辑器

1. 启动 ZEngine 编辑器
2. 在菜单栏选择 `Window` -> `Material Editor`
3. 材质编辑器窗口将打开

### 创建节点

1. 在节点图区域右键点击
2. 从上下文菜单中选择节点类型：
   - **Input**：输入节点（纹理、常量、时间等）
   - **Math**：数学运算节点
   - **Vector**：向量运算节点
   - **Texture**：纹理采样节点

### 连接节点

1. 点击输出节点的输出引脚（右侧）
2. 拖拽到目标输入节点的输入引脚（左侧）
3. 释放鼠标完成连接

### 编辑节点属性

1. 点击选中节点
2. 在右侧属性面板中编辑：
   - **Constant 节点**：直接拖拽数值滑块
   - **Texture2D 节点**：输入纹理路径
   - 其他节点：根据节点类型显示相应属性

### 删除节点/连接

- **删除节点**：选中节点后按 `Delete` 键，或右键菜单选择 "Delete Selected Node"
- **删除连接**：右键点击连接线，选择 "Delete Link"

### 移动节点

- 点击并拖拽节点标题栏移动节点位置

### 移动画布

- 按住中键（鼠标滚轮）并拖拽移动画布视图

## 节点说明

### 输入节点

#### Texture2D
- **输出**：Texture2D
- **用途**：提供2D纹理资源
- **属性**：纹理路径

#### Constant / Constant2 / Constant3 / Constant4
- **输出**：Float / Float2 / Float3 / Float4
- **用途**：提供常量值
- **属性**：数值（可在节点上直接编辑）

#### Time
- **输出**：Float
- **用途**：提供时间值（用于动画效果）

#### UV
- **输出**：Float2
- **用途**：提供UV坐标

### 数学运算节点

#### Add / Subtract / Multiply / Divide
- **输入**：Float A, Float B
- **输出**：Float
- **用途**：基本数学运算

#### Lerp
- **输入**：Float3 A, Float3 B, Float T
- **输出**：Float3
- **用途**：线性插值

#### Clamp
- **输入**：Float Value, Float Min, Float Max
- **输出**：Float
- **用途**：将值限制在范围内

#### Saturate
- **输入**：Float Value
- **输出**：Float
- **用途**：将值限制在 [0, 1] 范围内

### 纹理采样节点

#### Sample Texture2D
- **输入**：Texture2D Texture, Float2 UV
- **输出**：Float4 RGBA
- **用途**：采样2D纹理

### 材质输出节点

#### Base Color
- **输入**：Float3 Base Color
- **用途**：设置材质的基础颜色

#### Metallic
- **输入**：Float Metallic
- **用途**：设置材质的金属度（0-1）

#### Roughness
- **输入**：Float Roughness
- **用途**：设置材质的粗糙度（0-1）

#### Normal
- **输入**：Float3 Normal
- **用途**：设置法线贴图

## 示例：创建简单材质

### 示例1：基础颜色材质

1. 添加 `Constant3` 节点
2. 设置颜色值（例如：1.0, 0.5, 0.0 表示橙色）
3. 连接 `Constant3` 的输出到 `Base Color` 节点的输入

### 示例2：纹理材质

1. 添加 `Texture2D` 节点
2. 设置纹理路径
3. 添加 `UV` 节点
4. 添加 `Sample Texture2D` 节点
5. 连接 `Texture2D` 到 `Sample Texture2D` 的 Texture 输入
6. 连接 `UV` 到 `Sample Texture2D` 的 UV 输入
7. 连接 `Sample Texture2D` 的 RGBA 输出到 `Base Color` 的输入（需要 Split 节点提取 RGB）

### 示例3：混合材质

1. 创建两个纹理采样节点（Texture A 和 Texture B）
2. 添加 `Lerp` 节点
3. 连接 Texture A 到 Lerp 的 A 输入
4. 连接 Texture B 到 Lerp 的 B 输入
5. 添加 `Constant` 节点作为混合因子（T）
6. 连接 Constant 到 Lerp 的 T 输入
7. 连接 Lerp 的输出到 Base Color

## 快捷键

- `Delete`：删除选中的节点
- `中键拖拽`：移动画布
- `左键拖拽`：移动节点
- `右键点击`：显示上下文菜单

## 未来计划

- [ ] 材质图序列化和反序列化（保存/加载材质）
- [ ] 实时材质预览渲染
- [ ] 更多数学运算节点
- [ ] 更多向量运算节点
- [ ] 条件节点（If/Else）
- [ ] 材质实例化支持
- [ ] 节点分组和注释
- [ ] 撤销/重做功能
- [ ] 节点搜索功能
- [ ] 材质参数暴露（Material Parameters）
- [ ] 着色器代码生成

## 技术实现

材质编辑器基于以下技术：

- **UI框架**：ImGui
- **节点图系统**：自定义实现
- **渲染系统**：ZEngine 渲染管线

## 注意事项

1. 当前版本为初始实现，部分功能仍在开发中
2. 材质预览功能需要渲染系统支持，目前为占位实现
3. 材质图的保存/加载功能尚未实现
4. 某些高级节点类型可能尚未完全实现

## 参考

- Unity Shader Graph 文档
- Unreal Engine Material Editor 文档

