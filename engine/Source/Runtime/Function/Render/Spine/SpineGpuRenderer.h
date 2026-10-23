#pragma once

// =====================================================================
// GPU-Spine 渲染侧：容器 + 解算器 → 屏幕
// 规格：doc/GPU_SPINE_INTERCHANGE_FORMAT.md §3 / §5 / §6 / §8
// ---------------------------------------------------------------------
// 每帧 CPU 只做两件事：解算（S5 起是 `SpineGpuAnimationState::Update/Apply` + `SpineGpuUpdateWorld`，
// 骨骼世界变换 + 槽位状态）与"哪些绘制项可见"；
// **顶点一个都不在 CPU 上算** —— 蒙皮在顶点着色器里做（spine_gpu.vert），静态顶点 /
// 影响流 / UV / 索引每个资产只上传一次、所有实例共用。
//
// 第一版的边界（明确不做，不是忘了）：
//   * 三个桌面后端都画（WebGL2 不画）：
//       Vulkan —— MainCameraPass 的 RP2 UI subpass（画在 UI 之前，与 UI 层同一合成）；
//       DX12   —— MainCameraRp2Pass 的 Combine UI pass 里、合成之后直接画进 swapchain（DX12 编辑器不用
//                 UI 层，ZSlate / ImGui 在 RP2 之后画 ⇒ 面板照样盖在上面）；HLSL 版着色器运行时编；
//       Metal  —— RenderSystem 的 swapchain pass 里、scene blit 之后 post-UI 之前；SPIR-V 经 SPIRV-Cross。
//   * 多实例：每个实例有自己的骨架状态 / AnimationState / 摆放；同一路径的实例**共用**一份资产
//     （静态缓冲 + 图集页），每帧按资产把各实例拼成一包（SpineGpuBatch：bone_base / inst_index /
//     deform、裁剪顶点的偏移）、按实例顺序画（先加的在下面）。自动摆放 = 视口网格里按顺序占一格、
//     setup 包围盒居中占 80%；`spine.place` 改成手动。`spine.load` = 清空后加一个（老语义）；
//   * 多轨混合（S5）：加载时轨道 0 循环播请求的动画，之后用 spine.play / spine.queue /
//     spine.empty / spine.mix / spine.speed / spine.alpha 改（语义即 spine::AnimationState）；
//   * mesh deform（S6）在 GPU 上做：解算器给每帧生效的 deform 数组，上传到 b7，shader 里
//     rigid 整体替换 bindPos、weighted 逐影响加偏移，共享 VB 不动；
//   * 裁剪（v15）：每帧按当前绘制顺序跑参考实现的作用域状态机（SpineGpuClipper），作用域内的项
//     在 CPU 上蒙皮 + `clipTriangles`，结果上传到 b8 + 每帧动态索引缓冲；作用域外照旧 GPU 蒙皮；
//   * 换肤：`spine.skin <名字|下标|none> [setup]` = Skeleton::setSkin（+ setSlotsToSetupPose），每个实例各自的
//     当前皮肤；附件按"先当前皮肤、再 default"解析（SpineGpuSkeletonState：SpineGpuResolveAttachment / SetSkin）；
//   * tint black（双色）：`spine.tintblack 1` 打开（= SpineSettings::tintBlack，**默认关**，与 OSGame 线上一致）；
//     片元按 spine-unity TintBlack shader 加 `(1 - tex.rgb) · dark · tex.a · vc.a`，暗色 float 不截断
//     （SpineGpuTintBlack.h）。
//   * 两种实例来源、两层：
//       控制台实例 —— spine.load / spine.add，摆在视口像素空间（自动网格 / spine.place），动画走墙钟；
//                     画在 **UI 层**（上面三个接入点，Draw / DrawInEditorViewport），无深度、不经 tonemap；
//       世界实例   —— SpineRenderer 组件（场景里的 GameObject），见下面 RequestAddWorldInstance；
//                     画在 **RP1 的 forward / 透明 subpass**（DrawWorld），测场景深度、不写深度、远→近排序，
//                     输出转线性后与透明网格 / 粒子一起经 tonemap。
//     两层每帧缓冲 / 合包 / 管线各一份（同一帧两个时刻录制，共用会互相覆盖）。
//   * 资产来源：骨架源文件（.skel.bytes / .skel / .json + 同名图集，加载时现烘焙）**或** 已烘焙的
//     `.zspg` 容器（直接读，图集页按页名相对 .zspg 所在目录找）。两者进同一份运行时数据。
// =====================================================================

#include <cstdint>
#include <string>
#include <vector>

class RHI;
class RHIRenderPass;
struct RHIViewport;
enum class ViewportType : uint8_t;   // RenderType.h

