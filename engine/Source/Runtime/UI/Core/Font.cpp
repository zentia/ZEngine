#include "Runtime/UI/Core/Font.h"

#include "Runtime/Core/Base/SystemRegistry.h"
#include "Runtime/Project/ProjectInfo.h"

#include <filesystem>

IMPLEMENT_REGISTER_CLASS(Font);
IMPLEMENT_OBJECT_SERAILIZE(Font);
INSTANTIATE_TEMPLATE_TRANSFER_EXPORTED(Font)

template<typename TransferFunction>
void Font::Transfer(TransferFunction& transfer)
{
    transfer.Transfer(m_SourceRelPath, "source_rel_path");
    transfer.Transfer(m_DefaultSize, "default_size");
}

void Font::SetSourceRelPath(const eastl::string& path)
{
    m_SourceRelPath = path;
}

void Font::SetDefaultSize(int size)
{
    if (size > 0)
    {
        m_DefaultSize = size;
    }
}

void Font::SetSourcePath(const eastl::string& path)
{
    m_SourceRelPath = path;
}

eastl::string Font::GetSourcePath() const
{
    if (m_SourceRelPath.empty())
    {
        return eastl::string();
    }

    std::filesystem::path candidate(m_SourceRelPath.c_str());
    if (candidate.is_absolute())
    {
        return m_SourceRelPath;
    }

    if (const auto project_info = GET_SYSTEM(ProjectInfo))
    {
        const std::filesystem::path abs = project_info->GetProjectRoot() / candidate;
        return abs.generic_string().c_str();
    }

    return m_SourceRelPath;
}
