# ZEngine 线程库使用指南

## 概述

ZEngine 现在引入了线程库，实现了类似 Unreal Engine 的多线程渲染架构，包括：
- **游戏线程 (Game Thread)**: 主线程，运行游戏逻辑
- **渲染线程 (Render Thread)**: 准备渲染命令和数据
- **RHI 线程 (RHI Thread)**: 执行实际的 GPU API 调用（Vulkan）

## 架构设计

### 线程类型

1. **游戏线程**: 主线程，不需要显式创建
2. **渲染线程**: 独立的线程，用于准备渲染命令
3. **RHI 线程**: 独立的线程，用于执行 GPU API 调用

### 组件

- `TaskQueue`: 线程安全的任务队列
- `ThreadManager`: 线程管理器，管理所有工作线程
- `RenderingThread`: 渲染线程封装，提供便捷的接口
- `TaskGraph`: 任务图系统，管理任务依赖关系（类似 UE 的任务图）

## 使用方法

### 1. 初始化

线程管理器会在引擎启动时自动初始化，无需手动调用。

### 2. 在渲染线程上执行任务

```cpp
#include "runtime/function/render/rendering_thread/rendering_thread.h"

// 异步执行任务
RenderingThread::enqueueRenderThreadTask([]() {
    // 在渲染线程上执行的代码
    // 准备渲染数据、构建渲染命令等
}, TaskPriority::normal);

// 同步执行任务（等待完成）
RenderingThread::executeOnRenderThread([]() {
    // 在渲染线程上执行的代码
    // 会阻塞直到任务完成
});
```

### 3. 在 RHI 线程上执行任务

```cpp
// 异步执行任务
RenderingThread::enqueueRHITask([]() {
    // 在 RHI 线程上执行的代码
    // 执行 Vulkan API 调用
    vkCmdDraw(...);
}, TaskPriority::high);

// 同步执行任务
RenderingThread::executeOnRHIThread([]() {
    // 在 RHI 线程上执行的代码
    // 会阻塞直到任务完成
});
```

### 4. 检查当前线程

```cpp
// 检查是否在渲染线程
if (RenderingThread::isOnRenderThread()) {
    // 当前在渲染线程上
}

// 检查是否在 RHI 线程
if (RenderingThread::isOnRHIThread()) {
    // 当前在 RHI 线程上
}
```

### 5. 等待任务完成

```cpp
// 等待渲染线程完成所有任务
RenderingThread::waitForRenderThread();

// 等待 RHI 线程完成所有任务
RenderingThread::waitForRHIThread();

// 等待所有渲染相关线程完成
RenderingThread::waitForAll();
```

## 任务优先级

任务支持三种优先级：

- `TaskPriority::low`: 低优先级
- `TaskPriority::normal`: 普通优先级（默认）
- `TaskPriority::high`: 高优先级

```cpp
RenderingThread::enqueueRenderThreadTask([]() {
    // 高优先级任务
}, TaskPriority::high);
```

## 典型使用场景

### 场景 1: 多线程渲染管线（已接入 RenderSystem）

```cpp
// 游戏线程 (Application::TickOneFrame)
GET_SYSTEM(RenderSystem)->SwapLogicRenderData();
GET_SYSTEM(RenderSystem)->Tick(delta_time);  // enqueue only; no per-frame flush

// RenderSystem::Tick (desktop, pipelined)
RenderFramePipeline::WaitForFrameSlot();
BuildRenderSystemFrameCommands(*this, delta_time, frame_draw_list_slot);  // typed ENQUEUE_RENDER_COMMAND list
RenderingThread::SubmitRenderFrame();               // one render-worker batch
RenderFramePipeline::OnFrameSubmitted();

// Inside the batch (RenderSystemFrameCommands.cpp):
//   RenderSyncGameCamera -> RenderProcessSwapData -> RenderUpdateScene
//   -> RenderPreparePassData -> RenderBuildDrawLists (CPU draw-list build)
//   -> RenderDispatchRHICommands
//       -> RHIPrepareContext -> RHISubmitDrawLists (GPU submit only; RHI worker batch)

// Explicit flush (picking, shutdown, readback)
GET_SYSTEM(RenderSystem)->FlushRenderingCommands();
// Mesh picking: RenderSystem::GetGuidOfPickedMesh uses RunSynchronizedGpuReadback
// (flush + ExecuteOnRHIThread) so PickPass GPU readback never runs on the game thread.
GET_SYSTEM(RenderSystem)->GetGuidOfPickedMesh(picked_uv);
```

