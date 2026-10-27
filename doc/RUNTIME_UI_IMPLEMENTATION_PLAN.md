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

### 3.1 缺口 2 — UMG 动画（接 ZSlate CurveSequence）

**现状**：ZSlate 有 `Animation/CurveSequence.h`（编辑器已用于面板动画），UMG 层未接。

**方案**：
```cpp
// UMG/Animation/UWidgetAnimation.h
class UWidgetAnimation {
public:
    void AddFloatTrack(const std::string& property_path, CurveSequence curve);
    void Play(float play_rate = 1.0f);
    void Stop();
    bool IsPlaying() const;
    // 每帧由 UUserWidget::TickTree 驱动
    void Evaluate(float delta_time, UWidget* target);
private:
    float m_Time {0.0f};
    std::vector<std::pair<std::string, CurveSequence>> m_Tracks;
};
```
- `UUserWidget` 持有 `std::vector<UWidgetAnimation>`，`TickTree` 里 `Evaluate`。
- property_path 用现成的 `UMGPropertyBag` 键名（`UWidget::SerializeProperties` 已用 bag）。
- 典型动画：`RenderOpacity`(0→1)、`RenderTranslation`、`RenderScale`——需在 `UWidget` 加
  这三个运行时变换字段，`RebuildWidget` 时应用到 ZSlate `SWidget` 的 transform。

**先决**：`UWidget` 加 `m_RenderOpacity/m_RenderTranslation/m_RenderScale`（Unity 的 CanvasGroup
alpha + RectTransform 位置的最小集）。

### 3.2 缺口 6 — 锚点/自适应（P1 末）

UMG 用 ZSlate flow layout，无 Unity Anchor。最小方案：给 `UWidget` 加
`Anchor`(0-1 归一化) + `Offset`，`UUserWidget` 布局时按 viewport 尺寸换算成绝对位置。
> 这是"够用"方案，不是 Unity 的完整 anchor presets。真要全功能可后置。

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
