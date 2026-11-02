#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <EASTL/string.h>

#include <vector>

#include "ScriptQuality.generated.h"

// 画质档位与画质开关（Unity 的 `UnityEngine.QualitySettings` 静态类）。
//
// 这一档与 `SystemInfo` 相反：**它自己是唯一的数据源**。
// ---------------------------------------------------------------
// `SystemInfo` 是"问机器"，拿不到就只能给哨兵值。`QualitySettings` 是"用户设
// 定"，没有上游可问 —— 渲染器目前不读这些值（见下面"接线状态"），所以这里
// 存的就是权威值。脚本写了什么，之后接线的渲染代码就读到什么。
//
// 接线状态（读这段再决定要不要用）
// --------------------------------
// ⚠️ 目前**没有**渲染代码消费这些开关：`vsyncCount` 不会真的去改交换链，
// `shadowDistance` 也不会真的去改阴影级联。它们是"能写能读、语义与 Unity
// 一致、但还不生效"的状态。
//
// 这与 `Screen.SetFullScreen` 的处理不同（那里是干脆不导出）。区别在：全屏
// 切不了是**窗口后端的缺口**，导出它等于承诺一个做不到的事；而画质档位是
// **上层策略**，脚本侧本来就该有地方表达"我要中等画质"，渲染侧什么时候来
// 读是实现进度问题。缺一个"用户意图"会让"画质设置面板"这种需求无处下手。
//
// 每接一项进来，就在下面的列表里划掉一项，并在 `ScriptQuality.cpp` 的
// `PresetTable` 注释里记一笔（预设值随接线进度会调）。
//
// 档位与预设
// ----------
// `SetQualityLevel(i)` 会把预设表里第 i 档的值**整体覆盖**到当前开关上 —— 与
// Unity 一致（Unity 里档位就是一组曲线上的取值）。所以"先 SetQualityLevel(3)
// 再单独改 antiAliasing"是合法的：后者只改那一项，不会把档位打回。
//
// 反过来 `SetQualityLevel` 会覆盖你手动改过的项 —— 这是 Unity 的行为，有意
// 保留。想"档位 + 少量覆盖"就在切档之后再改。
//
// 名字用 Unity 的默认档位名（"Very Low" ... "Ultra"），这样从 Unity 搬过来
// 的 UI 文案不用改。

// Unity 的 `UnityEngine.ShadowResolution`。
ZENUM()
enum class ShadowResolution : int
{
    Low      = 0,
    Medium   = 1,
    High     = 2,
    VeryHigh = 3,
};

// Unity 的 `UnityEngine.ShadowProjection`。
ZENUM()
enum class ShadowProjection : int
{
    CloseFit  = 0,
    StableFit = 1,
};

// Unity 的 `UnityEngine.AnisotropicFiltering`。
ZENUM()
enum class AnisotropicFiltering : int
{
    Disable     = 0,
    Enable      = 1,
    ForceEnable = 2,
};

// Unity 的 `UnityEngine.SkinWeights`。取值与 Unity 一致（1/2/4/255），不是
// 0/1/2/3 —— 搬过来的代码里 `SkinWeights.FourBones == 4` 这类判断要能对。
ZENUM()
enum class SkinWeights : int
{
    OneBone   = 1,
    TwoBones  = 2,
    FourBones = 4,
    Unlimited = 255,
};

ZCLASS()
class QualitySettings : public Object
{
    ZGENERATED_BODY(QualitySettings)

public:
    // ---- 档位 ----

    /// 当前档位下标 [0, GetQualityLevelCount())。
    /// Unity 的 `QualitySettings.GetQualityLevel()` / `qualityLevel`。
    ZFUNCTION()
    static int GetQualityLevel();
    ZSCRIPTPROPERTY(Name = "qualityLevel", Getter = "GetQualityLevel", Setter = "SetQualityLevel")

    /// 切档：把预设表里这一档的值覆盖到所有开关上。
    ///
    /// **越界时不改任何东西**（与 Unity 一致：它忽略越界调用，而不是夹紧）。
    /// 夹紧会让 `SetQualityLevel(99)` 悄悄变成"最高档" —— 那种"我传错了但看
    /// 起来成功了"比没反应更难查。
    ZFUNCTION()
    static void SetQualityLevel(int level);

    /// 提高一档（已是最高档则不动）。Unity 的
    /// `QualitySettings.IncreaseLevel()`。
    ZFUNCTION()
    static void IncreaseLevel();

    /// 降低一档（已是最低档则不动）。
    ZFUNCTION()
    static void DecreaseLevel();

    /// 档位数量。
    ZFUNCTION()
    static int GetQualityLevelCount();

    /// 全部档位名。Unity 的 `QualitySettings.names`。
    ZFUNCTION()
    static std::vector<eastl::string> GetNames();
    ZSCRIPTPROPERTY(Name = "names", Getter = "GetNames")

    /// 第 `level` 档的名字。越界返回空串。
    ZFUNCTION()
    static eastl::string GetNameForLevel(int level);

