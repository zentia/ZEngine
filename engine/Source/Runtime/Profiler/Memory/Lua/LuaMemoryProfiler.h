#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Utility/Utility.h"

#include <algorithm>

struct LuaInfo
{
    DECLARE_SERIALIZE(LuaInfo)
    uint32_t size;
    uint32_t type;
    eastl::string stack;
};

template<typename TransferFunction>
void LuaInfo::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(size, "size");
    transfer.Transfer(type, "type");
    transfer.Transfer(stack, "stack");
}

struct LuaDisplayData
{
    uint32_t count {0};
    size_t size {0};
    uint32_t type {0};
    eastl::string stack;
};

/* Type tag mirror of ltracker.h's lua_track_type_t (GameCore). Keep in sync. */
inline const char* LuaMemoryTypeName(uint32_t type)
{
    switch (type)
    {
    case 0: return "table";
    case 1: return "string";
    case 2: return "userdata";
    case 3: return "lua_closure";
    case 4: return "c_closure";
    case 5: return "proto";
    case 6: return "upval";
    case 7: return "thread";
    default: return "unknown";
    }
}

struct LuaMemoryProfiler
{
    DECLARE_SERIALIZE(LuaMemoryProfiler)

    std::unordered_map<uint64_t, LuaInfo> data;

    eastl::vector<LuaDisplayData> luaInfos;
    size_t size {0};

    eastl::vector<LuaDisplayData>& GetLuaInfos()
    {
        if (size > 0)
            return luaInfos;
        eastl::unordered_map<StringWithHash, LuaDisplayData> sizeData;
        for (auto&& info : data)
        {
            eastl::string keyStr = LuaMemoryTypeName(info.second.type);
            keyStr += "|";
            keyStr += info.second.stack;
            auto&& element = sizeData[StringWithHash(keyStr)];
            element.size += info.second.size;
            size += info.second.size;
            element.count += 1;
            element.type = info.second.type;
            element.stack = info.second.stack;
        }

        // 转换为 vector 并按 size 降序排序
        eastl::vector<eastl::pair<StringWithHash, LuaDisplayData>> sortedData(sizeData.begin(), sizeData.end());
        eastl::sort(sortedData.begin(), sortedData.end(), [](const auto& a, const auto& b) { return a.second.size > b.second.size; });

        // 放入 luaInfos
        luaInfos.clear();
        luaInfos.reserve(sortedData.size());
        for (const auto& item : sortedData)
        {
            luaInfos.push_back({item.second.count, item.second.size, item.second.type, item.second.stack});
        }

        return luaInfos;
    }
};

template<typename TransferFunction>
void LuaMemoryProfiler::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(data, "data");
}