#include "Runtime/BaseClasses/PropertyMeta.h"

#include "Runtime/BaseClasses/Type.h"

#include <cstring>

namespace PropertyMetaRegistry
{
    namespace
    {
        // Constant-initialized, so it is valid before any dynamic static init
        // (generated registrars run during dynamic init, in unspecified order).
        ZClassPropertyMeta* g_Head = nullptr;
    }  // namespace

    void Register(ZClassPropertyMeta& meta)
    {
        meta.next = g_Head;
        g_Head = &meta;
    }

    const ZClassPropertyMeta* FindClass(const Type* type)
    {
        for (const ZClassPropertyMeta* it = g_Head; it != nullptr; it = it->next)
        {
            if (it->type == type)
                return it;
        }
        return nullptr;
    }

    const ZClassPropertyMeta* Classes() { return g_Head; }

    const ZPropertyMeta* Find(const Type* type, const char* serialized_name)
    {
        if (serialized_name == nullptr)
            return nullptr;
        for (const Type* cursor = type; cursor != nullptr; cursor = cursor->base)
        {
            const ZClassPropertyMeta* cls = FindClass(cursor);
            if (cls == nullptr)
                continue;
            for (size_t i = 0; i < cls->count; ++i)
            {
                if (std::strcmp(cls->properties[i].serializedName, serialized_name) == 0)
                    return &cls->properties[i];
            }
        }
        return nullptr;
    }
}  // namespace PropertyMetaRegistry