`max frames in flight` is set from `RHI::GetMaxFramesInFlight()` at `RenderSystem::Initialize` (DX12/Vulkan: 3). The game thread can run `LogicalTick` while up to N-1 GPU frames are still executing; it only blocks in `WaitForFrameSlot()` when submitting a new frame would exceed the cap.

Editor: ImGui widget build stays on the game thread (GLFW input). `EditorUI::PrepareGameThreadImGuiFrame()` runs before `TickOneFrame`; `EditorUIPass::Draw` on the RHI worker only submits GPU draw data.

### Editor sync points (picking / GPU readback)

Any game-thread code that touches the GPU synchronously must either flush in-flight frames or marshal onto the RHI worker:

| API | Behavior |
|-----|----------|
| `RenderSystem::FlushRenderingCommands()` | Game thread only. Drains render/RHI workers, waits for pipelined frames, clears stale command queues. |
| `RenderSystem::RunSynchronizedGpuReadback(fn)` | When parallel: `FlushRenderingCommands()` then `ExecuteOnRHIThread(fn)`. When single-threaded: runs `fn` inline. |
| `RenderSystem::GetGuidOfPickedMesh(uv)` | Uses `RunSynchronizedGpuReadback`; `PickPass::Pick` asserts `CHECK_RHI_THREAD`. |

Use `RunSynchronizedGpuReadback` for future editor readbacks (buffer `Map`, `CmdCopyImageToBuffer`, etc.) instead of calling RHI APIs directly from input/UI code.

## ENQUEUE_RENDER_COMMAND

UE-style typed render commands replace whole-frame lambdas. Each command is a small struct implementing `IRenderCommand`:

```cpp
#include "Runtime/Function/Render/RenderingThread/RenderingThread.h"

struct MyRenderCmd final : IRenderCommand
{
    RenderSystem* System {nullptr};
    explicit MyRenderCmd(RenderSystem* system) : System(system) {}

    void Execute() override { /* render-thread work */ }
    const char* GetDebugName() const override { return "MyRender"; }
};

// Game thread (comma-separated constructor args):
ENQUEUE_RENDER_COMMAND(MyRenderCmd, render_system_ptr);
RenderingThread::SubmitRenderFrame();

// RHI commands (usually from a render-thread tail command):
ENQUEUE_RHI_COMMAND(RHISubmitDrawListsCmd, render_system_ptr, frame_draw_list_slot);
RenderingThread::SubmitRHICommandBatch();
```

Render thread builds `RHIDrawList` entries (deferred pass `Draw()` lambdas) in `RenderPipeline::BuildDrawLists`. RHI thread runs fence wait, command-pool reset, `PrepareBeforePass`, executes the draw list, then `SubmitRendering` in `RenderPipeline::SubmitDrawLists`. Three ring-buffered `RHIDrawList` slots match `RenderFramePipeline` max in-flight frames.

- `EnqueueRenderCommand` / `EnqueueRHICommand`: append to a per-thread pending list (mutex-protected).
- `SubmitRenderFrame`: steal the render list and dispatch **one** render-worker task that runs each command in order (with `Z_PROFILE_SCOPE` per command name).
- `SubmitRHICommandBatch`: steal the RHI list, run on the RHI worker, then `RenderFramePipeline::OnFrameCompleted()`.
- `FlushRenderingCommands`: drain worker queues, wait for pipelined frames, clear any stale pending commands.

### Thread-affinity debug checks

`RenderThreadChecks.h` defines debug-only macros (no-op in Release, skipped when parallel rendering is off):

```cpp
#include "Runtime/Function/Render/RenderingThread/RenderThreadChecks.h"

CHECK_GAME_THREAD();    // game thread enqueues render commands, frame pipeline wait/submit
CHECK_RENDER_THREAD();  // BuildDrawLists, PreparePassData, ExecuteRenderCommands
CHECK_RHI_THREAD();     // SubmitDrawLists, RHIDrawList::ExecuteAll, ExecuteRHICommands
```

Violations trip `ASSERT` in Debug builds.

### 场景 2: 异步资源加载

```cpp
// 游戏线程
void loadTextureAsync(const std::string& path) {
    RenderingThread::enqueueRenderThreadTask([path]() {
        // 在渲染线程上加载纹理数据
        TextureData data = loadTextureData(path);
        
        // 提交到 RHI 线程创建 GPU 资源
        RenderingThread::enqueueRHITask([data]() {
            // 在 RHI 线程上创建 Vulkan 纹理
            createVulkanTexture(data);
        });
    });
}
```

### 场景 3: 线程安全的数据访问

