#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "ZSlate/Application/SlateApplication.h"
#include "ZSlate/Widgets/Input/SButton.h"
#include "ZSlate/Widgets/Text/STextBlock.h"
#include "ZSlate/Widgets/Layout/SScrollBox.h"
#include "ZSlate/Widgets/Input/SEditableTextBox.h"
#include "ZSlate/Widgets/Input/SCheckBox.h"
#include "ZSlate/Widgets/Layout/SBoxPanel.h"
#include "ZSlate/Widgets/Layout/SSpacer.h"
#include "ZSlate/Widgets/Panels/SBorder.h"
#include "ZSlate/Widgets/Views/SListView.h"
#include "ZSlate/Widgets/Text/SMultiLineEditableText.h"

namespace StackTraceApp
{

    // Settings structure
    struct StackTraceSettings
    {
        std::string Input;
        std::string Addr2linePath;
        std::vector<std::string> SymbolFiles;
        bool ShowDiagnostics = true;
    };

    // Resolve result
    struct ResolveResult
    {
        std::string FunctionName;
        std::string SourceLocation;
        std::string RawOutput;
        std::string Diagnostic;
    };

    // Main application class
    class StackTraceApp
    {
    public:
        StackTraceApp();
        ~StackTraceApp();

        void Initialize();
        void Shutdown();
        void OnFrame();
        void OnMouseEvent(int button, int action);
        void OnKeyboardEvent(int key, int scancode, int action, int mods);
        void OnScrollEvent(double xoffset, double yoffset);
        void OnResize(int width, int height);

        std::shared_ptr<ZSlate::SBoxPanel> GetRoot() const { return m_Root; }

    private:
        // UI Construction
        void BuildUI();
        void BuildTopPanel();
        void BuildSymbolPanel();
        void BuildInputPanel();
        void BuildButtonPanel();
        void BuildOutputPanel();
        void BuildStatusPanel();

        // Event Handlers
        void OnParseStack();
        void OnClear();
        void OnCopyOutput();
        void OnTestAddr2line();
        void OnViewLog();
        void OnAddSymbolFile();
        void OnRemoveSymbolFile();
        void OnClearSymbolFiles();
        void OnBrowseAddr2line();
        void OnAutoFindAddr2line();
        void OnShowDiagChanged(bool checked);

        // Parsing
        std::string ParseStackTrace(
            const std::string& input,
            const std::string& addr2linePath,
            const std::vector<std::string>& symbolFiles,
            bool showDiagnostics);
        ResolveResult ResolveAddress(
            const std::string& addr2linePath,
            const std::string& symbolFile,
            const std::string& address);
        std::string ResolveToolPath(const std::string& toolPath);
        std::string FindAddr2line();
        std::vector<std::string> FindAddr2lineCandidates();

        // File Operations
        std::string OpenFileDialog(const std::string& title, const std::string& filter);
        std::vector<std::string> OpenMultiFileDialog(const std::string& title, const std::string& filter);

        // Settings
        void LoadSettings();
        void SaveSettings();
        void Log(const std::string& message);
        void AppendLog(const std::string& input, const std::string& output);

        // Utility
        std::string GetSettingsPath();
        std::string GetLegacySettingsPath();
        std::string GetLogPath();
        std::string GetAppDataPath();

    private:
        // UI Root
        std::shared_ptr<ZSlate::SBoxPanel> m_Root;

        // Input Controls
        std::shared_ptr<ZSlate::SEditableTextBox> m_Addr2linePathBox;
        std::shared_ptr<ZSlate::SCheckBox> m_ShowDiagCheckBox;
        std::shared_ptr<ZSlate::SMultiLineEditableText> m_InputTextArea;
        std::shared_ptr<ZSlate::SMultiLineEditableText> m_OutputTextArea;
        std::shared_ptr<ZSlate::SListView<std::string>> m_SymbolFilesList;
        std::shared_ptr<ZSlate::TSimpleListViewDataSource<std::string>> m_SymbolFilesDataSource;

        // Status
        std::shared_ptr<ZSlate::STextBlock> m_StatusLabel;

        // Settings
        StackTraceSettings m_Settings;

        // State
        bool m_Initialized = false;
    };
}
