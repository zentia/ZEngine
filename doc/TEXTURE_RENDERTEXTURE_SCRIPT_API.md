# `Texture2D` / `RenderTexture` 脚本门面

TS 侧：`tex.GetPixel(3, 5)`、`tex.width`、`rt.Create()`、`rt.isCreated`。
对位 Unity 的 `UnityEngine.Texture2D` / `UnityEngine.RenderTexture`。

代码位置：

| 文件 | 内容 |
| --- | --- |
| `Runtime/Function/Render/Texture/Texture2D.h` | 门面声明（资产类本身） |
| `Runtime/Function/Render/Texture/Texture2D.cpp` | 像素读写 / Resize 实现（新增） |
| `Runtime/Function/Render/Texture/RenderTexture.h/.cpp` | 门面声明 + 生命周期实现 |
| `Runtime/Function/Render/Texture/RenderTextureDesc.h` | 描述结构（补齐默认值） |

---

## 1. `Texture2D`：只覆盖 CPU 侧那一半

导出的：宽高 / 格式号 / mip 数 / `isValid` / `hasUncompressedPixels` /
`GetPixel` / `SetPixel` / `ClearPixels` / `Resize`。

**没有** `Apply()` / `LoadImage()` / `EncodeToPNG()`。

| Unity API | 依赖 | 本引擎现状 |
| --- | --- | --- |
| `Apply()` | GPU 上传 + 重算 mip | Texture2D 刻意**不持有 RHI 状态**（要能在 cooker / headless 里反序列化） |
| `LoadImage()` | png/jpg 解码 | 归 importer |
| `EncodeToPNG()` | png 编码 | 归导出工具 |

在门面上补一个 `Apply()` 空实现，会让脚本以为"改完像素屏幕上就变了" —— 那是假
成功。**缺一个 API 是编译期报错，给个假值是运行期的错行为。**

### 为什么像素访问要求"未压缩 RGBA8"，而且不查格式号

`m_Format` 存的是 `RHIFormat` 的**序号**（Texture2D.h 刻意不依赖 RHI）。门面这边没
法判定"这个序号是不是每像素 4 字节" —— 猜错的后果是静默读错位（BC7 上一个像素
1 字节，按 4 字节读就越界）。

判据因此是**可观测的事实而不是格式号**：

```
m_Pixels.size() >= width * height * 4
```

成立就按 RGBA8 读写；不成立就拒绝（`SetPixel` 返回 `false`，`GetPixel` 前提供
`hasUncompressedPixels` 先查）。cook 出来的资产格式号写错、或者在 importer 之外
手搓的 Texture2D，都不会读坏内存。

### 边界行为

* `GetPixel` 越界 / 不支持 → 透明黑（`r=g=b=a=0`）。**不夹紧** —— 夹紧会让
  `GetPixel(-1, 0)` 悄悄返回边缘像素，把"索引算错了"变成"画面上有个奇怪的边"。
* `SetPixel` 返回 `bool`（是否真的写了）。
* 通道转换与 `ColorUtility.ToHtmlStringRGB` 同一套舍入（`floor(v*255+0.5)`），
  来回转换不漂；并且夹紧到 [0,255]，避免 `Color(1.5)` 被 `uint8_t` 截断成 0。
* `Resize` 重开一张**全 0 的空图**并丢掉全部 mip（不是"保留内容重采样"）。
  上限 16384：更大的贴图在任何现存 GPU 上都创建失败，而失败会发生在上传时。

---

## 2. `RenderTexture`：描述 + 生命周期，没有 GPU 那一半

导出的：`width` / `height` / `antiAliasing` / `depth` / `format` / `useMipMap` /
`autoGenerateMips` / `isCreated` / `Create()` / `Release()` / `DiscardContents()` /
静态 `active`。

⚠️ 本引擎目前**没有** RenderTexture 的 GPU 通路：`RenderCamera::targetTexture` 只
存了个指针，渲染那边还没读它。所以 `Create()` 之后**并没有真的建出一张 GPU 贴图**。

与 `Screen.SetFullScreen`（不导出）的处理不同：描述与生命周期是**上层契约**，"我要
一张 1280x720 带深度的 RT"这个意图得有地方表达。等 GPU 通路接进来时，`Create()` 里
加一句真正的分配即可，脚本侧一个字都不用改。

### `RenderTextureDesc` 的每个字段都必须有默认值

`RenderTexture` 里是 `RenderTextureDesc m_Desc;` 这种默认构造。没有默认值的
`int width;` 在默认初始化下是**不确定值** —— 一台机器上读成 0、另一台读成
0xCCCCCCCC，而"宽是负数"会一路传到 RHI 才炸。这次顺手补齐了。

### 状态之间的自洽

* `useMipMap = false` 会**连带关掉** `autoGenerateMips`（没有 mip 链却自动生 mip
  是自相矛盾的状态）；
* `autoGenerateMips = true` 会**连带打开** `useMipMap`；
* `Release()` 时如果它是活动 RT，活动 RT 自动回落为 `nullptr` —— 留着它，之后的
  渲染会往一张已经没有资源的 RT 上画（Unity 也是这个行为）；
* `Create()` 在尺寸非法时**不置位**，`IsCreated()` 保持 `false`，脚本能看到"这一步
  没成"。

---

## 3. TS 用法

```ts
// 生成一张程序化贴图
const tex = ... as Texture2D;      // 从资产拿到
if (tex.hasUncompressedPixels) {
    tex.Resize(64, 64);
    tex.ClearPixels(Color.white);
    tex.SetPixel(0, 0, Color.red);
}

const rt = ... as RenderTexture;   // 从资产拿到
rt.width = 1280;
rt.height = 720;
rt.depth = 24;
rt.Create();
if (rt.isCreated) {
    RenderTexture.active = rt;
}
```
