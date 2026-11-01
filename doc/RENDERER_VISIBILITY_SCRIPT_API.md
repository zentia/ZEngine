# 渲染器可见性 —— `OnBecameVisible` / `OnBecameInvisible` / `bounds` / `localBounds` / `isVisible`

```ts
class LodSwitch extends Behaviour {
    private renderer: BaseRenderer | null = null;

    OnStart(): void {
        this.renderer = this.GetComponent(BaseRenderer);
    }

    OnBecameVisible(): void { Debug.Log("进入画面：" + this.renderer!.isVisible); }
    OnBecameInvisible(): void { Debug.Log("离开画面"); }

    OnUpdate(): void {
        // 世界空间 AABB：八个角都过了 LocalToWorldMatrix 之后重新求的。
        const b: Bounds = this.renderer!.bounds;
        Debug.Log(b.center + " / " + b.extents);
    }
}
```

两个都是 `Behaviour` 上的**空默认实现**，脚本里定义了才会被调用（与鼠标、
gizmo 那几组一样：不需要 `override`，也没有"必须实现"的接口）。

| TypeScript | C++ 钩子 | 何时来 |
|---|---|---|
| `OnBecameVisible()` | `Component::OnBecameVisible` | 从"没有任何相机看得见"变成"至少一台看得见"的**那一帧**，一次 |
| `OnBecameInvisible()` | `Component::OnBecameInvisible` | 反向的那一帧，一次 |

`OnBecameVisible` / `OnBecameInvisible` 是**变化事件**，不是每帧事件。

## 派发链

```
Application::LogicalTick
  └─ BaseRenderer::UpdateVisibilityForAll()      ← 每帧一次，在 Update 之前
        └─ 每个 BaseRenderer::UpdateVisibilityAndNotify()
              ├─ isEnabled() && 宿主 IsActiveInHierarchy()
              ├─ GetBounds()  （对象空间盒 → 八个角 → 世界空间 AABB）
              ├─ 每台 CanSeeAnything() 的相机：Frustum::FromViewProjection → Intersects
              └─ 只在 visible 变化那一帧 → GameObject::BroadcastVisibilityChange(visible)
                    └─ 每个组件 OnBecameVisible / OnBecameInvisible
                          └─ TypeScriptComponent → JS
```

另外两次"立刻刷新"，不走主循环：

```
renderer.enabled = false     → BaseRenderer::SetEnabled         → UpdateVisibilityAndNotify()
gameObject.SetActive(false)  → BaseRenderer::OnOwnerActiveChanged → UpdateVisibilityAndNotify()
```

## 为什么可见性挂在 `UpdateVisibilityForAll()` 上，而不挂在 `BaseRenderer::Tick` 上

`BaseRenderer::Tick` 在 transform 不脏时**直接 return**（那是渲染状态提交的
优化，不能为了可见性去掉）。于是"相机转开了、物体没动"这种情况永远不会被
发现 —— 可见性会一直停在旧值上。同理，对象未激活时 `GameObject::Tick` 直接
返回，所以 `OnOwnerActiveChanged` 里也必须刷一次。

`isVisible` 是**上一帧扫描的结果**：放在 Update 之前扫，脚本在同一帧的
`OnUpdate` 里就能读到新值，而不是晚一帧。

## `enabled` 挡不住可见性消息

Unity 明文：`enabled` *"will only prevent Start(), Awake(), Update(),
FixedUpdate(), and OnGUI() from executing."* —— 所以**停用的脚本照样收到**
`OnBecameVisible` / `OnBecameInvisible`，与鼠标、gizmo 那两组一致
（`BroadcastVisibilityChange` 里只判 `IsActiveInHierarchy`，不判 `isEnabled`）。

但**停用的渲染器不可见**（`UpdateVisibilityAndNotify` 判 `isEnabled()`）：它
不渲染，于是"有没有相机看得见它"这件事没有意义。停用的相机同理
（`CameraComponent::CanSeeAnything()` 判 `isEnabled()`）。

四条判据汇总：

