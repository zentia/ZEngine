# 渲染通道简化方案

## 目标
将当前的多子通道（Multi-subpass）渲染通道简化为类似 UE 的独立 Pass 架构，避免子通道仿真带来的复杂性。

## 当前问题
1. DX12 需要仿真 Vulkan 的子通道语义（`loadOp` 仅在首次使用时应用）
2. `m_ActiveSubpassAttachmentsUsed` 等复杂逻辑容易引入 bug
3. 子通道之间的 attachment 清除逻辑难以调试

## 简化方案

### 1. 渲染通道拆分

将当前的单一 RP1 渲染通道（含 3 个子通道）拆分为 3 个独立的渲染通道：

| 原 Subpass | 新 Render Pass | 输入 Attachments | 输出 Attachments | 用途 |
|-------------|----------------|------------------|------------------|------|
| Subpass 0 (G-Buffer) | `GBufferPass` | - | GBufferA, GBufferB, GBufferC, Depth | 写入 G-Buffer |
| Subpass 1 (Deferred + Sky) | `DeferredLightingPass` | GBufferA, GBufferB, GBufferC, Depth | BackupOdd | 延迟光照 + 天空 |
| Subpass 2 (Forward) | `ForwardLightingPass` | BackupOdd, Depth | BackupOdd | 透明物体 |

### 2. 帧缓冲区设计

#### GBufferPass Framebuffer
- **Attachments**: GBufferA (R8G8B8A8_UNORM), GBufferB (R8G8B8A8_UNORM), GBufferC (R8G8B8A8_SRGB), Depth (D32_SFLOAT)
- **Render Pass**: 简单的单子通道渲染通道，清除所有附件
- **Final Layout**: G-Buffer → `SHADER_READ_ONLY_OPTIMAL`, Depth → `DEPTH_STENCIL_READ_ONLY_OPTIMAL`

#### DeferredLightingPass Framebuffer
- **Attachments**: GBufferA, GBufferB, GBufferC, Depth (输入, 作为采样器), BackupOdd (R16G16B16A16_SFLOAT) (输出)
- **Render Pass**: 简单的单子通道渲染通道，清除 BackupOdd
- **Final Layout**: BackupOdd → `SHADER_READ_ONLY_OPTIMAL`

#### ForwardLightingPass Framebuffer
- **Attachments**: BackupOdd (输入和输出), Depth (输入)
- **Render Pass**: 简单的单子通道渲染通道，加载 BackupOdd（不清除）
- **Final Layout**: BackupOdd → `SHADER_READ_ONLY_OPTIMAL`

### 3. 图像布局转换

需要在渲染通道之间插入显式的图像布局转换：

```
GBufferPass (写入 G-Buffer)
  ↓ (布局转换: G-Buffer → SHADER_READ_ONLY_OPTIMAL)
DeferredLightingPass (读取 G-Buffer, 写入 BackupOdd)
  ↓ (布局转换: BackupOdd → SHADER_READ_ONLY_OPTIMAL)
ForwardLightingPass (读取 BackupOdd, 写入 BackupOdd)
```

### 4. 内存屏障

需要在渲染通道之间插入内存屏障，确保数据可见性：

- **GBufferPass → DeferredLightingPass**:
  - `srcStageMask`: `COLOR_ATTACHMENT_OUTPUT_BIT`
  - `dstStageMask`: `FRAGMENT_SHADER_BIT`
  - `srcAccessMask`: `COLOR_ATTACHMENT_WRITE_BIT`
  - `dstAccessMask`: `SHADER_READ_BIT`

- **DeferredLightingPass → ForwardLightingPass**:
  - `srcStageMask`: `COLOR_ATTACHMENT_OUTPUT_BIT`
  - `dstStageMask`: `COLOR_ATTACHMENT_OUTPUT_BIT`
  - `srcAccessMask`: `COLOR_ATTACHMENT_WRITE_BIT`
  - `dstAccessMask`: `COLOR_ATTACHMENT_READ_BIT`

### 5. 代码修改计划

#### 步骤 1: 修改 `MainCameraFramebufferResources`

1. **移除 `SetupRenderPass1()` 中的多子通道逻辑**
   - 创建 3 个独立的渲染通道：`m_GBufferRenderPass`, `m_DeferredLightingRenderPass`, `m_ForwardLightingRenderPass`
   - 每个渲染通道只有一个子通道

2. **创建 3 个独立的帧缓冲区**
   - `m_GBufferFramebuffer`: 包含 GBufferA, GBufferB, GBufferC, Depth
   - `m_DeferredLightingFramebuffer`: 包含 GBufferA, GBufferB, GBufferC, Depth (输入), BackupOdd (输出)
   - `m_ForwardLightingFramebuffer`: 包含 BackupOdd, Depth

3. **修改 `SetupAttachments()`**
   - 确保附件的 `usage` 标志包含 `SAMPLED_BIT`（用于输入附件）

#### 步骤 2: 修改 `MainCameraRp1Pass`

1. **拆分 `DrawRP1()` 为 3 个独立函数**
   - `DrawGBufferPass()`: 绘制 G-Buffer
   - `DrawDeferredLightingPass()`: 绘制延迟光照 + 天空
   - `DrawForwardLightingPass()`: 绘制透明物体

2. **在每个函数之间插入布局转换和内存屏障**
   - 使用 `CmdPipelineBarrier()` 进行布局转换
   - 使用 `CmdPipelineBarrier()` 进行内存屏障

3. **修改管线创建逻辑**
   - 为每个渲染通道创建独立的管线
   - 管线的 `renderPass` 和 `subpass` 参数需要更新

#### 步骤 3: 修改 `DX12RHI.cpp`

1. **简化 `BindSubpassRenderTargets()`**
   - 移除 `m_ActiveSubpassAttachmentsUsed` 逻辑
   - 每个渲染通道独立清除附件

2. **确保 `CmdPipelineBarrier()` 正确实现**
   - 支持图像布局转换
   - 支持内存屏障

### 6. 预期收益

1. **简化代码**: 移除复杂的子通道仿真逻辑
2. **提高可维护性**: 每个渲染通道独立，易于调试
3. **提高兼容性**: 更符合 UE 的架构，易于参考 UE 的实现
4. **提高性能**: 避免子通道仿真带来的额外开销

### 7. 风险评估

1. **工作量较大**: 需要修改多个文件
2. **引入新 bug**: 布局转换和内存屏障容易出错
3. **性能回归**: 需要验证性能是否有所提升

### 8. 后续工作

1. **验证天空盒渲染**: 确保天空盒正确显示
2. **验证延迟光照**: 确保延迟光照正确计算
3. **验证透明物体**: 确保透明物体正确渲染
4. **性能测试**: 对比简化前后的性能

## 实施步骤

1. **创建分支**: 从 `main` 分支创建新分支 `refactor/render-pass-simplification`
2. **实施步骤 1**: 修改 `MainCameraFramebufferResources`
3. **实施步骤 2**: 修改 `MainCameraRp1Pass`
4. **实施步骤 3**: 修改 `DX12RHI.cpp`
5. **测试**: 验证所有功能正常工作
6. **提交 PR**: 提交拉取请求，等待审查

## 时间表

- **第 1 天**: 实施步骤 1
- **第 2-3 天**: 实施步骤 2
- **第 4 天**: 实施步骤 3
- **第 5 天**: 测试和调试
- **第 6 天**: 提交 PR

## 结论

这个简化方案将显著提高代码的可维护性和可读性，同时降低 bug 的风险。虽然工作量较大，但长期来看是值得的。