    /// 当前档位的名字。Unity 的 `QualitySettings.name`（**可写**，但 Unity 的
    /// setter 是改 Unity 内部那张表的名字，这里没那张表，所以只给读）。
    ZFUNCTION()
    static eastl::string GetName();
    ZSCRIPTPROPERTY(Name = "name", Getter = "GetName")

    // ---- 垂直同步 / 抗锯齿 ----

    /// 垂直同步：0 = 关，1 = 每个垂直 blank，2 = 每两个。Unity 的
    /// `QualitySettings.vSyncCount`。
    ZFUNCTION()
    static int GetVSyncCount();
    ZSCRIPTPROPERTY(Name = "vSyncCount", Getter = "GetVSyncCount", Setter = "SetVSyncCount")

    /// 设置垂直同步。负值夹到 0（"关"）。
    ZFUNCTION()
    static void SetVSyncCount(int count);

    /// MSAA 采样数（0 / 2 / 4 / 8）。Unity 的 `QualitySettings.antiAliasing`。
    ///
    /// ⚠️ 只接受 0/2/4/8：写了别的值会被夹到**不高于它的最近合法值**
    /// （3 -> 2，9 -> 8）。MSAA 的采样数在渲染管线里是编译期常量，夹一个
    /// 硬件不支持的值会在创 PSO 时才炸。
    ZFUNCTION()
    static int GetAntiAliasing();
    ZSCRIPTPROPERTY(Name = "antiAliasing", Getter = "GetAntiAliasing", Setter = "SetAntiAliasing")

    /// 设置 MSAA 采样数。
    ZFUNCTION()
    static void SetAntiAliasing(int samples);

    // ---- 阴影 ----

    /// 阴影投射距离（米）。Unity 的 `QualitySettings.shadowDistance`。
    ZFUNCTION()
    static float GetShadowDistance();
    ZSCRIPTPROPERTY(Name = "shadowDistance", Getter = "GetShadowDistance",
                    Setter = "SetShadowDistance")

    /// 设置阴影距离。负值夹到 0。
    ZFUNCTION()
    static void SetShadowDistance(float distance);

    /// 级联数（0 / 2 / 4）。Unity 的 `QualitySettings.shadowCascades`。
    ZFUNCTION()
    static int GetShadowCascades();
    ZSCRIPTPROPERTY(Name = "shadowCascades", Getter = "GetShadowCascades",
                    Setter = "SetShadowCascades")

    /// 设置级联数。只接受 0/2/4，其余夹到不高于它的合法值（与 antiAliasing
    /// 同一条理由）。
    ZFUNCTION()
    static void SetShadowCascades(int cascades);

    /// 阴影贴图分辨率档位。
    ZFUNCTION()
    static ShadowResolution GetShadowResolution();
    ZSCRIPTPROPERTY(Name = "shadowResolution", Getter = "GetShadowResolution",
                    Setter = "SetShadowResolution")

    /// 设置阴影贴图分辨率档位。
    ZFUNCTION()
    static void SetShadowResolution(ShadowResolution resolution);

    /// 阴影投影方式（CloseFit 更锐利，StableFit 不抖）。
    ZFUNCTION()
    static ShadowProjection GetShadowProjection();
    ZSCRIPTPROPERTY(Name = "shadowProjection", Getter = "GetShadowProjection",
                    Setter = "SetShadowProjection")

    /// 设置阴影投影方式。
    ZFUNCTION()
    static void SetShadowProjection(ShadowProjection projection);

    /// 阴影近平面偏移。Unity 的 `QualitySettings.shadowNearPlaneOffset`。
    ZFUNCTION()
    static float GetShadowNearPlaneOffset();
    ZSCRIPTPROPERTY(Name = "shadowNearPlaneOffset", Getter = "GetShadowNearPlaneOffset",
                    Setter = "SetShadowNearPlaneOffset")

    /// 设置阴影近平面偏移。负值夹到 0。
    ZFUNCTION()
    static void SetShadowNearPlaneOffset(float offset);

    // ---- 光照 / 纹理 ----

    /// 逐像素光源数上限。Unity 的 `QualitySettings.pixelLightCount`。
    ZFUNCTION()
    static int GetPixelLightCount();
    ZSCRIPTPROPERTY(Name = "pixelLightCount", Getter = "GetPixelLightCount",
                    Setter = "SetPixelLightCount")

    /// 设置逐像素光源数。负值夹到 0。
    ZFUNCTION()
    static void SetPixelLightCount(int count);

    /// 全局纹理 mip 上限（0 = 原图，1 = 最大 mip 减一，…）。
    /// Unity 的 `QualitySettings.masterTextureLimit`。
    ZFUNCTION()
    static int GetMasterTextureLimit();
    ZSCRIPTPROPERTY(Name = "masterTextureLimit", Getter = "GetMasterTextureLimit",
                    Setter = "SetMasterTextureLimit")

    /// 设置全局 mip 上限。夹到 [0,3] —— 再大就跳过整张纹理，画出来是全白。
    ZFUNCTION()
    static void SetMasterTextureLimit(int limit);