```cpp
// 确保在正确的线程上访问数据
void updateRenderData() {
    if (RenderingThread::isOnRenderThread()) {
        // 直接访问，已经在渲染线程上
        m_render_data.update();
    } else {
        // 切换到渲染线程
        RenderingThread::executeOnRenderThread([this]() {
            m_render_data.update();
        });
    }
}
```

### 场景 4: 使用任务图管理任务依赖关系（推荐）

任务图系统类似 Unreal Engine 的任务图，可以自动管理跨线程的任务依赖关系：

```cpp
#include "runtime/function/render/rendering_thread/rendering_thread.h"

void RenderSystem::tick(float delta_time) {
    // 创建任务图
    auto graph = RenderingThread::createTaskGraph();
    
    // 1. 在渲染线程上准备渲染数据
    TaskHandle prepare_task = graph->addTask(
        ThreadType::render,
        [this]() {
            m_render_resource->updatePerFrameBuffer(m_render_scene, m_render_camera);
            m_render_scene->updateVisibleObjects(m_render_resource, m_render_camera);
        },
        TaskPriority::normal,
        "PrepareRenderData"
    );
    
    // 2. 在渲染线程上准备渲染通道数据（依赖 prepare_task）
    TaskHandle prepare_pass_task = graph->addTask(
        ThreadType::render,
        [this]() {
            m_render_pipeline->preparePassData(m_render_resource);
        },
        TaskPriority::normal,
        "PreparePassData"
    );
    graph->addDependency(prepare_pass_task, prepare_task);
    
    // 3. 在 RHI 线程上执行渲染（依赖 prepare_pass_task）
    TaskHandle render_task = graph->addTask(
        ThreadType::rhi,
        [this]() {
            m_render_pipeline->forwardRender(m_rhi, m_render_resource);
        },
        TaskPriority::high,
        "ExecuteRender"
    );
    graph->addDependency(render_task, prepare_pass_task);
    
    // 执行任务图（异步）
    RenderingThread::executeTaskGraph(graph.get());
    
    // 如果需要等待完成
    // RenderingThread::executeTaskGraphAndWait(graph.get());
}
```

### 场景 5: 复杂的跨线程任务依赖

```cpp
void loadAndRenderTexture(const std::string& path) {
    auto graph = RenderingThread::createTaskGraph();
    
    // 1. 在渲染线程上加载纹理数据
    TaskHandle load_task = graph->addTask(
        ThreadType::render,
        [path, &texture_data]() {
            texture_data = loadTextureData(path);
        },
        TaskPriority::normal,
        "LoadTexture"
    );
    
    // 2. 在 RHI 线程上创建 GPU 纹理（依赖 load_task）
    TaskHandle create_task = graph->addTask(
        ThreadType::rhi,
        [&texture_data]() {
            createVulkanTexture(texture_data);
        },
        TaskPriority::high,
        "CreateGPUTexture"
    );
    graph->addDependency(create_task, load_task);
    
    // 3. 在渲染线程上绑定纹理（依赖 create_task）
    TaskHandle bind_task = graph->addTask(
        ThreadType::render,
        [this]() {
            bindTextureToShader();
        },
        TaskPriority::normal,
        "BindTexture"
    );
    graph->addDependency(bind_task, create_task);
    
    // 执行并等待完成
    RenderingThread::executeTaskGraphAndWait(graph.get());
}
```

## 注意事项

1. **线程安全**: 确保在正确的线程上访问数据
   - RHI 对象（如 Vulkan 资源）只能在 RHI 线程上访问
   - 渲染数据通常在渲染线程上准备

2. **死锁**: 避免在同步调用中嵌套同步调用
   ```cpp
   // 错误示例：可能导致死锁
   RenderingThread::executeOnRenderThread([]() {
       RenderingThread::executeOnRHIThread([]() {
           // ...
       });
   });
   ```

3. **生命周期**: 确保 lambda 捕获的对象在任务执行时仍然有效
   ```cpp
   // 使用 shared_ptr 或确保对象生命周期
   auto data = std::make_shared<RenderData>();
   RenderingThread::enqueueRenderThreadTask([data]() {
       // data 在任务执行时仍然有效
   });
   ```

4. **性能**: 
   - 小任务使用异步执行
   - 大任务考虑拆分
   - 频繁的同步调用会影响性能

## API 参考

### ThreadManager

全局线程管理器实例：`g_thread_manager`

```cpp
// 检查线程管理器是否初始化
if (g_thread_manager) {
    // 使用线程管理器
}
```

### RenderingThread 静态方法

