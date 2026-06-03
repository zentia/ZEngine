#include "AssetsMenu.h"

#include "Editor/EditorAsset/EditorAssetManager.h"
#include "Runtime/Core/Base/Macro.h"
#include "Runtime/Project/ProjectInfo.h"

#include <filesystem>
#include <system_error>

namespace
{

    // Return the project's `<Project>/Assets/` content root in absolute,
    // canonical form. Empty when no project is loaded (caller falls through
    // to "import to source-file's own directory" legacy behaviour, which is
    // exactly the old pre-PR-PW2 contract -- nothing to lose).
    std::filesystem::path resolveProjectContentRoot()
    {
        ProjectInfo* project_info = GET_SYSTEM(ProjectInfo);
        if (project_info == nullptr)
        {
            return {};
        }
        const std::filesystem::path content = project_info->GetProjectContent();
        if (content.empty())
        {
            return {};
        }
        std::error_code ec;
        auto abs = std::filesystem::weakly_canonical(content, ec);
        if (ec || abs.empty())
        {
            abs = std::filesystem::absolute(content, ec);
        }
        return abs;
    }

    // Decide whether `candidate` lies inside `root` (or equals it). Both
    // arguments must already be absolute. Uses lexical comparison so the
    // check works even when `candidate` does not yet exist on disk (we only
    // use it for the parent directory of an import product, which the user
    // might be creating right now via the import dialog).
    bool isPathInsideRoot(const std::filesystem::path& candidate,
                          const std::filesystem::path& root)
    {
        if (candidate.empty() || root.empty())
        {
            return false;
        }
        auto cand_iter = candidate.begin();
        auto cand_end = candidate.end();
        auto root_iter = root.begin();
        auto root_end = root.end();
        for (; root_iter != root_end; ++root_iter, ++cand_iter)
        {
            if (cand_iter == cand_end)
            {
                return false;
            }
            if (*cand_iter != *root_iter)
            {
                return false;
            }
        }
        return true;
    }

    // Resolve the directory the .zasset product should land in. UE Content
    // Browser model: products always live under `<Project>/Assets/` so the
    // AssetRegistry can find them. The user's currently-selected folder is
    // honoured ONLY when it falls inside Assets/.
    std::filesystem::path
    resolveOutputDirectory(const std::filesystem::path& source_path,
                           const std::filesystem::path& target_dir)
    {
        const std::filesystem::path content_root = resolveProjectContentRoot();

        // No project loaded? Fall back to the legacy "next to source" rule
        // (this only happens before any project is open, which is rare for
        // the import dialog -- but keeps the function pure and safe).
        if (content_root.empty())
        {
            return source_path.parent_path();
        }

        if (!target_dir.empty())
        {
            std::error_code ec;
            std::filesystem::path target_abs = std::filesystem::weakly_canonical(target_dir, ec);
            if (ec || target_abs.empty())
            {
                target_abs = std::filesystem::absolute(target_dir, ec);
            }
            if (!target_abs.empty() && isPathInsideRoot(target_abs, content_root))
            {
                return target_abs;
            }
        }

        return content_root;
    }

}  // namespace

void AssetsMenu::ConvertAsset(eastl::string path, eastl::string target_dir)
{
    std::filesystem::path source_path(path.c_str());

    // Check if the source file exists
    if (!std::filesystem::exists(source_path))
    {
        LOG_ERROR(ZAsset, "Source file does not exist: {}", path.c_str());
        return;
    }

    std::filesystem::path target_dir_path =
        target_dir.empty() ? std::filesystem::path {} : std::filesystem::path(target_dir.c_str());

    // PR-PW2: route every import product into <Project>/Assets/... so the
    // AssetRegistry sees it. The source file itself stays wherever the user
    // picked it from on disk -- it is NEVER copied into Assets/, mirroring
    // UE's Content Browser policy that source assets are invisible to the
    // registry.
    std::filesystem::path output_dir = resolveOutputDirectory(source_path, target_dir_path);
    std::filesystem::path output_path = output_dir / source_path.stem();
    output_path.replace_extension(".zasset");

    // Ensure the destination directory exists (target_dir might be a brand-
    // new subfolder the user just created in the Project window).
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);

    auto editor_asset_mgr = dynamic_cast<EditorAssetManager*>(GET_SYSTEM(AssetManager));
    if (editor_asset_mgr == nullptr)
    {
        LOG_ERROR(ZAsset, "EditorAssetManager unavailable; cannot import {}", path.c_str());
        return;
    }

    // Importers are registered on EditorAssetManager::m_ImportManager, not on a
    // standalone AssetImportManager system (GET_SYSTEM(AssetImportManager) is null).
    const bool success =
        editor_asset_mgr->importSourceAsset(source_path, output_path, nullptr);

    if (success)
    {
        LOG_INFO(ZAsset, "Successfully converted asset: {} -> {}", path.c_str(), output_path.string());
    }
    else
    {
        LOG_ERROR(ZAsset, "Failed to convert asset: {}", path.c_str());
    }
}
