#include "MaterialCustomParamNames.h"

namespace MaterialCustomParams
{
    namespace
    {
        struct NamedLocation
        {
            const char* name;
            MaterialCustomParamLocation location;
        };

        // 槽位常量来自 RenderCommon.h，和 MeshMaterialUniform 里注释的约定一一对应。
        // 别在这里写魔法数字：改了槽位约定而忘了改这里，脚本会静默写到错的槽上。
        const NamedLocation k_FloatEntries[] = {
            {k_DissolveAmount, {k_MaterialCustomParamSlot_Dissolve, 0}},
            {k_DissolveEdgeWidth, {k_MaterialCustomParamSlot_Dissolve, 1}},
        };

        const NamedLocation k_VectorEntries[] = {
            {k_DissolveEdgeColor, {k_MaterialCustomParamSlot_DissolveEdgeColor, 0}},
            {k_CustomParam0, {k_MaterialCustomParamSlot_Custom0, 0}},
            {k_CustomParam1, {k_MaterialCustomParamSlot_Custom1, 0}},
        };

        template <size_t N>
        const MaterialCustomParamLocation* Lookup(const NamedLocation (&table)[N], const eastl::string& name)
        {
            for (const NamedLocation& entry : table)
            {
                if (name == entry.name)
                {
                    return &entry.location;
                }
            }
            return nullptr;
        }

        template <size_t N>
        std::vector<const char*> CollectNames(const NamedLocation (&table)[N])
        {
            std::vector<const char*> names;
            names.reserve(N);
            for (const NamedLocation& entry : table)
            {
                names.push_back(entry.name);
            }
            return names;
        }
    }  // namespace

    const MaterialCustomParamLocation* FindFloat(const eastl::string& name)
    {
        return Lookup(k_FloatEntries, name);
    }

    const MaterialCustomParamLocation* FindVector(const eastl::string& name)
    {
        return Lookup(k_VectorEntries, name);
    }

    const std::vector<const char*>& FloatNames()
    {
        static const std::vector<const char*> names = CollectNames(k_FloatEntries);
        return names;
    }

    const std::vector<const char*>& VectorNames()
    {
        static const std::vector<const char*> names = CollectNames(k_VectorEntries);
        return names;
    }
}  // namespace MaterialCustomParams
