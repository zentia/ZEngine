#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Utility/Utility.h"

#include <algorithm>

struct LuaInfo
{
    DECLARE_SERIALIZE(LuaInfo)
    uint32_t size;
    eastl::string stack;
};

template<typename TransferFunction>
void LuaInfo::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(size, "size");
    transfer.Transfer(stack, "stack");
}

struct LuaDisplayData
{
    uint32_t count {0};
    size_t size {0};
    eastl::string stack;
};

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
            auto&& element = sizeData[StringWithHash(info.second.stack)];
            element.size += info.second.size;
            size += info.second.size;
            element.count += 1;
        }

        // 转换为 vector 并按 size 降序排序
        eastl::vector<eastl::pair<StringWithHash, LuaDisplayData>> sortedData(sizeData.begin(), sizeData.end());
        eastl::sort(sortedData.begin(), sortedData.end(), [](const auto& a, const auto& b) { return a.second.size > b.second.size; });

        // 放入 luaInfos
        luaInfos.clear();
        luaInfos.reserve(sortedData.size());
        for (const auto& item : sortedData)
        {
            luaInfos.push_back({item.second.count, item.second.size, item.first.str});
        }

        return luaInfos;
    }
};

template<typename TransferFunction>
void LuaMemoryProfiler::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(data, "data");
}