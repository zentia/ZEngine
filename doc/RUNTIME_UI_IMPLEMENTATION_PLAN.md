# 运行时 UI 系统补齐方案（可直接落地）

> **重要修正**：此前对比分析称"ZEngine 完全没有运行时 UI"是**错误的**。实测存在完整的
> **UE UMG 风格**系统，走 ZSlate retained-widgets 路线（非 Unity uGUI）：
> - `Runtime/UI/`（22 文件）：`UISystem` 驱动 + GPU 批渲染 + 字体图集 + 文本生成
> - `Runtime/UMG/`（17 文件）：`UUserWidget`/`UWidget`/`UPanelWidget` + 12 种具体 widget
>   + 资产序列化（`UMGAssetIO`/`UMGWidgetSerializer`）+ 编辑器设计器（`ZSlateUMGDesignerWindow`）
> - `Libraries/ZSlate/`（submodule）：完整 Slate widget 库（Button/CheckBox/ComboBox/Slider/
>   Text/Image/Canvas/Overlay/GridPanel/ScrollBox/ListView/TreeView/Menu/Tooltip/Dialog/Docking…）
>

---

## ✅ P0 落地记录（2026-10-27，数据绑定 + 焦点导航 + Tick 纯逻辑）

P0 的**可本地验证部分**全部落地并验证。真机/渲染绑定的接线（见下方边界）留清单。

**缺口 3（数据绑定/事件序列化）——已落地**：
- **`UMG/Core/UMGDelegates.h`**：`UMGEventRegistry`（Bind/Invoke/Unbind/UnbindEvent/Clear），
  命名事件 id → handler 表。`GetUMGEventRegistry()` 进程级单例（function-local static，无 init-order 陷阱）。
  handler 生命周期：Bind 返回 token，owner 析构时 Unbind 防悬垂；Invoke 前拷贝 handler 列表，
  handler 内 Unbind 自身/同 id 其他 handler 不崩。
- **`UButton`** 加 `OnClickEvent`（序列化进资产）+ 点击时同时触发代码绑定的 `OnClicked` 和注册表事件。
  Designer 资产存事件名，游戏按名 Bind，widget 点击 Invoke——解掉"Designer 绑了运行时丢"。
- **验证**：`UMGDelegatesSmokeTest` 22/22 绿（Bind/Invoke/多监听者/Unbind/UnbindEvent/Clear/未知 id no-op/
  自解绑不崩/重入安全/token 唯一/端到端/全局单例）。

**缺口 1（逐帧 Tick）+ 缺口 4（焦点导航）——纯逻辑已落地**：
- **`UWidget`** 加 `NativeTick(float)` 虚函数 + `IsFocusable/SetFocusable/GetTabOrder/SetTabOrder`
  + `GetChildCount/GetChildAt`（虚子节点访问，panel 覆写）。
- **`UMG/Core/UMGTree.h`**：模板遍历算法（`TickWidgetTree` 深度优先 + Collapsed 子树跳过；
  `CollectFocusable` 按树序收集可聚焦；`NextFocusIndex` Tab 循环索引算术）。模板参数化节点类型，
  测试用最小桩 struct（不拖 ZSlate/引擎数学链）headless 验证。
- **`UUserWidget`** 加 `TickTree(dt)` / `CollectFocusableWidgets()` / `AdvanceFocus(index_hint, reverse)`。
  `UPanelWidget` 覆写子节点访问。`UButton` 构造时 `m_Focusable=true`。
- **验证**：`UMGTreeSmokeTest` 全绿（深度优先 tick/Collapsed 整枝跳过/焦点收集树序/TabOrder 稳定排序/
  Tab 前进后退 wrap/空列表/端到端循环）。
- ZRuntime 干净编译（全部 UMG 改动类型检查通过）。

**未落地（真机/渲染绑定，留清单）**：
1. **Tick 驱动点**：`UISystem::PreRender()` 无 delta_time 参数，`IEngineSystem` 也没有 Tick(delta)
   接口。要驱动 `TickTree` 需给 UISystem 取帧 delta（改接口或从别处取）——扩大改动面且无法本地验证，
   故留真机接线。纯逻辑（TickWidgetTree）已就位。
2. **焦点落到 ZSlate**：`AdvanceFocus` 已算出目标 index，但真正 `SetKeyboardFocusWidget`（落到
   `SWidget`）需 UISystem 持有的 `SlateInputRouter`，属渲染/输入绑定。ZSlate 已有完整焦点 API
   （`SupportsKeyboardFocus/OnFocusReceived/OnFocusLost`），接线即可。
