# RenderSystem架构设计：Unity风格的单RenderPipeline + 多Camera系统

## 问题背景

在编辑器模式下，如果创建两个独立的RenderSystem（Editor RenderSystem和Runtime RenderSystem），会导致画面闪烁。这是因为：

1. **资源竞争**：两个RenderSystem会竞争同一个RHI（Render Hardware Interface）资源
2. **同步问题**：两个系统同时提交渲染命令会导致命令缓冲区冲突
3. **状态混乱**：两个系统可能同时修改全局渲染状态（如viewport、framebuffer等）

## 解决方案：Unity风格的单RenderPipeline + 多Camera架构

### 核心原则

**RenderSystem只能有一个实例**，采用**Unity风格的多Camera系统**实现Scene视图和Game视图的差异化处理，并天然支持画中画等高级功能。

### 架构设计（Unity风格）

```
┌─────────────────────────────────────────┐
│         RenderSystem (单例)             │
│  ┌───────────────────────────────────┐  │
│  │      RenderPipeline (单例)        │  │
│  │  ┌─────────────────────────────┐ │  │
│  │  │   MainCameraPass            │ │  │
│  │  │   - drawCameraToTexture()   │ │  │
│  │  └─────────────────────────────┘ │  │
│  └───────────────────────────────────┘  │
│                                         │
│  ┌───────────────────────────────────┐  │
│  │   Camera List (多Camera)          │  │
│  │   - EditorCamera                 │  │
│  │     targetTexture: "scene_rt"    │  │
│  │   - GameCamera                   │  │
│  │     targetTexture: "game_rt"    │  │
│  │   - PictureInPictureCamera      │  │
│  │     targetTexture: "pip_rt"      │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
    ┌─────────┐         ┌─────────┐         ┌─────────┐
    │ Scene   │         │ Game    │         │ 画中画   │
    │ Camera  │         │ Camera  │         │ Camera  │
    │ -> RT   │         │ -> RT   │         │ -> RT   │
    └─────────┘         └─────────┘         └─────────┘
```

### 关键组件

1. **单RenderSystem实例**
   - 在`RuntimeGlobalContext`中创建
   - Editor和Runtime共享同一个实例

2. **单RenderPipeline实例**
   - 全局只有一个`RenderPipeline`实例
   - 所有Camera共享同一个渲染管线

3. **多Camera系统（Unity风格）**
   - 每个Camera可以设置`targetTexture`（类似Unity的`Camera.targetTexture`）
   - 如果`targetTexture`为空，Camera渲染到屏幕/swapchain
   - 如果`targetTexture`不为空，Camera渲染到指定的RenderTexture
   - 每个Camera有独立的渲染设置（FOV、Aspect、View/Projection矩阵等）

4. **Camera管理**
   - `RenderSystem::addCamera()` - 添加Camera
   - `RenderSystem::getCamera()` - 获取指定Camera
   - `RenderSystem::getAllCameras()` - 获取所有Camera
   - `RenderCamera::setTargetTexture()` - 设置Camera的渲染目标

## Unity vs Unreal：为什么选择Unity方式？

### Unity的做法（已采用）✅

Unity使用**单RenderPipeline + 多Camera**的架构：

1. **单RenderPipeline实例**
   - 全局只有一个`RenderPipeline`实例
   - 所有Camera共享同一个渲染管线

2. **多Camera系统**
   - Scene视图使用`SceneView.camera`（Editor Camera）
   - Game视图使用场景中的`Camera.main`或指定的Camera
   - 每个Camera可以有不同的渲染设置（Culling Mask、Clear Flags等）
   - **关键特性**：每个Camera可以设置`targetTexture`，实现画中画等功能

3. **渲染流程**
   ```csharp
   // Unity伪代码
   foreach (Camera camera in cameras) {
       if (camera.targetTexture != null) {
           // 渲染到RenderTexture（多视口、画中画等）
           RenderPipeline.Render(camera, camera.targetTexture);
       } else {
           // 渲染到屏幕
           RenderPipeline.Render(camera, screen);
       }
   }
   ```

### Unreal的做法（未采用）

Unreal使用**单World + 多Viewport**的架构：

1. **单World实例**
   - Editor和Game模式共享同一个`UWorld`
   - 所有视口共享同一个场景数据

2. **多Viewport系统**
   - Scene视口使用`FEditorViewportClient`（Editor Viewport）
   - Game视口使用`UGameViewportClient`（Game Viewport）
   - 每个Viewport有独立的`FSceneView`和`FViewInfo`

3. **渲染流程**
   ```cpp
   // Unreal伪代码
   for (FViewport* Viewport : Viewports) {
       FSceneView* View = Viewport->CreateSceneView();
       Renderer->Render(View, Viewport->GetRenderTarget());
   }
   ```

### 为什么Unity方式更优？

1. **画中画支持** ⭐
   - Unity方式：天然支持，只需创建新Camera并设置`targetTexture`
   - Unreal方式：需要创建新Viewport，概念上更复杂

2. **灵活性**
   - Unity方式：Camera是场景对象，可以动态创建/销毁，易于管理
   - Unreal方式：Viewport更多是编辑器概念，运行时创建较复杂

3. **直观性**
   - Unity方式：`Camera.targetTexture`概念清晰，符合直觉
   - Unreal方式：Viewport-Camera映射需要额外抽象层