    /// 各向异性过滤。
    ZFUNCTION()
    static AnisotropicFiltering GetAnisotropicFiltering();
    ZSCRIPTPROPERTY(Name = "anisotropicFiltering", Getter = "GetAnisotropicFiltering",
                    Setter = "SetAnisotropicFiltering")

    /// 设置各向异性过滤。
    ZFUNCTION()
    static void SetAnisotropicFiltering(AnisotropicFiltering filtering);

    // ---- 粒子 / 蒙皮 / LOD ----

    /// 粒子是否与不透明几何做软混合。
    ZFUNCTION()
    static bool GetSoftParticles();
    ZSCRIPTPROPERTY(Name = "softParticles", Getter = "GetSoftParticles",
                    Setter = "SetSoftParticles")

    /// 设置软粒子。
    ZFUNCTION()
    static void SetSoftParticles(bool enabled);

    /// 公告板是否始终朝向相机位置。
    ZFUNCTION()
    static bool GetBillboardsFaceCameraPosition();
    ZSCRIPTPROPERTY(Name = "billboardsFaceCameraPosition", Getter = "GetBillboardsFaceCameraPosition",
                    Setter = "SetBillboardsFaceCameraPosition")

    /// 设置公告板朝向。
    ZFUNCTION()
    static void SetBillboardsFaceCameraPosition(bool enabled);

    /// 每根骨骼影响顶点的最大骨骼数。
    ZFUNCTION()
    static SkinWeights GetSkinWeights();
    ZSCRIPTPROPERTY(Name = "skinWeights", Getter = "GetSkinWeights", Setter = "SetSkinWeights")

    /// 设置蒙皮骨骼数上限。
    ZFUNCTION()
    static void SetSkinWeights(SkinWeights weights);

    /// LOD 偏移量（>0 倾向低模）。Unity 的 `QualitySettings.lodBias`。
    ZFUNCTION()
    static float GetLodBias();
    ZSCRIPTPROPERTY(Name = "lodBias", Getter = "GetLodBias", Setter = "SetLodBias")

    /// 设置 LOD 偏移。夹到 [0,10]。
    ZFUNCTION()
    static void SetLodBias(float bias);

    /// 最高可用 LOD 级（0 = 只用最高模）。
    ZFUNCTION()
    static int GetMaximumLODLevel();
    ZSCRIPTPROPERTY(Name = "maximumLODLevel", Getter = "GetMaximumLODLevel",
                    Setter = "SetMaximumLODLevel")

    /// 设置最高 LOD 级。负值夹到 0。
    ZFUNCTION()
    static void SetMaximumLODLevel(int level);

    // ---- 上传 / 排队 ----

    /// 允许排队的帧数上限。Unity 的 `QualitySettings.maxQueuedFrames`。
    ZFUNCTION()
    static int GetMaxQueuedFrames();
    ZSCRIPTPROPERTY(Name = "maxQueuedFrames", Getter = "GetMaxQueuedFrames",
                    Setter = "SetMaxQueuedFrames")

    /// 设置排队帧数。夹到 [1,8]。
    ZFUNCTION()
    static void SetMaxQueuedFrames(int frames);

    /// 异步上传每帧时间预算（毫秒）。
    ZFUNCTION()
    static int GetAsyncUploadTimeSlice();
    ZSCRIPTPROPERTY(Name = "asyncUploadTimeSlice", Getter = "GetAsyncUploadTimeSlice",
                    Setter = "SetAsyncUploadTimeSlice")

    /// 设置异步上传时间预算。夹到 [1,32]。
    ZFUNCTION()
    static void SetAsyncUploadTimeSlice(int milliseconds);

    /// 异步上传环形缓冲大小（MB）。
    ZFUNCTION()
    static int GetAsyncUploadBufferSize();
    ZSCRIPTPROPERTY(Name = "asyncUploadBufferSize", Getter = "GetAsyncUploadBufferSize",
                    Setter = "SetAsyncUploadBufferSize")

    /// 设置异步上传缓冲大小。夹到 [1,1024]。
    ZFUNCTION()
    static void SetAsyncUploadBufferSize(int megabytes);

    /// 粒子射线检测预算（每帧多少条）。
    ZFUNCTION()
    static int GetParticleRaycastBudget();
    ZSCRIPTPROPERTY(Name = "particleRaycastBudget", Getter = "GetParticleRaycastBudget",
                    Setter = "SetParticleRaycastBudget")

    /// 设置粒子射线检测预算。夹到 [1,4096]。
    ZFUNCTION()
    static void SetParticleRaycastBudget(int budget);

    // ---- 复位 ----

    /// 把所有开关与当前档位恢复到出厂值（中档）。
    ///
    /// 测试与"恢复默认设置"按钮用。Unity 没有这一项（它在编辑器里改的是
    /// 工程资产），这里的状态是进程内的，得有个还回去的入口。
    ZFUNCTION()
    static void ResetToDefaults();
};