3. **手柄导航**：依赖 Input 系统手柄轴，先做键盘 Tab。

---

## ✅ P1 落地记录（2026-10-27，动画纯核心 + 渲染变换字段）

P1 的**可本地验证部分**（动画的纯逻辑驱动 + 作用对象字段）全部落地并验证。把插值出的值真正
画到屏幕（opacity 乘进 tint / transform 变几何）属 ZSlate submodule paint 链改造，留真机清单。

**缺口 2（UMG 动画）——纯逻辑已落地**：
- **`UMG/Animation/UMGAnimation.h`**（header-only，无引擎/ZSlate 依赖）：
  - `anim_core::ApplyEasing(t, Easing)`：进度→缓动曲线（Linear/Quad/Cubic/Sine/Expo），越界夹紧。
  - `anim_core::AnimTrack{StartValue,EndValue,Easing}`：`Evaluate(lerp)` 先缓动再线性插值。
  - `anim_core::RenderTransform`：动画作用对象（UE `RenderOpacity`/`RenderTransform` 对应物），
    六个槽（Opacity/TranslationX/Y/ScaleX/Y/Rotation），默认值中性（opacity=1、scale=1、其余 0）。
    `ApplyToSlot(path,value)` 按属性路径名分派（短名 `render_opacity` 与 UE 全名
    `RenderTransform.Translation.X` 都认），未识别路径返回 false。**只覆盖命中的槽**——一条只动
    opacity 的动画不会把 scale 意外归零。
- **`UMG/Animation/UWidgetAnimation.h`**（header-only）：命名轨道表（`AddTrack/RemoveTrack/Clear`）
  + 自持的归一化进度状态机（`Play/PlayReverse/Pause/Resume/Stop`，支持 loop/reverse/瞬时）。
  `Tick(dt, target)` 每帧把 lerp 喂给每条轨道、按 property_path 写进目标 `RenderTransform`，返回是否仍在播放。
  **不依赖 ZSlate::FCurveSequence**——那颗头成员函数前向声明顺序在 MSVC 单独包含时报 C2027，且本层
  只需"进度随时间线性推进 + 可循环/反向"这一小撮语义，自持更轻、可 headless 测。
- **`UMG/Core/UWidget.h`**：加 `m_RenderTransform`（`GetRenderTransform()`）+ `m_Animations`
  （`AddAnimation()` 返回稳定引用供配置轨道）。`Tick(dt)` 先推进本 widget 的所有动画写进
  `m_RenderTransform`，再调 `NativeTick`（P0 的子类钩子保持不变）。
- **验证**：`UMGAnimationSmokeTest` 43/43 绿（easing 端点+夹紧 / AnimTrack 线性+缓动+反向 /
  ApplyToSlot 路径分派+未识别返回 false+未命中槽保默认+UE 全名 / UWidgetAnimation 单轨道淡入
  逐帧推进+多轨道同播+未绑定槽保默认+loop 回绕+reverse 起点/中点+瞬时一次性写终点）。
- ZRuntime 干净编译（`UWidget.h` 新字段/新方法类型检查通过）。

**未落地（ZSlate paint 链改造，留真机清单）**：
1. **RenderOpacity → paint**：`SWidget` 无 opacity 字段，`ISlateRenderer` draw 原语吃 `UIColor`(RGBA)。
   要把 `m_RenderTransform.Opacity` 乘进 tint 需改 `FPaintContext` 透传 + 每个绘制原语应用。
2. **RenderTransform → geometry**：平移/缩放/旋转要折进 layout/paint/hit-test 全链的几何变换
   （`SWidget` 无 transform 字段），影响面横跨 ZSlate submodule，无法本地验证。
3. **动画驱动点**：`UWidget::Tick` 已就位，但仍需 P0 记录里那条"UISystem 取帧 delta → 逐帧调
   `TickTree`"的真机接线才能真正每帧跑起来。

---

## ✅ P1 落地记录（补，2026-10-27，锚点/自适应布局纯换算）

