#include "Editor/EditorUI/PropertyDrawer/EditorPropertyDrawer.h"

#include <cctype>
#include <cstring>

namespace EditorPropertyDrawer
{
    std::string MakeDisplayLabel(const char* raw_name)
    {
        if (raw_name == nullptr || raw_name[0] == '\0')
        {
            return "Unnamed";
        }

        std::string label(raw_name);
        if (label.rfind("m_", 0) == 0)
        {
            label.erase(0, 2);
        }

        for (char& character : label)
        {
            if (character == '_')
            {
                character = ' ';
            }
        }

        bool capitalize_next = true;
        for (char& character : label)
        {
            if (capitalize_next && std::islower(static_cast<unsigned char>(character)))
            {
                character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            }
            capitalize_next = (character == ' ');
        }

        return label;
    }

    std::string MakeTypeHeaderLabel(const char* raw_type_name)
    {
        if (raw_type_name == nullptr || raw_type_name[0] == '\0')
        {
            return "Component";
        }

        if (std::strcmp(raw_type_name, "Transform") == 0)
        {
            return "Transform";
        }

        std::string label(raw_type_name);
        constexpr const char* suffixes[] = {"Component", "Parameter", "Res"};
        for (const char* suffix : suffixes)
        {
            const size_t suffix_length = std::strlen(suffix);
            if (label.size() > suffix_length && label.compare(label.size() - suffix_length, suffix_length, suffix) == 0)
            {
                label.erase(label.size() - suffix_length);
                break;
            }
        }

        return label;
    }
}
