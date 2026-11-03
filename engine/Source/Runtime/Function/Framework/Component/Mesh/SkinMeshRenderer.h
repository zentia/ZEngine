#pragma once

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "SkinMeshRenderer.generated.h"

class AnimationComponent;

ZCLASS()
class SkinMeshRenderer : public BaseRenderer
{
    ZGENERATED_BODY(SkinMeshRenderer)

public:
    SkinMeshRenderer() = default;

    /// 同对象上有没有 AnimationComponent 在驱动蒙皮。
    ///
    /// 没有就是退化成静态网格（`Tick` 里走的是 `BaseRenderer::Tick`）。脚本最常
    /// 撞上的坑是"挂了 SkinMeshRenderer 但角色不动"，而那条路**静默** ——
    /// 没有动画组件既不报错也不警告。给一条自查的路。
    ///
    /// ⚠️ 顺带这也是本类能进 d.ts 的原因：ZCLASS 类若既无 ZFUNCTION 也无
    /// ZSCRIPTPROPERTY，codegen 不发 ⇒ 脚本里连 `SkinMeshRenderer` 这个名字都
    /// 拿不到。
    ZFUNCTION()
    bool HasAnimator() const;

    void Tick(float delta_time) override;

private:
    GameObjectDesc BuildGameObjectDesc(const Transform* transform_component) const override;
};