**缺口 6（锚点/自适应）——纯换算已落地**：
- **`UMG/Layout/UMGAnchor.h`**（header-only，无引擎/ZSlate 依赖）：Unity RectTransform 的最小可用子集。
  - `Clamp01`：anchor/pivot 越界夹紧。
  - `AnchorSpanOffset/AnchorSpanSize`：拉伸锚下锚点跨距的像素偏移与尺寸（Fill 用）。
  - `ResolveTopLeft`：点锚下 widget 左上角绝对像素位置。公式
    `top_left = parent_pos + anchor*parent_size + offset - pivot*size`，统一处理四角/中心锚
    （anchor=pivot=(0,0)→贴父左上；(1,1)→贴父右下；(0.5,0.5)→居中）。
- **`UMG/Core/UWidget.h`**：加 `m_AnchorMin/m_AnchorMax/m_Pivot/m_Offset`（默认 anchor 0,0 /
  pivot 0,0 / offset 0 = "贴左上、无偏移"，即 ZSlate flow layout 现状，**现有 widget 不受影响**）
  + getter/setter（setter 自动 Clamp01）+ `ResolveAbsoluteTopLeft(parent_pos,parent_size,size)`
  （纯转发到 `layout_core::ResolveTopLeft`）。anchor 四字段序列化进 `UMGPropertyBag`
  （`SerializeProperties/DeserializeProperties`），Designer 存的锚点能 round-trip。
- **验证**：`UMGAnchorSmokeTest` 17/17 绿（Clamp01 / 拉伸锚跨距：全屏/顶部条/点锚 /
  点锚四角定位 / 中心居中 / 正偏移 / 负偏移内缩 / 越界 anchor+pivot 夹紧 / 非原点父矩形）。
- ZRuntime 干净编译（`UWidget.h` 新字段/新方法 + 序列化类型检查通过）。

**未落地（渲染绑定，留真机清单）**：
1. **喂给 ZSlate FGeometry**：`ResolveAbsoluteTopLeft` 算出的绝对位置要经 `FGeometry::MakeChild`
   传给 layout/paint——属 ZSlate submodule 渲染绑定，无法本地验证。
2. **拉伸锚的实际 Fill**：`AnchorSpanSize` 给出跨距，但让 widget 真按跨距拉伸需接 ZSlate 的
   `AlignChildInRegion`（Fill 分支）。

---

> 所以本方案**不是从零建 UI**，而是**补齐现有 UMG 相对 Unity uGUI / UE UMG 的缺口**。

---

## 0. 现状盘点（实测）

### 已实现（可用）
| 能力 | 位置 | 状态 |
|---|---|---|
| Widget 树 retained 模式 | `UMG/Core/UWidget.h` `RebuildWidget()` → ZSlate `SWidget` | ✅ |
| 12 种 widget | `UMG/Widgets/`：Button/Text/Image/CheckBox/Slider/EditableText/Border/BoxPanel/Overlay/ScrollBox/SizeBox/Spacer | ✅ |
| 资产序列化 | `UMG/Asset/UMGWidgetSerializer` + `UWidgetAsset` | ✅ |
| 设计器 | `Editor/.../ZSlateUMGDesignerWindow` | ✅ |
| GPU 批渲染 | `UI/Render/BatchedUIRenderer` + `UIPass` | ✅ |
| 字体/文本 | `UI/Core/Font` + `UI/Render/TextGenerator` + `ZFontAtlas`（stb_truetype） | ✅ |
| 输入路由 | `UISystem.cpp:148` `SlateInputRouter`（鼠标/键盘/滚轮 → ZSlate hit-test） | ✅（但见缺口 3） |
| 可见性 | `UWidget::SetVisibility(EVisibility)` | ✅ |
| 按钮回调 | `UButton::OnClicked`（`std::function`，代码绑定） | ✅ |

### 缺口（相对 Unity uGUI / UE UMG）

| # | 缺口 | 证据 | 影响 |
|---|---|---|---|
| **1** | **无逐帧 Tick** | `UWidget.h`/`UUserWidget.h` 无 `Tick`/`NativeTick` | 进度条、血条平滑、计时器 UI 做不了 |
| **2** | **无动画系统（UMG 层）** | UMG 无 animation；ZSlate 有 `Animation/CurveSequence.h` 但 UMG 未接 | 按钮悬停/按下反馈、面板弹出/淡入淡出做不了 |
| **3** | **无数据绑定** | 无 `BindWidget`/property binding；`OnClicked` 只能代码绑 | Designer 里绑的回调运行时丢（`UButton.h:35` 注释明说"P2"） |
| **4** | **无焦点/导航系统** | 无 `OnFocus`/tab 导航/手柄导航 | 手柄/键盘操作菜单做不了 |
| **5** | **widget 种类不全** | 缺 ProgressBar/ComboBox(UMG层)/Tab/ListView(UMG层)/CanvasPanel(锚点) | 复杂 HUD 需自己拼 |
| **6** | **无锚点/自适应布局** | UMG 用 ZSlate 的 flow layout，无 Unity 式 Anchor/Stretch | 多分辨率适配靠手算 |
| **7** | **无 SafeZone/DPI** | 无 | 主机/移动端异形屏适配缺 |