// 世界实例的公告板（"面向相机"）。渲染线程按**当帧画它的那台相机**算旋转（编辑器 Scene 相机 / PIE 游戏相机
// 各自对），所以组件只给枢轴点和缩放、不给旋转。
enum class SpineGpuBillboardMode : uint8_t
{
    kNone,       // 用 GameObject 的旋转（骨架立在局部 XZ 平面）
    kFull,       // 骨架平面 = 相机平面（屏幕对齐，俯视时人物跟着仰）
    kVertical,   // 只绕世界 Z 转：人物始终直立、正对相机水平方向（2.5D 角色常用）
};
struct SpineGpuAnimationEvent;   // SpineGpuAnimationState.h

// 世界实例某条轨道在上一次解算后的状态（游戏线程查"现在在播什么"用；有一帧以上的延迟）
struct SpineGpuTrackSnapshot
{
    uint32_t animation = 0xFFFFFFFFu;   // kSpineGpuEmptyAnimation = 空动画
    float track_time = 0.0f;
    bool loop = false;
    bool valid = false;                 // false = 这条轨道上没有 TrackEntry
};

struct SpineGpuBillboard
{
    SpineGpuBillboardMode mode {SpineGpuBillboardMode::kNone};
    float position[3] {0.0f, 0.0f, 0.0f};   // 枢轴点（骨架原点）的世界坐标
    float scale_x {1.0f};                    // 每骨架单位多少世界单位，沿相机右；带符号（负 = 水平镜像）
    float scale_y {1.0f};                    // 同上，沿相机上 / 世界 Z
};

class SpineGpuRenderer
{
public:
    static SpineGpuRenderer& Get();

    // 游戏线程：请求加载（实际加载在渲染线程下一次 Draw 时做）。
    // `skeleton_path` 是 .skel.bytes / .skel / .json；图集取同名 .atlas.txt / .atlas，
    // 贴图取图集里写的页名（相对图集所在目录）。
    // RequestLoad = RequestClear + RequestAddInstance（多实例之前的语义：替换掉全部实例）。
    void RequestLoad(const std::string& skeleton_path, int anim_index);
    // 加一个实例（排在绘制顺序最后 = 画在最上面）并选中它；返回实例 id（≥ 1）。同一路径的资产只加载一次。
    // 加载失败时渲染线程会把这个 id 摘掉（日志里有原因），之后发给它的命令丢弃。
    uint32_t RequestAddInstance(const std::string& skeleton_path, int anim_index);
    // 删实例；id 0 = 当前选中。id 不在（游戏线程视角）⇒ false。
    bool RequestRemoveInstance(uint32_t id);
    void RequestClear();   // 删全部实例
    // 选中：之后的播放 / 摆放命令作用在它上面（入队时解析）。id 不在 ⇒ false。
    bool RequestSelect(uint32_t id);
    uint32_t Selected() const;
    // 选中实例手动摆放：骨架原点在视口中心偏 (x, y) 像素（y 向上），1 骨架单位 = scale 像素；
    // RequestPlaceAuto 回到自动网格。
    void RequestPlace(float x, float y, float scale);
    void RequestPlaceAuto();

    // 游戏线程：多轨播放命令（S5），作用在**当前选中**的实例上（defaultMix 除外：全局）。语义与
    // spine::AnimationState 同名方法一致；在渲染线程下一次 Draw 的 Update 之前按提交顺序执行。
    // `anim` 是动画下标（纯数字）或动画名；找不到 ⇒ 命令丢弃并记一行日志。
    void RequestSetAnimation(int track, const std::string& anim, bool loop);
    void RequestAddAnimation(int track, const std::string& anim, bool loop, float delay);
    void RequestSetEmptyAnimation(int track, float mix_duration);
    void RequestDefaultMix(float seconds);   // AnimationStateData::defaultMix（跨资产保留，初值 0.2）
    void RequestTimeScale(float scale);      // 选中实例的 AnimationState::timeScale
    void RequestTrackAlpha(int track, float alpha);
    // 选中实例换肤（= Skeleton::setSkin）：皮肤名 / 下标 / "none"（= setSkin(NULL)）。`setup_slots` 为真时
    // 之后再 setSlotsToSetupPose（spine-unity 的惯用法：让 setup 附件按新皮肤重挂）。找不到 ⇒ 丢弃并记日志。
    void RequestSkin(const std::string& skin, bool setup_slots);
    // tint black（双色）开关 = SpineSettings::tintBlack。默认关（OSGame 线上全关，见 SpineGpuTintBlack.h）。
    // 立即生效（下一次 Draw），不走动画命令队列；跨资产保留。
    void RequestTintBlack(bool enabled);