| 派发 | activeInHierarchy | enabled | 编辑器白名单 |
|---|---|---|---|
| Tick / FixedTick / LateTick | ✅ | ✅ | ✅ |
| `BroadcastAppEvent` | ❌ | ❌ | ❌ |
| `BroadcastDrawGizmos` | ✅ | ❌ | ❌ |
| `BroadcastMouseEvent` | ✅ | ❌ | ❌ |
| `BroadcastVisibilityChange` | ✅ | ❌ | ❌ |

## `localBounds` / `bounds`

```ts
renderer.localBounds = Bounds.FromCenterSize(Vector3.ZERO, new Vector3(2, 2, 2));
const world: Bounds = renderer.bounds;      // 只读
const vis: boolean = renderer.isVisible;    // 只读
```

* `localBounds` **可写**（Unity 同）。默认从 sub-mesh 的网格资产算；没有网格
  资产（headless / 工具 / 专用服务器）时默认回退成**零盒**，不是单位立方体。
  脚本和导入器随时可以接管（粒子、`SkinnedMeshRenderer` 就是这么用的）。
* `bounds` 是**世界空间** AABB：把对象空间盒的**八个角**逐个过
  `LocalToWorldMatrix` 之后再求 min/max。只变换 center、按缩放拉伸 extents 是
  错的 —— 非均匀缩放 + 旋转下那样算出来的盒比真的小，斜着放的长条会露一截。
* 包围盒结果带缓存，`SetSubMeshes` / `PostLoadResource` /
  `OnSerializedFieldsUpdated` 会让它失效。

## 视锥判据是"保守"的

`Frustum::Intersects(Bounds)` 用的是 AABB 对平面求"最正面点"的标准写法：只
测沿法线方向最远的那个角。它在背面 ⇒ 全在背面（可以判"不相交"）；它在正面
**不代表**盒子真的相交。所以半边长 1 的盒子中心离眼 0.05（near = 0.1）时算
**可见** —— 盒子不是点，这是这个精度的固有行为，不是 bug（见
`UnityParitySmokeTest` 的 RV12 / RV12b 那对互补断言）。

## 相机

```ts
const cam: CameraComponent = this.GetComponent(CameraComponent);
cam.nearClipPlane = 0.1;
cam.farClipPlane = 1000;
```

`CameraComponent` 现在带一张**场景内所有相机**的注册表
（`CameraComponent::GetAllCameraComponents()`）：可见性得遍历所有相机，而
headless 下没有活动关卡（`FindObjectsOfType` 走不到），只能靠这张表。默认
`nearClipPlane = 0.3`、`farClipPlane = 1000`，与 Unity 的默认值一致。
深度范围用 `Frustum::DepthRange::kZeroToOne`（本引擎是 [0,1]，不是 OpenGL 的
[-1,1]）。

## 测试与变异

* `UnityParitySmokeTest`：`TestRendererVisibility()`（RV0–RV20，含 RV12b /
  RV17b / RV18b 三条互补断言）+ `TestRendererVisibilityWiring()`（RW0–RW12）。
* `ScriptComponentSmokeTest`：V0–V3，端到端走 TS（`visibilityLog` 的**字母顺序**
  判方向 —— 合成一个计数器的话"进出转发到同一个 JS 名字"是绿的）。
* `.mutate_rv.py`：19 个变异体，覆盖"顺手把 enabled 也判上"、三条判据各删一条、
  只变换两个角、零盒变单位立方体、两张注册表不注册、主循环没接、转发名字串了、
  发布面缺失、以及门本身的门（CRLF 归一化）。

### 已知覆盖缺口

* `UpdateVisibilityForAll()` 先拷一份注册表再遍历（脚本可以在
  `OnBecameVisible` 里销毁对象）。没有夹具在回调里改注册表，所以"直接遍历活的
  vector"这个变异体必然存活 —— 按惯例不写必然存活的变异体，缺口记在这里。
* `~BaseRenderer` 从注册表里摘掉自己同理没有行为级门。