---

## 1. 优先级排序（按"做游戏最先卡住"）

**P0 — 交互闭环**（不做就做不出可操作的菜单）
- 缺口 3：数据绑定 / 事件序列化
- 缺口 4：焦点 + 键盘/手柄导航

**P1 — 动态表现**（不做就做不出"活"的 HUD）
- 缺口 1：逐帧 Tick
- 缺口 2：UMG 动画（接 ZSlate CurveSequence）

**P2 — 内容广度**（不做能凑合，做了体验好）
- 缺口 5：补 ProgressBar / UMG-ComboBox / Tab
- 缺口 6/7：锚点自适应 + SafeZone

---

## 2. P0 落地：交互闭环

### 2.1 缺口 1 — 逐帧 Tick（最简单，先做）

**Step**：`UWidget` 加虚函数，`UUserWidget` 驱动递归。
```cpp
// UWidget.h
virtual void NativeTick(const FGeometry& geo, float delta_time) {}

// UUserWidget.h  —— 每帧从 UISystem 调
void TickTree(float delta_time) {
    if (m_RootWidget) TickRecursive(m_RootWidget, delta_time);
}
```
**驱动点**：`UISystem::PreRender()`（`UISystem.cpp`）在 `RenderSlateRoot()` 之前遍历所有
`AddToViewport` 的 `UUserWidget` 调 `TickTree(dt)`。

> 需要一个 `UUserWidget` 注册表（viewport 栈），`AddToViewport`/`RemoveFromViewport` 维护。
> 现在 `AddToViewport` 已存在（`UUserWidget.h:43`），加一个 `std::vector<UUserWidget*> m_ViewportStack`。

### 2.2 缺口 3 — 数据绑定 / 事件序列化

**现状**：`UButton::OnClicked` 是 `std::function`，代码绑；Designer 里绑的回调序列化不了
（`UButton.h:35` 注释：`OnClicked is code-bound, not serialised (P2)`）。

**方案**：引入**命名事件 + 运行时绑定表**（UE 的 `DynamicDelegate` 简化版）：
```cpp
// UMG/Core/UMGDelegates.h
using UMGEventId = std::string;   // "OnStartClicked"

class UMGEventRegistry {
public:
    using Handler = std::function<void(UUserWidget*)>;
    void Bind(const UMGEventId& id, Handler h);
    void Invoke(const UMGEventId& id, UUserWidget* ctx);
private:
    std::unordered_map<UMGEventId, std::vector<Handler>> m_Handlers;
};
```
- 序列化侧：`UMGWidgetSerializer` 把 widget 的事件名（如 button 的 `OnClickEvent="OnStartClicked"`）
  存进资产（现在只存属性，加一个 `events` 段）。
- 运行时侧：游戏代码 `Registry.Bind("OnStartClicked", [](UUserWidget* w){ ... })`；
  widget 触发时 `Registry.Invoke(event_id, this)`。

> 这解掉"Designer 绑了但运行时丢"的问题，且不引入反射式 property binding 的复杂度。
> Property binding（文本框显示某属性值）作为 P1 增量，用同样的 id → getter 表实现。

### 2.3 缺口 4 — 焦点 + 导航

**现状**：`UISystem` 的 `SlateInputRouter` 已有 `HasKeyboardFocus()`（`UISystem.cpp:150`），
ZSlate 层有焦点概念，但 UMG 层没有暴露"哪个 widget 有焦点 / tab 顺序"。

**方案**：
- `UWidget` 加 `SetFocus()`/`bIsFocusable` + `NavigationRule`（上下左右 → 目标 widget 名）。
- `UUserWidget` 加 `SetKeyboardFocus(UWidget*)`，把 ZSlate 的焦点落到对应 `SWidget`。
- 手柄：`UISystem` 的输入路由已收键盘，手柄键（需 Input 系统映射）转成"焦点移动"事件。

> 这块依赖 Input 系统的手柄支持。先做键盘 tab 导航（纯 UI 内），手柄导航等 Input 系统接手柄轴。

---

## 3. P1 落地：动态表现

