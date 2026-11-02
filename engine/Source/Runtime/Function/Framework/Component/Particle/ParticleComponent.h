#pragma once

#include "Runtime/Core/Math/Matrix4.h"
#include "Runtime/Function/Framework/Component/Component.h"
#include "Runtime/Function/Particle/ParticleDesc.h"
#include "Runtime/Resource/ResType/Components/Emitter.h"
#include "ParticleComponent.generated.h"

// 粒子系统的播放状态（Unity 的 `ParticleSystem` 的 isPlaying / isPaused /
// isStopped 三态）。
//
// Unity 用三个 bool 表达，实际上它们**不是**独立的：`isStopped` 为真时另外
// 两个必为假。用一个三态枚举而不是三个 bool，是为了让"两个真同时成立"这种
// 不可能状态**表示不出来** —— 那种状态在 Tick 里的行为没有定义，而它只会
// 来自某处漏改一个 bool。
//
// 与 Unity 的一处差异：`Paused` 时**仍然更新 transform**（Unity 也是），
// 只是不推进模拟。暂停中移动发射器，恢复后粒子从新位置继续 —— 而不是
// "移动被吞掉"。
//
// ⚠️ 为什么在**全局**命名空间而不是 `namespace Runtime` 里：ZHeaderTool 生成
// 的 `.gen.cpp` 在全局作用域里写 `static_cast<ParticlePlayState (...)>`，
// **不会**给枚举加命名空间限定。放进去就是一堆
// "syntax error: identifier 'ParticlePlayState'"。已有先例（`ParticleComponent`
// 本身是 `Runtime::` 的，所以限定是有的，枚举没有）。
ZENUM()
enum class ParticlePlayState : int
{
    Stopped = 0,
    Playing = 1,
    Paused  = 2,
};

namespace Runtime
{
    ZCLASS()
    class ParticleComponent : public Component
    {
        ZGENERATED_BODY(ParticleComponent)

    public:
        ParticleComponent() {}

        void PostLoadResource(GameObject* parent_object) override;

        void Tick(float delta_time) override;

        // =====================================================================
        // 脚本门面（Unity 的 `ParticleSystem` 的播放控制那一部分）
        // =====================================================================
        //
        // Unity 的 ParticleSystem 有几十个模块（main / emission / shape /
        // velocityOverLifetime …）。这里只有**播放控制**，因为模块那一半对应
        // 的是 `ParticleComponentRes` 的字段 —— 那些字段走的是资产序列化，不是
        // 运行时脚本 API，要导出得先把 `ParticleDesc.h` 那一层抬上来。
        //
        // ⚠️ 没有 `Emit(count)` / `GetParticles()` / `SetParticles()`
        // ----------------------------------------------------------
        // 这些要能**读写单个粒子**，而本引擎的粒子是 GPU 驱动的（见
        // ParticleManager / ParticleGpuEmitter）：粒子数据住在 GPU buffer 里，
        // CPU 侧没有可读回的数组。给一个 `GetParticles()` 却只能返回空数组，
        // 是典型的假成功。

        /// 开始播放。Unity 的 `ParticleSystem.Play()`。
        ///
        /// 已经在 Playing 时是空操作（**不重置时间** —— Unity 的 Play 也不重置，
        /// 要重头播得先 `Stop()` 或 `Clear()`）。
        ZFUNCTION()
        void Play();

        /// 停止：不推进模拟。Unity 的 `ParticleSystem.Stop()`。
        ZFUNCTION()
        void Stop();

        /// 暂停：保留当前时间，不推进。Unity 的 `ParticleSystem.Pause()`。
        ///
        /// 已经 Stopped 时**不**改成 Paused：Unity 的 Pause 对已停止的系统没有
        /// 作用（它只暂停"正在播的"）。
        ZFUNCTION()
        void Pause();

        /// 停止并把时间归零。Unity 的 `ParticleSystem.Clear()`。
        ZFUNCTION()
        void Clear();

        /// 是否正在播放。
        ZFUNCTION()
        bool IsPlaying() const { return m_PlayState == ParticlePlayState::Playing; }
        ZSCRIPTPROPERTY(Name = "isPlaying", Getter = "IsPlaying")

        /// 是否已暂停。
        ZFUNCTION()
        bool IsPaused() const { return m_PlayState == ParticlePlayState::Paused; }
        ZSCRIPTPROPERTY(Name = "isPaused", Getter = "IsPaused")

        /// 是否已停止。
        ZFUNCTION()
        bool IsStopped() const { return m_PlayState == ParticlePlayState::Stopped; }
        ZSCRIPTPROPERTY(Name = "isStopped", Getter = "IsStopped")

        /// 是否"活着"（Unity 的 `ParticleSystem.IsAlive()`：还在播或还有粒子）。
        ///
        /// 这里等价于"没停止"。GPU 粒子没有"还有几个活着"的 CPU 侧计数，所以
        /// 不做更细的判定 —— 宁可语义窄一点，也不要一个"大概还活着"的值。
        ZFUNCTION()
        bool IsAlive() const { return m_PlayState != ParticlePlayState::Stopped; }
        ZSCRIPTPROPERTY(Name = "isAlive", Getter = "IsAlive")

        /// 累计模拟时间（秒）。Unity 的 `ParticleSystem.time`。
        ///
        /// ⚠️ 这是**组件自己记的**时间，不是"发射器内部的时间"：GPU 侧没有回读
        /// 通路。它的用途是"我暂停 / 恢复了多久"这类脚本侧记账。
        ZFUNCTION()
        float GetTime() const { return m_Time; }
        ZSCRIPTPROPERTY(Name = "time", Getter = "GetTime", Setter = "SetTime")

        /// 设置累计时间。负值夹到 0。
        ZFUNCTION()
        void SetTime(float time) { m_Time = time > 0.0f ? time : 0.0f; }

        /// 播放状态（三态枚举本身，给要一次拿全的场合）。
        ZFUNCTION()
        ParticlePlayState GetPlayState() const { return m_PlayState; }
        ZSCRIPTPROPERTY(Name = "playState", Getter = "GetPlayState", Setter = "SetPlayState")

        /// 直接设置播放状态。
        ZFUNCTION()
        void SetPlayState(ParticlePlayState state);

    private:
        void ComputeGlobalTransform();

        ZPROPERTY()
        ParticleComponentRes m_ParticleRes;

        Matrix4x4 m_LocalTransform;

        ParticleEmitterTransformDesc m_TransformDesc;

        // 播放状态**不**序列化：它是运行时状态，存档里记"上次播到第几秒"没有
        // 意义（Unity 也不记）。
        ParticlePlayState m_PlayState {ParticlePlayState::Playing};
        float             m_Time {0.0f};
    };
}  // namespace Runtime