    // ---- 世界实例（SpineRenderer 组件用；游戏线程）----
    // 与控制台实例的区别：
    //   * 摆放 = 所属 GameObject 的 local-to-world × 相机 proj·view（`viewport_type` 那台相机），
    //     骨架平面是模型空间的 z = 0；不进自动网格、不吃 spine.place；
    //   * 动画时间由组件喂（RequestSetWorldTransform 的 dt，累加到下一次 Draw）—— 暂停时组件不 tick ⇒
    //     动画跟着停，不走墙钟；
    //   * 画在 RP1 透明 subpass（DrawWorld）：测场景深度、不写深度、实例间按观察空间深度远→近画；
    //   * 不进"选中"，spine.load / spine.clear 不清它们（只清控制台实例）；
    //   * 所属 GameObject 被删（RenderScene::DeleteEntityByGObjectID ⇒ RemoveWorldInstancesOfOwner）时
    //     实例跟着删；组件下一次 tick 发现 id 已不认识就重建（撤销删除时就这样复活）。
    // `anim` 为动画名或下标（空 = 第 0 个）。返回 id（≥ 1）。加载失败时 id 仍然"认识"（不会被组件
    // 每帧重建刷日志），原因见 spine.status。
    uint32_t RequestAddWorldInstance(const std::string& path, const std::string& anim, bool loop, uint64_t owner);
    // `local_to_world` = Matrix4x4 的 m_Mat（行主序、列向量约定），已含组件的骨架单位缩放与朝向；
    // `billboard.mode != kNone` 时渲染线程改用 billboard 的枢轴 / 缩放 + 相机朝向，忽略它的旋转。
    // 返回 false ⇒ 渲染器已不认识这个 id（所属对象被删过），调用方应重建。
    bool RequestSetWorldTransform(uint32_t id, const float local_to_world[16], float dt, bool visible,
                                  const SpineGpuBillboard& billboard);
    void RequestRemoveWorldInstance(uint32_t id);
    void RemoveWorldInstancesOfOwner(uint64_t owner);
    // 逻辑帧 +1（WorldManager::Tick 开头调）。世界实例只在"上一个或这一个逻辑帧里被推过变换"时才画 ⇒
    // 组件 disabled / GameObject 非激活 / 对象不在关卡里（不 tick）时自动不画；暂停时逻辑帧不前进 ⇒ 定格。
    void AdvanceWorldTick();
    // 定向版播放命令：作用在 `id` 上（不看选中），语义同上面的同名命令
    void RequestSetAnimationOn(uint32_t id, int track, const std::string& anim, bool loop);
    void RequestTimeScaleOn(uint32_t id, float scale);
    void RequestSkinOn(uint32_t id, const std::string& skin, bool setup_slots);
    void RequestAddAnimationOn(uint32_t id, int track, const std::string& anim, bool loop, float delay);
    void RequestSetEmptyAnimationOn(uint32_t id, int track, float mix_duration);
    void RequestAddEmptyAnimationOn(uint32_t id, int track, float mix_duration, float delay);
    void RequestClearTrackOn(uint32_t id, int track);   // track < 0 ⇒ clearTracks()
    // AnimationStateData::setMix(from, to, duration)，只作用在这个实例上（动画名或下标）
    void RequestMixOn(uint32_t id, const std::string& from, const std::string& to, float duration);
    // 游戏线程取走该实例自上次以来派发的事件（start / interrupt / end / dispose / complete，顺序同
    // spine::AnimationState 的 listener），并拿到上一次解算后的轨道快照。事件在渲染线程的解算里产生 ⇒
    // 相对命令有 1~N 帧延迟（N = 渲染领先逻辑的帧数）。id 不认识 ⇒ false。`dropped` = 累计因积压丢掉的条数。
    bool DrainWorldEvents(uint32_t id, std::vector<SpineGpuAnimationEvent>& out,
                          std::vector<SpineGpuTrackSnapshot>* tracks, uint64_t* dropped);

    // 渲染线程：在 `render_pass` 的 `subpass` 里、`viewport` 范围内画**控制台实例**（UI 层）。
    // 调用方负责已 begin 了该 render pass、并处在该 subpass。每帧必须调一次（延迟销毁的倒计时挂在这里）。
    void Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
              ViewportType viewport_type);
    // 渲染线程：画**世界实例**。`render_pass` / `subpass` 必须是带场景深度附件的透明 / forward subpass，
    // `viewport` 必须与网格用的同一个（深度才对得上），`viewport_type` 选相机。每帧至多调一次（同一层的
    // 每帧缓冲只有一份；编辑器只画一个视口：Scene 开着画 Scene，否则 Game）。在 UI 层的 Draw 之前调。
    void DrawWorld(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
                   ViewportType viewport_type);

    // 同上，视口按编辑器规则自己挑：Scene 窗口开着（宽高 > 0）就画在 Scene 里，否则画在 Game 视口里。
    // DX12 / Metal 的接入点用它（Vulkan 的 MainCameraPass 自己挑，规则相同）。
    void DrawInEditorViewport(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass);

    // 诊断：上一帧画了几个绘制项（全部实例之和；`spine.status` 用）。
    uint32_t LastDrawCount() const;
    std::string Status() const;

private:
    void DrawLayer(int layer, RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
                   ViewportType viewport_type);
    SpineGpuRenderer();   // Impl 在构造时就建好：游戏线程与渲染线程都只读这根指针
    struct Impl;
    Impl* m_Impl {nullptr};
    Impl& impl();
};
