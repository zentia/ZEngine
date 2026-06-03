#pragma once

#include "Runtime/BaseClasses/Object.h"
#include "Runtime/Core/Serialize/SerializeUtility.h"
#include "Runtime/Utility/Utility.h"

struct CSharpInfo
{
    DECLARE_SERIALIZE(CSharpInfo)
    eastl::string name;
    eastl::string stack;
};

template<typename TransferFunction>
void CSharpInfo::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(name, "name");
    transfer.Transfer(stack, "stack");
}

struct CSharpDisplayData
{
    uint32_t count {0};
    eastl::string name;
    eastl::string stack;
};

struct UserdataDisplayData
{
    uint32_t count {0};
    eastl::string stack;
};

struct CSharpMemoryProfiler
{
    DECLARE_SERIALIZE(CSharpMemoryProfiler)

    std::unordered_map<int, CSharpInfo> data;

    std::unordered_map<uint64_t, eastl::string> userdata;

    eastl::vector<CSharpDisplayData> csharpInfos;
    bool csharpInited {false};

    eastl::vector<UserdataDisplayData> userdataInfos;
    bool userdataInited {false};

    eastl::vector<CSharpDisplayData>& GetCSharpInfos()
    {
        if (csharpInited)
            return csharpInfos;
        eastl::unordered_map<StringWithHash, CSharpDisplayData> sizeData;
        for (auto&& info : data)
        {
            eastl::string keyStr = info.second.name + "|" + info.second.stack;
            auto&& element = sizeData[StringWithHash(keyStr)];
            element.count += 1;
            element.name = info.second.name;
            element.stack = info.second.stack;
        }

        eastl::vector<eastl::pair<StringWithHash, CSharpDisplayData>> sortedData(sizeData.begin(), sizeData.end());
        eastl::sort(sortedData.begin(), sortedData.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

        csharpInfos.clear();
        csharpInfos.reserve(sortedData.size());
        for (const auto& item : sortedData)
        {
            csharpInfos.push_back({item.second.count, item.second.name, item.second.stack});
        }

        csharpInited = true;
        return csharpInfos;
    }

    eastl::vector<UserdataDisplayData>& GetUserdataInfos()
    {
        if (userdataInited)
            return userdataInfos;
        eastl::unordered_map<StringWithHash, UserdataDisplayData> sizeData;
        for (auto&& info : userdata)
        {
            auto&& element = sizeData[StringWithHash(info.second)];
            element.count += 1;
            element.stack = info.second;
        }

        eastl::vector<eastl::pair<StringWithHash, UserdataDisplayData>> sortedData(sizeData.begin(), sizeData.end());
        eastl::sort(sortedData.begin(), sortedData.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

        userdataInfos.clear();
        userdataInfos.reserve(sortedData.size());
        for (const auto& item : sortedData)
        {
            userdataInfos.push_back({item.second.count, item.second.stack});
        }

        userdataInited = true;
        return userdataInfos;
    }
};

template<typename TransferFunction>
void CSharpMemoryProfiler::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(data, "data");
    transfer.Transfer(userdata, "userdata");
}