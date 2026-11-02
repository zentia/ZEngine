#include "Runtime/Scripting/ScriptLayerMask/ScriptLayerMask.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Function/Framework/TagLayer/TagLayerManager.h"

// 实现全在 `TagLayerManager`。这里只补 Unity 的拼写 —— 复制一份层表（或复制
// 一份"1 << layer"）迟早会和物理查询用的那份不一致，而不一致的表现是"射线
// 打不到东西"，没有任何报错。

namespace Runtime
{
    int32_t LayerMask::NameToLayer(const eastl::string& name)
    {
        const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
        if (tag_layer == nullptr)
        {
            return TagLayerManager::kInvalidLayer;
        }
        return tag_layer->NameToLayer(name);
    }

    eastl::string LayerMask::LayerToName(int32_t layer)
    {
        const TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
        if (tag_layer == nullptr)
        {
            return eastl::string();
        }
        return tag_layer->LayerToName(layer);
    }

    int32_t LayerMask::GetMask(const std::vector<eastl::string>& layer_names)
    {
        TagLayerManager* tag_layer = GET_SYSTEM(TagLayerManager);
        if (tag_layer == nullptr)
        {
            return 0;
        }

        // `TagLayerManager::GetMask` 吃 eastl 的 vector；绑定层给的是 std 的
        // （`Converter<std::vector<T>>`）。这里转一次，不去改数据所在处的签名。
        eastl::vector<eastl::string> names;
        names.reserve(layer_names.size());
        for (const eastl::string& name : layer_names)
        {
            names.push_back(name);
        }
        return static_cast<int32_t>(tag_layer->GetMask(names));
    }

    bool LayerMask::MaskContainsLayer(int32_t mask, int32_t layer)
    {
        // 两端类型：位运算按无符号做（`1u << 31` 不是负数），入参是 int32
        // （与 `Physics` 那批查询的 `layerMask` 同类型）。
        return TagLayerManager::MaskContainsLayer(static_cast<uint32_t>(mask), layer);
    }
}  // namespace Runtime
