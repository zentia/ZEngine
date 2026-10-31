# 对象生命周期脚本 API（Destroy / DestroyImmediate / FindObject(s)OfType）

切片 16。补的是"我不管它在场景哪个角落"这一类入口。

## 缺口

在此之前 `GameObject` 上只有三个生命周期/查找入口：`Instantiate(path)`、
`Find(name)`、`Destroy(obj)`。缺的是：

| Unity | 补之前 |
| --- | --- |
| `Destroy(obj, t)` | ❌ 只能"这一帧末"，没有延迟 |
| `DestroyImmediate(obj)` | ❌ |
| `FindObjectOfType<T>()` | ❌ 只能按**名字**查对象，不能按**组件类型**查 |
| `FindObjectsOfType<T>()` | ❌ |

按类型全场查找是 Unity 里"找主相机 / 找 GameManager"的标准写法，缺了它脚本只能
从场景根开始自己遍历整棵树。

## API

```ts
GameObject.Destroy(obj, delay)      // delay 是**必填**的
GameObject.DestroyImmediate(obj)
GameObject.FindObjectOfType(type)   // Component | null
GameObject.FindObjectsOfType(type)  // Component[]
```

### `Destroy(obj, delay)` 的 delay 为什么是必填

Unity 的签名是 `Destroy(Object obj, float t = 0.0F)`，靠 C# 的默认实参让
`Destroy(obj)` 成立。ZFUNCTION 层没有可选参数（同 PlayerPrefs 的默认值只能是显式
形参），所以脚本要写 `GameObject.Destroy(obj, 0)`。

宁可多写一个 0，也不要一个"看起来只传一个参数也行"的绑定 —— 那会在运行时才炸，
而不是在编译时。

`delay <= 0` 就是原来的语义（这一帧末）。`delay > 0` 是**游戏内**时间：受
`timeScale` 影响，与 `WaitForSeconds` 同一个口径 —— 慢动作里对象该多活一会儿。

### 三个不变式

1. **销毁永远是延迟的**。`DestroyImmediate` 也只是"入队 + 立刻冲刷"，不绕开
   `Level::FlushPendingDestroys`。那里面主表对象（`shared_ptr` erase）和运行时
   对象（`ObjectManager::Destroy`）是两条**不能混**的路径，重复实现一遍迟早漏。
2. **重复请求保留更早的时间点**。`Destroy(obj, 2)` 之后再 `Destroy(obj, 5)`，
   对象在 2 秒时死 —— 覆盖成 5 会让它比第一次请求的时刻活得更久，那不是"再请求
   一次"的意思。
3. **到点 ≠ 已销毁**。倒计时到点只是把对象移进帧末队列，真正的销毁仍然统一在
   `FlushPendingDestroys` 里做。否则在 Tick 中途销毁会让正在遍历对象表的迭代器
   当场失效。

### `DestroyImmediate` 的坑

Unity 把它标成"别在运行时用"是有原因的：它**当场执行**销毁。在一个对象的 Tick /
OnUpdate 里调用它去销毁别的对象，正在遍历对象表的那个迭代器会失效。运行时一律用
`Destroy(obj, 0)`。

### `FindObject(s)OfType` 的顺序

关卡对象表在前、运行时生成的对象（Instantiate 产物）在后，与
`Level::FindGObjectByName` 同一个顺序。两张表都是哈希表，**顺序不保证** —— Unity
的 `FindObjectsOfType` 同样不保证。要稳定顺序自己排。

基类也算命中：`FindObjectsOfType("Component")` 捞到全部组件。判定走
`Type::IsBaseOf`，与 `GameObject.GetComponents` 同一处实现。

## 实现结构：为什么有个 `In` 后缀的核心

活动关卡只能经 `WorldManager::getCurrentActiveLevel()` 拿到，而它**没有 setter** ——
headless 下恒为 `nullptr`。只测 ZFUNCTION 那层壳的话，等于什么也没测：壳在碰到
判空之前就已经在关卡检查处返回了。

所以逻辑都放进吃一张明确 `Level` 的核心，ZFUNCTION 退化成一层壳：

| 壳（脚本入口） | 核心（真实实现） |
| --- | --- |
| `GameObject.Destroy` / `DestroyImmediate` | `GameObject::DestroyIn(level, obj, delay, flush_now)` |
| `GameObject.FindObject(s)OfType` | `GameObject::CollectComponentsOfTypeIn(level, type)` |
| — | `Level::QueueDestroy(id, delay)` / `Level::TickDestroyTimers(dt)` |

这个形状和 CharacterController / MotorComponent 那套一致。

⚠️ `TickDestroyTimers` 是**单独的方法**而不是塞进 `Level::Tick` 内部：`Tick` 在未
加载的关卡上直接返回，而这条计时逻辑要能被单独驱动（测试、暂停后恢复）。它由
`Level::Tick` 在 `FlushPendingDestroys()` 之前调用 —— 少了这一句，延迟销毁永远
不会发生，而且没有报错。