- `enqueueRenderThreadTask()`: 在渲染线程上异步执行任务
- `enqueueRHITask()`: 在 RHI 线程上异步执行任务
- `executeOnRenderThread()`: 在渲染线程上同步执行任务
- `executeOnRHIThread()`: 在 RHI 线程上同步执行任务
- `waitForRenderThread()`: 等待渲染线程完成
- `waitForRHIThread()`: 等待 RHI 线程完成
- `waitForAll()`: 等待所有线程完成
- `isOnRenderThread()`: 检查是否在渲染线程
- `isOnRHIThread()`: 检查是否在 RHI 线程
- `createTaskGraph()`: 创建任务图（用于管理任务依赖）
- `executeTaskGraph()`: 执行任务图（异步）
- `executeTaskGraphAndWait()`: 执行任务图并等待完成（同步）

### TaskGraph 方法

- `addTask()`: 添加任务节点，返回任务句柄
- `addDependency()`: 添加任务依赖关系
- `addDependencies()`: 添加多个依赖关系
- `compile()`: 编译任务图（检查依赖关系）
- `execute()`: 执行任务图（异步）
- `wait()`: 等待任务图执行完成
- `waitFor()`: 等待任务图执行完成（带超时）
- `isCompleted()`: 检查任务图是否已完成
- `reset()`: 重置任务图（用于重复使用）
- `clear()`: 清空所有任务
- `getTaskState()`: 获取任务状态
- `getTaskName()`: 获取任务名称（用于调试）

## 任务图系统（Task Graph）

任务图系统是 ZEngine 的核心特性，用于管理渲染线程和 RHI 线程之间的任务依赖关系。它类似于 Unreal Engine 的任务图系统。

### 优势

1. **自动依赖管理**: 系统自动处理任务之间的依赖关系，确保任务按正确顺序执行
2. **跨线程支持**: 支持渲染线程和 RHI 线程之间的依赖关系
3. **循环检测**: 自动检测并报告循环依赖错误
4. **拓扑排序**: 自动计算任务执行顺序
5. **线程安全**: 所有操作都是线程安全的

### 任务图使用流程

1. **创建任务图**: 使用 `RenderingThread::createTaskGraph()` 创建
2. **添加任务**: 使用 `addTask()` 添加任务节点，指定线程类型和优先级
3. **定义依赖**: 使用 `addDependency()` 定义任务之间的依赖关系
4. **执行任务图**: 使用 `execute()` 或 `executeTaskGraphAndWait()` 执行

### 任务状态

- `Pending`: 等待执行（依赖未满足）
- `Ready`: 就绪（依赖已满足，可以执行）
- `Running`: 正在执行
- `Completed`: 已完成
- `Failed`: 执行失败

### 示例：渲染管线任务图

```cpp
void RenderSystem::renderFrame() {
    auto graph = RenderingThread::createTaskGraph();
    
    // 任务1: 更新可见物体（渲染线程）
    TaskHandle update_visibility = graph->addTask(
        ThreadType::render,
        [this]() { updateVisibility(); },
        TaskPriority::normal,
        "UpdateVisibility"
    );
    
    // 任务2: 准备渲染资源（渲染线程，依赖 update_visibility）
    TaskHandle prepare_resources = graph->addTask(
        ThreadType::render,
        [this]() { prepareResources(); },
        TaskPriority::normal,
        "PrepareResources"
    );
    graph->addDependency(prepare_resources, update_visibility);
    
    // 任务3: 构建渲染命令（渲染线程，依赖 prepare_resources）
    TaskHandle build_commands = graph->addTask(
        ThreadType::render,
        [this]() { buildRenderCommands(); },
        TaskPriority::normal,
        "BuildRenderCommands"
    );
    graph->addDependency(build_commands, prepare_resources);
    
    // 任务4: 执行渲染（RHI 线程，依赖 build_commands）
    TaskHandle execute_render = graph->addTask(
        ThreadType::rhi,
        [this]() { executeRender(); },
        TaskPriority::high,
        "ExecuteRender"
    );
    graph->addDependency(execute_render, build_commands);
    
    // 执行任务图
    RenderingThread::executeTaskGraphAndWait(graph.get());
}
```

### 注意事项

1. **编译任务图**: 任务图在执行前会自动编译，检查依赖关系
2. **循环依赖**: 如果检测到循环依赖，`compile()` 会返回 `false`
3. **任务图生命周期**: 任务图在执行期间必须保持有效，不要提前销毁
4. **重复使用**: 可以使用 `reset()` 重置任务图并重复使用

## 未来扩展

- 任务组和同步点
- 性能分析和调试工具
- 动态线程池大小调整
- 任务图可视化工具