4. **扩展性**
   - Unity方式：易于添加新Camera（如安全摄像头、后视镜等）
   - Unreal方式：需要管理Viewport生命周期，相对复杂

## 实现方案（Unity风格）

### 1. RenderCamera添加targetTexture支持

```cpp
class RenderCamera {
public:
    // Unity-style: Camera.targetTexture support
    std::string m_target_texture_id; // ID of the RenderTexture to render to (empty = render to screen)
    
    void setTargetTexture(const std::string& texture_id) { m_target_texture_id = texture_id; }
    const std::string& getTargetTexture() const { return m_target_texture_id; }
    bool hasTargetTexture() const { return !m_target_texture_id.empty(); }
};
```

### 2. RenderSystem管理多个Camera

```cpp
class RenderSystem {
private:
    std::map<std::string, std::shared_ptr<RenderCamera>> m_cameras; // Camera ID -> Camera mapping
    
public:
    void addCamera(const std::string& camera_id, std::shared_ptr<RenderCamera> camera);
    std::shared_ptr<RenderCamera> getCamera(const std::string& camera_id) const;
    void removeCamera(const std::string& camera_id);
    std::vector<std::shared_ptr<RenderCamera>> getAllCameras() const;
};
```

### 3. RenderPipeline遍历所有Camera

```cpp
void RenderPipeline::forwardRender(...) {
    // Unity-style: Render all cameras
    RenderSystem* render_system = g_runtime_global_context.m_render_system;
    std::vector<std::shared_ptr<RenderCamera>> cameras = render_system->getAllCameras();
    
    for (auto& camera : cameras) {
        if (camera->hasTargetTexture()) {
            // Render camera to its target RenderTexture
            main_camera_pass->drawCameraToTexture(camera, camera->getTargetTexture(), ...);
        } else {
            // Render camera to screen/swapchain
            main_camera_pass->drawForward(...);
        }
    }
}
```

### 4. MainCameraPass支持Camera-centric渲染

```cpp
void MainCameraPass::drawCameraToTexture(std::shared_ptr<RenderCamera> camera,
                                        const std::string& texture_id,
                                        ...) {
    // Get RenderTexture
    auto* rt = rhi->getViewportRenderTexture(texture_id);
    
    // Update camera aspect based on RenderTexture dimensions
    camera->setAspect(rt->width / rt->height);
    
    // Render using camera's view/projection matrices
    // ...
}
```

### 5. 使用示例：画中画功能

```cpp
// 创建画中画Camera
auto pip_camera = std::make_shared<RenderCamera>();
pip_camera->setTargetTexture("pip_render_texture"); // 设置渲染目标
render_system->addCamera("pip_camera", pip_camera);

// 在游戏循环中更新Camera位置
pip_camera->setPosition(pip_position);
pip_camera->lookAt(pip_target, pip_up);

// RenderPipeline会自动渲染所有Camera，包括画中画Camera
```

### 6. Scene视图和Game视图的差异化

**Scene视图（Editor模式）**：
- 创建`EditorCamera`，设置`targetTexture`为`"scene_rt"`
- 使用`EditorCamera`（可自由移动、旋转）
- 显示Gizmos、网格线等编辑器辅助元素

**Game视图（Runtime模式）**：
- 创建`GameCamera`，设置`targetTexture`为`"game_rt"`
- 使用场景中的`GameCamera`（受游戏逻辑控制）
- 不显示编辑器辅助元素

## 优势（Unity方式）

1. **避免闪烁**：单RenderSystem避免了资源竞争
2. **性能优化**：共享渲染资源（Shader、Texture等）
3. **灵活性**：每个Camera可以有不同的渲染设置（FOV、Aspect、View/Projection等）
4. **画中画支持** ⭐：天然支持，只需创建新Camera并设置`targetTexture`
5. **易于扩展**：可以轻松添加新Camera（安全摄像头、后视镜、监控画面等）
6. **直观性**：`Camera.targetTexture`概念清晰，符合直觉
7. **一致性**：与Unity的主流做法一致

## 画中画应用场景

1. **游戏内画中画**
   - 后视镜（赛车游戏）
   - 监控摄像头（恐怖游戏）
   - 小地图3D视图
   - 武器瞄准镜

2. **编辑器功能**
   - 多视角预览
   - 场景分屏显示
   - 调试视图

3. **运行时功能**
   - 分屏多人游戏
   - 安全摄像头系统
   - 实时回放

## 注意事项

1. **Camera管理**：确保Camera正确创建和销毁，避免内存泄漏
2. **资源管理**：每个Camera的RenderTexture需要正确创建和销毁
3. **性能考虑**：多Camera渲染会增加GPU负担，需要合理控制Camera数量
4. **可见性剔除**：当前实现使用主Camera的可见节点，未来可以优化为每个Camera独立的可见性剔除

## 总结

- ✅ **RenderSystem只能有一个实例**
- ✅ **采用Unity风格的单RenderPipeline + 多Camera架构**
- ✅ **每个Camera可以设置targetTexture（类似Unity的Camera.targetTexture）**
- ✅ **天然支持画中画等高级功能**
- ✅ **比Unreal的Viewport方式更灵活、更直观**

