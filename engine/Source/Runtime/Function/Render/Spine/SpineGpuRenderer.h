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
//   * 两种实例来源：
//       控制台实例 —— spine.load / spine.add，摆在视口像素空间（自动网格 / spine.place），动画走墙钟；
//       世界实例   —— SpineRenderer 组件（场景里的 GameObject），见下面 RequestAddWorldInstance。
//   * 资产来源：骨架源文件（.skel.bytes / .skel / .json + 同名图集，加载时现烘焙）**或** 已烘焙的
//     `.zspg` 容器（直接读，图集页按页名相对 .zspg 所在目录找）。两者进同一份运行时数据。
// =====================================================================

#include <cstdint>
#include <string>

class RHI;
class RHIRenderPass;
struct RHIViewport;
enum class ViewportType : uint8_t;   // RenderType.h

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
    //   * 不进"选中"，spine.load / spine.clear 不清它们（只清控制台实例）；
    //   * 所属 GameObject 被删（RenderScene::DeleteEntityByGObjectID ⇒ RemoveWorldInstancesOfOwner）时
    //     实例跟着删；组件下一次 tick 发现 id 已不认识就重建（撤销删除时就这样复活）。
    // ⚠️ 第一版没有深度：画在场景之上（与控制台实例同一层），不被网格遮挡。
    // `anim` 为动画名或下标（空 = 第 0 个）。返回 id（≥ 1）。加载失败时 id 仍然"认识"（不会被组件
    // 每帧重建刷日志），原因见 spine.status。
    uint32_t RequestAddWorldInstance(const std::string& path, const std::string& anim, bool loop, uint64_t owner);
    // `local_to_world` = Matrix4x4 的 m_Mat（行主序、列向量约定），已含组件的骨架单位缩放。
    // 返回 false ⇒ 渲染器已不认识这个 id（所属对象被删过），调用方应重建。
    bool RequestSetWorldTransform(uint32_t id, const float local_to_world[16], float dt, bool visible);
    void RequestRemoveWorldInstance(uint32_t id);
    void RemoveWorldInstancesOfOwner(uint64_t owner);
    // 逻辑帧 +1（WorldManager::Tick 开头调）。世界实例只在"上一个或这一个逻辑帧里被推过变换"时才画 ⇒
    // 组件 disabled / GameObject 非激活 / 对象不在关卡里（不 tick）时自动不画；暂停时逻辑帧不前进 ⇒ 定格。
    void AdvanceWorldTick();
    // 定向版播放命令：作用在 `id` 上（不看选中），语义同上面的同名命令
    void RequestSetAnimationOn(uint32_t id, int track, const std::string& anim, bool loop);
    void RequestTimeScaleOn(uint32_t id, float scale);
    void RequestSkinOn(uint32_t id, const std::string& skin, bool setup_slots);

    // 渲染线程：在 `render_pass` 的 `subpass` 里、`viewport` 范围内画全部实例。
    // 调用方负责已 begin 了该 render pass、并处在该 subpass。`viewport_type` 选世界实例用哪台相机。
    void Draw(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass, const RHIViewport& viewport,
              ViewportType viewport_type);

    // 同上，视口按编辑器规则自己挑：Scene 窗口开着（宽高 > 0）就画在 Scene 里，否则画在 Game 视口里。
    // DX12 / Metal 的接入点用它（Vulkan 的 MainCameraPass 自己挑，规则相同）。
    void DrawInEditorViewport(RHI* rhi, RHIRenderPass* render_pass, uint32_t subpass);

    // 诊断：上一帧画了几个绘制项（全部实例之和；`spine.status` 用）。
    uint32_t LastDrawCount() const;
    std::string Status() const;

private:
    SpineGpuRenderer();   // Impl 在构造时就建好：游戏线程与渲染线程都只读这根指针
    struct Impl;
    Impl* m_Impl {nullptr};
    Impl& impl();
};
