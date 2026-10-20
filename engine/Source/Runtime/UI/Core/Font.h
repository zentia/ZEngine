#pragma once

#include "Runtime/BaseClasses/Object.h"

#include <EASTL/string.h>
#include "Font.generated.h"

// Runtime font asset (.zasset via FontImporter). Source TTF/OTF path is stored
// project-relative; GPU glyphs bake through UIGpuResources (ImFontAtlas).
ZCLASS()
class Font : public Object
{
    ZGENERATED_BODY(Font)

public:
    // Project-relative source path (forward slashes), e.g. "Fonts/MyFont.ttf".
    ZPROPERTY()
    eastl::string m_SourceRelPath;
    ZPROPERTY()
    int m_DefaultSize {16};

    const eastl::string& GetSourceRelPath() const { return m_SourceRelPath; }
    void SetSourceRelPath(const eastl::string& path);

    int GetDefaultSize() const { return m_DefaultSize; }
    void SetDefaultSize(int size);

    bool HasSource() const { return !m_SourceRelPath.empty(); }

    // Absolute path for filesystem / ImGui TTF load (resolves via ProjectInfo).
    eastl::string GetSourcePath() const;
    void SetSourcePath(const eastl::string& path);
};
