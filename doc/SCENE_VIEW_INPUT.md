# SceneView 输入捕获与操作说明

本文档记录编辑器 `Scene` 视图的输入捕获状态、Scene Camera 设置和操作行为，整体参考 Unity SceneView 交互方式。

## 输入捕获状态

`SceneView` 使用类似 Unity `hotControl` 的输入捕获状态，确保一次拖拽过程中只由一个操作消费鼠标输入，并在释放或离开窗口时恢复到空闲状态。

| 状态 | 说明 |
| --- | --- |
| `None` | 未捕获输入 |
| `Orbit` | 旋转 Scene 视角 |
| `Pan` | 平移 Scene 视角 |
| `DragZoom` | 拖拽缩放 Scene 视角 |
| `Gizmo` | 拖拽当前选中对象的 gizmo |
| `Selection` | 点击选择场景对象 |

## Scene 视图鼠标操作

| 操作 | 行为 |
| --- | --- |
| 右键拖拽 | 旋转视角，进入 `Orbit` 捕获状态 |
| 右键按住 + `WASD` | 沿编辑器相机 `forward` / `right` 方向移动镜头 |
| 中键拖拽 | 平移视角，进入 `Pan` 捕获状态 |
| `Alt + 左键拖拽` | 平移视角，进入 `Pan` 捕获状态 |
| `Alt + 右键拖拽` | 拖拽缩放，进入 `DragZoom` 捕获状态 |
| 滚轮 | 沿相机 `forward` 方向 dolly 缩放，不再只修改 FOV |
| 左键点击 | 点击选择场景对象，进入 `Selection` 捕获状态 |
| 左键按住 + `WASD` | 沿编辑器相机 `forward` / `right` 方向移动镜头 |
| 左键拖拽 gizmo | 保留原有物体位移 / 旋转 / 缩放逻辑，进入 `Gizmo` 捕获状态 |

## Scene Camera 设置

Scene 视图工具栏提供 `Scene Camera` 设置入口，参考 Unity 的 `SceneViewCameraWindow`。

| 设置 | 说明 |
| --- | --- |
| `Field of View` | 调整 Scene Camera 的视场角，范围为 `4` 到 `120` 度 |
| `Dynamic Clipping` | 启用后裁剪面由 Scene 视图动态管理，手动 `Near/Far` 输入禁用 |
| `Clipping Planes` | 关闭 `Dynamic Clipping` 后可手动编辑 `Near` / `Far` |
| `Occlusion Culling` | Scene Camera 遮挡剔除开关状态 |
| `Camera Easing` | Scene Camera 移动缓动开关状态 |
| `Camera Acceleration` | Scene Camera 加速开关状态 |
| `Camera Speed` | Scene Camera 键盘移动速度 |
| `Min` / `Max` | `Camera Speed` 可调范围 |

## 捕获释放规则

- 鼠标按下时，如果指针位于 Scene 视图区域内，会根据当前按键组合进入对应捕获状态。
- 捕获期间，后续鼠标移动只驱动当前状态对应的操作。
- 鼠标释放后清理当前捕获状态。
- 鼠标离开窗口时会强制释放捕获，避免拖拽状态卡住。
- 右键拖拽旋转时不会触发右键上下文菜单；只有右键点击且未发生拖拽时才打开菜单。

## 快捷键

快捷键参考 Unity，同时保留原有兼容行为。

| 快捷键 | 行为 |
| --- | --- |
| `W` | 切换位移工具；左键或右键按住时作为 FPS 前进 |
| `E` | 切换旋转工具；右键按住时作为垂直移动 |
| `R` | 切换缩放工具 |
| `T` | 保留为位移工具兼容快捷键 |
| `C` | 保留为缩放工具兼容快捷键 |

## 相机导航细节

- `Orbit`：根据鼠标位移旋转编辑器相机。
- `Pan`：根据鼠标位移沿相机 `right` / `up` 方向移动。
- `DragZoom`：根据拖拽主方向沿相机 `forward` 方向 dolly。
- 滚轮缩放：直接沿相机 `forward` 方向移动相机位置，避免修改 FOV 导致透视畸变。
- 按住 `Shift` 时，平移和缩放速度加快。

