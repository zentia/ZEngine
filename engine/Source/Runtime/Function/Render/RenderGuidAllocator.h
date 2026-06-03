#pragma once

#include <unordered_map>

static const size_t s_InvalidGuid = 0;

template<typename T>
class GuidAllocator
{
public:
    static bool isValidGuid(size_t guid) { return guid != s_InvalidGuid; }

    size_t allocGuid(const T& t)
    {
        auto find_it = m_ElementsGuidMap.find(t);
        if (find_it != m_ElementsGuidMap.end())
        {
            return find_it->second;
        }

        for (size_t i = 0; i < m_GuidElementsMap.size() + 1; i++)
        {
            size_t guid = i + 1;
            if (m_GuidElementsMap.find(guid) == m_GuidElementsMap.end())
            {
                m_GuidElementsMap.insert(std::make_pair(guid, t));
                m_ElementsGuidMap.insert(std::make_pair(t, guid));
                return guid;
            }
        }

        return s_InvalidGuid;
    }

    bool getGuidRelatedElement(size_t guid, T& t)
    {
        auto find_it = m_GuidElementsMap.find(guid);
        if (find_it != m_GuidElementsMap.end())
        {
            t = find_it->second;
            return true;
        }
        return false;
    }

    bool getElementGuid(const T& t, size_t& guid)
    {
        auto find_it = m_ElementsGuidMap.find(t);
        if (find_it != m_ElementsGuidMap.end())
        {
            guid = find_it->second;
            return true;
        }
        return false;
    }

    bool hasElement(const T& t) { return m_ElementsGuidMap.find(t) != m_ElementsGuidMap.end(); }

    void freeGuid(size_t guid)
    {
        auto find_it = m_GuidElementsMap.find(guid);
        if (find_it != m_GuidElementsMap.end())
        {
            const auto& ele = find_it->second;
            m_ElementsGuidMap.erase(ele);
            m_GuidElementsMap.erase(guid);
        }
    }

    void freeElement(const T& t)
    {
        auto find_it = m_ElementsGuidMap.find(t);
        if (find_it != m_ElementsGuidMap.end())
        {
            const auto& guid = find_it->second;
            m_ElementsGuidMap.erase(t);
            m_GuidElementsMap.erase(guid);
        }
    }

    std::vector<size_t> getAllocatedGuids() const
    {
        std::vector<size_t> allocated_guids;
        for (const auto& ele : m_GuidElementsMap)
        {
            allocated_guids.push_back(ele.first);
        }
        return allocated_guids;
    }

    void clear()
    {
        m_ElementsGuidMap.clear();
        m_GuidElementsMap.clear();
    }

private:
    std::unordered_map<T, size_t> m_ElementsGuidMap;
    std::unordered_map<size_t, T> m_GuidElementsMap;
};
