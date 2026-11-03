#pragma once

#include "Runtime/Function/Framework/Component/Mesh/BaseRenderer.h"
#include "MeshRenderer.generated.h"

ZCLASS()
class MeshRenderer : public BaseRenderer
{
    ZGENERATED_BODY(MeshRenderer)

public:
    MeshRenderer() = default;

    /// 同对象上有没有配对的 MeshFilter —— 与 `MeshFilter.HasRenderer()` 是同一件
    /// 事的反向那一半。留着是因为"改了材质画面没变"最常见的原因是这个对象上
    /// 根本没有网格来源，而那条路是静默的。
    ///
    /// ⚠️ 顺带这也是本类能进 d.ts 的原因：`ZCLASS` 类若既无 ZFUNCTION 也无
    /// ZSCRIPTPROPERTY，codegen 不发 ⇒ 脚本里连 `MeshRenderer` 这个名字都拿
    /// 不到，`GetComponent(MeshRenderer)` 无从谈起。
    ZFUNCTION()
    bool HasMeshFilter() const;

private:
    GameObjectDesc BuildGameObjectDesc(const Transform* transform_component) const override;
};
