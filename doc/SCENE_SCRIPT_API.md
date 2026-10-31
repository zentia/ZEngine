# 场景脚本 API（`Scene` / `SceneManager`）

对应 Unity：`UnityEngine.SceneManagement.Scene` / `SceneManager`。

代码：`Source/Runtime/Scripting/ScriptScene/ScriptScene.{h,cpp}`
门禁：`UnityParitySmokeTest` 的 **U 组**（行为）与 **V 组**（接线）。

---

## 1. 为什么现在才有

WorldManager 早就支持打开 / 叠加加载 / 卸载关卡，Level 也早就知道自己的 URL
和对象表 —— 但脚本面一个入口都没有。脚本想"换一关"只能从 C++ 侧调，于是
"按 R 重开一局""过了这关进下一关"这些最常见的玩法代码写不出来。

这是一层纯粹的转发，没有新的引擎子系统。

---

## 2. API

```ts
// 属性（Unity 同拼写）
scene.name       // "Main"      —— URL 的文件名部分，去扩展名
scene.path       // "Assets/Scenes/Main.level"
scene.rootCount  // 根对象数
scene.isLoaded
scene.isValid    // 是否指向一张真实存在的 Level

scene.GetRootGameObjects(): GameObject[]

// 静态类
SceneManager.GetSceneCount(): number
SceneManager.GetSceneAt(index: number): Scene
SceneManager.GetActiveScene(): Scene
SceneManager.GetSceneByName(name: string): Scene
SceneManager.GetSceneByPath(path: string): Scene
SceneManager.IsSceneLoaded(path: string): boolean
SceneManager.LoadScene(path: string): boolean
SceneManager.LoadSceneAdditive(path: string): boolean
SceneManager.UnloadScene(path: string): boolean
SceneManager.ReloadActiveScene(): boolean
```

`GetSceneAt` / `GetRootGameObjects` 的**顺序不确定**（来自 unordered_map 的
遍历序），一次会话内稳定。需要稳定顺序就自己按 `name` 或 `GetID()` 排 ——
Unity 也不保证这两个顺序。

---

## 3. 与 Unity 的两处刻意差异

### 3.1 没有 `buildIndex`

Unity 的 `Scene.buildIndex` 读的是 **Build Settings 里那份场景列表**的下标。
ZEngine 现在没有这样一份列表（ProjectInfo 和 PlayerSettings 里都没有，打包流程
也不读它）。

不能用"已加载列表里的第几个"冒充：Unity 里 buildIndex 是**固定的**、跨会话稳定，
所以脚本可以写 `LoadScene(3)`；而已加载列表的下标随加载顺序变化。给一个含义
不同的数字比不给更糟 —— 缺 API 是编译期报错，假值是运行期的错行为。

要补的时候从"给工程加一份有序的 Build Settings 场景列表"开始，而不是从
`Scene.GetBuildIndex()` 开始。V5 这条断言守的就是"别手滑加上"。

### 3.2 `Scene` 不是值类型

Unity 的 Scene 是 struct（`GetActiveScene()` 返回**快照**）。这里它是 Object
派生类，由 SceneManager **缓存并复用**。

- 为什么必须缓存：`SceneManager.GetActiveScene().name` 写在 Update 里是很常见
  的写法，每帧 new 一个 Object 就是每帧漏一个。
- 为什么按 `Level*` 而不是按 URL 缓存：**Level 会被重载**（同一个 URL 第二次
  加载是另一个 Level 实例）。按 URL 缓存的话，重载后脚本手里的 Scene 还指向
  旧的、已经 Unload 过的那张 —— `isLoaded` 变 false、根对象列表是空的，而脚本
  完全不知道发生了什么。按指针缓存则同一个 Scene 对象自动跟着换指向。

代价：脚本拿到的是"活的"视图而不是快照。与 Unity 不同，但更适合引擎侧。

---

## 4. 语义细节

**`LoadScene` 是同步的**：返回后 `GetActiveScene()` 已经是新场景。Unity 的
`LoadScene` 要等到下一帧才生效（它先卸载再加载，`SceneManager` 在帧末才切换）。
这里没有异步加载，所以也就没有 `LoadSceneAsync` / `AsyncOperation`。

**`UnloadScene` 拒绝卸载活动场景**：那会让引擎进入"没有活动关卡"的状态，而
`Level::Tick` / `GameObject.Find` / `Instantiate` 全都依赖它。Unity 会回退到
一张空场景，这里选择返回 `false` —— 静默地失去活动关卡比一个 false 难查得多。

**`GetSceneAt` 越界返回 nullptr，不夹紧到第 0 张**。夹紧是最阴的一类"帮我改
参数"：脚本里 `for (i = 0; i < n; ++i)` 的 n 写大了，它会让"我以为有 3 张其实
只有 1 张"悄悄变成"又拿到第 0 张"，而循环还正常跑完。

---

## 5. 门禁

**U 组（行为）**

- U1–U10：没有 WorldManager 时的降级（返回空 / false，**不能崩**）
- U12–U17：未绑定的 Scene 全部为空值
- U18–U23：绑定一张真实 Level 后的 name / path / isLoaded
- U24–U28：根对象计算（两张表都看 + 挂父节点后只剩一个根）
- U29–U30：名字解析的边界（无扩展名、反斜杠分隔符）
- U31–U35：装一个真实 WorldManager 后的查询

**V 组（接线）**

- V3–V4：属性形态
- V5–V6：`buildIndex` 确实没发布，且**说明必须有**（不加注释的话下一个人会
  当成遗漏直接补上，然后给个假值）
- V8：缓存按 `Level*`
- V9：根对象枚举两张表都看
- V14–V15：越界不夹紧、活动场景不许卸 —— 这两条 **headless 行为级够不到**
  （没有真实关卡文件就永远加载不出场景，U34/U35 只覆盖了"还没有场景"的早退），
  所以退到源码级，但判得很窄：不是"出现了某个字符串"，而是"禁止的那种写法
  没出现"。

### 夹具的坑

1. **headless 测试里 `Cast<Transform>()` 恒为 nullptr**。Transform 的 Type 是
   `IMPLEMENT_REGISTER_CLASS(Transform)` 注册的，而那行在 ZHeaderTool 生成的
   `Transform.gen.cpp` 里 —— 本 exe 不编 gen.cpp，于是 `GameObject::GetTransform()`
   永远返回 nullptr，**哪怕 Transform 明明挂在对象上**。症状是段错误，不是断言红。
   解法：显式声明 `template<> void RegisterKlass<Transform>();` 后直接调用
   （注册函数本身因为 /WHOLEARCHIVE 在镜像里），再 `InitializeAllTypes()`。
2. **GObjectID 必须在构造时给**（`new GameObject(ObjectIDAllocator::Alloc())`），
   没有事后 setter —— 运行时表按 id 建索引，两个 id=0 的对象会互相覆盖。