### 3.1 缺口 2 — UMG 动画（✅ 纯逻辑已落地，见顶部 P1 记录）

**现状**：已落地为 `UMG/Animation/UMGAnimation.h`（纯核心：easing + AnimTrack + RenderTransform
属性路径分派）+ `UMG/Animation/UWidgetAnimation.h`（命名轨道 + 自持归一化进度状态机）。
`UWidget` 持有 `m_RenderTransform` + `m_Animations`，`Tick(dt)` 逐帧推进。

**与原方案的偏差**：原方案想复用 ZSlate `FCurveSequence`，但那颗头成员函数前向声明顺序在 MSVC
单独包含时报 C2027，且本层只需"进度随时间线性推进 + 可循环/反向"这一小撮语义——改为自持一个
极小的归一化进度状态机，反而更轻、可 headless 测（只链 include root，不拉 ZSlate）。

**仍待真机**：把 `m_RenderTransform` 应用到 ZSlate `SWidget` 的 paint（opacity 乘 tint /
transform 折进几何），见顶部 P1 记录的"未落地"清单。

### 3.2 缺口 6 — 锚点/自适应（✅ 纯换算已落地，见顶部 P1 记录）

UMG 用 ZSlate flow layout，无 Unity Anchor。已落地为 `UMG/Layout/UMGAnchor.h`（纯换算核心）+
`UWidget` 的 `anchor_min/anchor_max/pivot/offset` 字段与 `ResolveAbsoluteTopLeft`。

**仍待真机**：把算出的绝对位置喂给 ZSlate 的 `FGeometry`（`MakeChild`）——属渲染绑定，留清单。
> 这是"够用"方案（点锚四角/中心 + 拉伸锚跨距），不是 Unity 的完整 anchor presets。

---

## 4. P2 落地：内容广度

- **缺口 5**：补 `UProgressBar`（复用 ZSlate `SProgressBar`）、`UComboBox`（复用 `SComboBox`）、
  `UTabControl`（复用 Docking tab）。每个 widget = 一个 `UWidget` 子类 + `RebuildWidget` + 序列化，
  照 `UButton.h` 模板抄，单个约 80-120 行。
- **缺口 7**：`UUserWidget` 加 `SafeZone` 包装（读 viewport 尺寸 - 系统安全区），主机/移动端用。

---

## 5. 改动清单（P0）

| 文件 | 改动 |
|---|---|
| `UMG/Core/UWidget.h` | +`NativeTick`、+`bIsFocusable/SetFocus`、+`RenderOpacity/Translation/Scale` |
| `UMG/Core/UUserWidget.{h,cpp}` | +`TickTree`、+viewport 栈、+`SetKeyboardFocus`、+动画容器 |
| `UMG/Core/UMGDelegates.h`（新） | 事件注册表 |
| `UMG/Asset/UMGWidgetSerializer.{h,cpp}` | 序列化 `events` 段 |
| `UI/UISystem.{h,cpp}` | `PreRender` 驱动 `TickTree`；事件表全局实例 |
| `UMG/Widgets/UButton.h` | `OnClicked` 走事件表（保留 `std::function` 直连作为快捷方式） |

**风险点**：
1. `NativeTick` 递归遍历的性能——widget 多时每帧全树遍历。缓解：只 tick `AddToViewport` 的根，
   且可见性为 `Collapsed` 的子树跳过。
2. 事件表的生命周期——`UUserWidget` 销毁时其注册的 handler 要能解绑，避免悬垂。用
   `UUserWidget` 析构时按 id 反注册。
3. ZSlate 是 submodule，改 ZSlate 层（如焦点 API）要考虑 submodule 更新流程。

---

## 6. 验证与测试

- **冒烟测试**（纯 CPU）：事件注册表 Bind/Invoke/Unbind 生命周期（含销毁后不悬垂）；
  `TickTree` 递归覆盖 + Collapsed 跳过；动画 `Evaluate` 曲线采样。落
  `Runtime/UMG/Test/UMGCoreSmokeTest.cpp`（仿 `GasCooldownSmokeTest`，只链 include root）。
- **真机验证**：Designer 拖一个 Button + TextBlock，存资产，运行时 `AddToViewport`，
  键盘 tab 聚焦 + Enter 触发 `OnClicked`（走事件表），TextBlock 靠 Tick + 动画淡入。
- **回归**：现有编辑器 UI（ZSlate 直用）不受影响——UMG 是其上的薄层。
