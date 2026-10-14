#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "StackTraceApp.h"
#include "ZSlate/Core/SlatePaint.h"
#include "ZSlate/Core/SlateClipboard.h"
#include "ZSlate/Application/SlateInput.h"

#include "Services/Paths.h"
#include "Services/JsonConfig.h"

#include <Windows.h>
#include <shobjidl.h>
#include <fstream>
#include <filesystem>
#include <regex>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace StackTraceApp
{
    // ============================================================================
    // Constructor / Destructor
    // ============================================================================

    StackTraceApp::StackTraceApp()
        : m_Root(std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical))
        , m_Addr2linePathBox(std::make_shared<ZSlate::SEditableTextBox>())
        , m_ShowDiagCheckBox(std::make_shared<ZSlate::SCheckBox>())
        , m_InputTextArea(std::make_shared<ZSlate::SMultiLineEditableText>())
        , m_OutputTextArea(std::make_shared<ZSlate::SMultiLineEditableText>())
        , m_StatusLabel(std::make_shared<ZSlate::STextBlock>())
    {
        m_StatusLabel->Text = "Ready";
        m_StatusLabel->FontSize = 16.0f;
        
        // Configure input text area
        m_InputTextArea->Options.FontSize = 16.0f;
        m_InputTextArea->Options.MinHeight = 200.0f;
        m_InputTextArea->Options.AutoWrap = false;
        m_InputTextArea->Options.ReadOnly = false;
        
        // Configure output text area
        m_OutputTextArea->Options.FontSize = 16.0f;
        m_OutputTextArea->Options.AutoWrap = false;
        m_OutputTextArea->Options.ReadOnly = true;
        
        // Initialize symbol files list
        m_SymbolFilesDataSource = std::make_shared<ZSlate::TSimpleListViewDataSource<std::string>>(std::vector<std::string>{});
        m_SymbolFilesList = std::make_shared<ZSlate::SListView<std::string>>();
        m_SymbolFilesList->SetDataSource(m_SymbolFilesDataSource);
        m_SymbolFilesList->Options.ItemHeight = 20.0f;
    }

    StackTraceApp::~StackTraceApp()
    {
        SaveSettings();
    }

    // ============================================================================
    // Initialization
    // ============================================================================

    void StackTraceApp::Initialize()
    {
        BuildUI();
        LoadSettings();
        m_Initialized = true;
    }

    void StackTraceApp::Shutdown()
    {
        SaveSettings();
        m_Initialized = false;
    }

    // ============================================================================
    // Helpers
    // ============================================================================

    // Create a button with text label
    static std::shared_ptr<ZSlate::SButton> MakeTextButton(
        const std::string& text, std::function<void()> onClick, float fontSize = 16.0f)
    {
        auto btn = std::make_shared<ZSlate::SButton>();
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text = text;
        label->FontSize = fontSize;
        label->Color = ZSlate::Colors::White;
        btn->SetContent(label);
        btn->OnClicked = std::move(onClick);
        return btn;
    }

    // ============================================================================
    // UI Construction
    // ============================================================================

    void StackTraceApp::BuildUI()
    {
        // Build all panels
        BuildTopPanel();
        BuildSymbolPanel();
        BuildInputPanel();
        BuildButtonPanel();
        BuildOutputPanel();
        BuildStatusPanel();

        // Set root content
        ZSlate::SlateApplication::Get().SetRootContent(m_Root);
    }

    void StackTraceApp::BuildTopPanel()
    {
        auto vbox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);

        // Row 1: label + textbox + browse
        auto hbox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Horizontal);

        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text = "addr2line:";
        label->FontSize = 16.0f;
        hbox->AddSlot(label);

        auto spacer = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f));
        hbox->AddSlot(spacer);

        m_Addr2linePathBox->Text = FindAddr2line();
        m_Addr2linePathBox->FontSize = 16.0f;
        m_Addr2linePathBox->MinWidth = 500.0f;
        hbox->AddSlot(m_Addr2linePathBox);

        hbox->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));

        // Browse button
        hbox->AddSlot(MakeTextButton("Browse...", [this]() { OnBrowseAddr2line(); }));

        hbox->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f)));

        // Auto-Find button (port of the C# "Auto-Find" entry)
        hbox->AddSlot(MakeTextButton("Auto-Find", [this]() { OnAutoFindAddr2line(); }));

        // Spacer
        hbox->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(12.0f, 1.0f)));

        // Show Diagnostics checkbox
        m_ShowDiagCheckBox->Checked = true;
        m_ShowDiagCheckBox->OnCheckStateChanged = [this](bool checked) {
            OnShowDiagChanged(checked);
        };
        hbox->AddSlot(m_ShowDiagCheckBox);

        auto diagLabel = std::make_shared<ZSlate::STextBlock>();
        diagLabel->Text = " Show Diag";
        diagLabel->FontSize = 14.0f;
        hbox->AddSlot(diagLabel);

        vbox->AddSlot(hbox);

        // Row 2: preset addr2line dropdown + quick-select buttons
        auto vbox2 = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);
        
        auto presetLabel = std::make_shared<ZSlate::STextBlock>();
        presetLabel->Text = "  Known addr2line paths (click to select, or type above / Browse):";
        presetLabel->FontSize = 13.0f;
        presetLabel->Color = ZSlate::UIColor(0.55f, 0.6f, 0.7f, 1.0f);
        vbox2->AddSlot(presetLabel);
        vbox2->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 2)));

        // Collect all known addr2line paths
        std::vector<std::string> knownPaths = {
            m_Addr2linePathBox->Text // current path always first
        };
        // Add paths from FindAddr2line search (predefined locations)
        auto scanned = FindAddr2lineCandidates();
        for (auto& p : scanned) {
            if (std::find(knownPaths.begin(), knownPaths.end(), p) == knownPaths.end())
                knownPaths.push_back(p);
        }

        // Create a row of quick-select buttons (wrap)
        auto presetRow = std::make_shared<ZSlate::SHorizontalBox>();
        int count = 0;
        for (auto& p : knownPaths) {
            if (p.empty() || p == "llvm-addr2line.exe") continue;
            auto fileName = std::filesystem::path(p).filename().string();
            auto btn = std::make_shared<ZSlate::SButton>();
            auto lbl = std::make_shared<ZSlate::STextBlock>();
            lbl->Text = fileName;
            lbl->FontSize = 12.0f;
            lbl->Color = ZSlate::UIColor(0.7f, 0.8f, 1.0f, 1.0f);
            btn->SetContent(lbl);
            std::string pathCopy = p;
            btn->OnClicked = [this, pathCopy]() {
                m_Addr2linePathBox->Text = pathCopy;
                m_StatusLabel->Text = "Selected: " + pathCopy;
            };
            presetRow->AddSlot(btn);
            presetRow->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(4, 1)));
            count++;
            if (count % 5 == 0) {
                vbox2->AddSlot(presetRow);
                vbox2->AddSlot(std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0, 2)));
                presetRow = std::make_shared<ZSlate::SHorizontalBox>();
            }
        }
        if (count % 5 != 0 || count == 0) {
            if (count == 0) {
                auto noFound = std::make_shared<ZSlate::STextBlock>();
                noFound->Text = "  (no other addr2line found — use Browse... or type path manually)";
                noFound->FontSize = 12.0f;
                noFound->Color = ZSlate::UIColor(0.4f, 0.45f, 0.5f, 1.0f);
                presetRow->AddSlot(noFound);
            }
            vbox2->AddSlot(presetRow);
        }

        vbox->AddSlot(vbox2);
        m_Root->AddSlot(vbox);
    }

    void StackTraceApp::BuildSymbolPanel()
    {
        auto hbox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Horizontal);

        // Left side: List view
        m_SymbolFilesList->Options.ItemHeight = 20.0f;
        m_SymbolFilesList->Options.SelectedBackgroundColor = ZSlate::UIColor(0.25f, 0.50f, 0.90f, 0.30f);
        m_SymbolFilesList->Options.BackgroundColor = ZSlate::UIColor(0.08f, 0.08f, 0.10f, 1.0f);
        
        // Set item generator
        m_SymbolFilesList->SetOnGenerateItemWidget([this](const std::string& item, int32_t index, const std::shared_ptr<ZSlate::SListView<std::string>>& listview) {
            auto itemWidget = std::make_shared<ZSlate::STextBlock>();
            itemWidget->Text = item;
            itemWidget->FontSize = 16.0f;
            return itemWidget;
        });
        
        // Set selection changed callback
        m_SymbolFilesList->SetOnSelectionChanged([this](const std::vector<int32_t>& selectedIndices) {
            // Handle selection changes
        });
        
        hbox->AddSlot(m_SymbolFilesList);

        // Spacer
        auto spacer4 = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(8.0f, 1.0f));
        hbox->AddSlot(spacer4);

        // Right side: Buttons
        auto btnBox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);

        btnBox->AddSlot(MakeTextButton("Add...",  [this]() { OnAddSymbolFile(); }));
        btnBox->AddSlot(MakeTextButton("Remove",  [this]() { OnRemoveSymbolFile(); }));

        auto spacerBtn = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(1.0f, 4.0f));
        btnBox->AddSlot(spacerBtn);

        btnBox->AddSlot(MakeTextButton("Clear All", [this]() { OnClearSymbolFiles(); }));

        hbox->AddSlot(btnBox);

        m_Root->AddSlot(hbox);
    }

    void StackTraceApp::BuildInputPanel()
    {
        auto vbox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);

        // Label
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text = "Input Stack Trace:";
        label->FontSize = 16.0f;
        vbox->AddSlot(label);

        // Spacer
        auto spacer = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 4.0f));
        vbox->AddSlot(spacer);

        // Text area (fill remaining space)
        m_InputTextArea->Options.MinHeight = 180.0f;
        m_InputTextArea->Options.AutoWrap = false;
        m_InputTextArea->Options.ReadOnly = false;
        m_InputTextArea->Options.FontSize = 16.0f;
        vbox->AddSlot(m_InputTextArea);

        m_Root->AddSlot(vbox);
    }

    void StackTraceApp::BuildButtonPanel()
    {
        auto hbox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Horizontal);

        hbox->AddSlot(MakeTextButton("Parse Stack", [this]() { OnParseStack(); }));
        hbox->AddSlot(MakeTextButton("Clear",       [this]() { OnClear(); }));
        hbox->AddSlot(MakeTextButton("Copy",        [this]() { OnCopyOutput(); }));
        hbox->AddSlot(MakeTextButton("Test",        [this]() { OnTestAddr2line(); }));
        hbox->AddSlot(MakeTextButton("View Log",    [this]() { OnViewLog(); }));

        m_Root->AddSlot(hbox);
    }

    void StackTraceApp::BuildOutputPanel()
    {
        auto vbox = std::make_shared<ZSlate::SBoxPanel>(ZSlate::EOrientation::Vertical);

        // Label
        auto label = std::make_shared<ZSlate::STextBlock>();
        label->Text = "Output (Resolved Stack Trace):";
        label->FontSize = 16.0f;
        vbox->AddSlot(label);

        // Spacer
        auto spacer = std::make_shared<ZSlate::SSpacer>(ZSlate::Vector2(0.0f, 4.0f));
        vbox->AddSlot(spacer);

        // Read-only text area
        m_OutputTextArea->Options.AutoWrap = false;
        m_OutputTextArea->Options.ReadOnly = true;
        m_OutputTextArea->Options.FontSize = 16.0f;
        // Dark editor colours, like the C# output box (30,30,30 / 212,212,212).
        m_OutputTextArea->BackgroundColor = ZSlate::UIColor(0.118f, 0.118f, 0.118f, 1.0f);
        m_OutputTextArea->TextColor       = ZSlate::UIColor(0.831f, 0.831f, 0.831f, 1.0f);
        vbox->AddSlot(m_OutputTextArea);

        auto hint = std::make_shared<ZSlate::STextBlock>();
        hint->Text     = "Format: #01 pc 0000abcd  libexample.so";
        hint->FontSize = 12.0f;
        hint->Color    = ZSlate::UIColor(0.55f, 0.58f, 0.66f, 1.0f);
        vbox->AddSlot(hint);

        m_Root->AddSlot(vbox);
    }

    void StackTraceApp::BuildStatusPanel()
    {
        // Status label at bottom
        m_StatusLabel->FontSize = 11.0f;
        m_StatusLabel->Color = ZSlate::UIColor(0.7f, 0.7f, 0.7f, 1.0f);
        m_Root->AddSlot(m_StatusLabel);
    }

    // ============================================================================
    // Event Handlers
    // ============================================================================

    void StackTraceApp::OnAutoFindAddr2line()
    {
        m_StatusLabel->Text = "Searching for addr2line...";

        const std::string found = FindAddr2line();
        if (!found.empty())
        {
            m_Addr2linePathBox->SetText(found);
            m_Settings.Addr2linePath = found;
            SaveSettings();
            m_StatusLabel->Text = "Found: " + found;
            return;
        }

        m_StatusLabel->Text = "llvm-addr2line.exe not found. Install the HarmonyOS SDK or "
                              "Android NDK, or pick the executable with Browse...";
    }

    void StackTraceApp::AppendLog(const std::string& input, const std::string& output)
    {
        std::ofstream file(GetLogPath(), std::ios::app);
        if (!file.is_open()) return;

        const std::time_t now = std::time(nullptr);
        std::tm local {};
        localtime_s(&local, &now);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

        file << "=== " << stamp << " ===\n"
             << "--- INPUT ---\n" << input << "\n"
             << "--- OUTPUT ---\n" << output << "\n";
    }

    void StackTraceApp::OnParseStack()
    {
        std::string input = m_InputTextArea->GetText();
        input.erase(0, input.find_first_not_of(" \t\n\r"));
        input.erase(input.find_last_not_of(" \t\n\r") + 1);

        if (input.empty())
        {
            m_StatusLabel->Text = "Please enter a stack trace.";
            return;
        }

        std::string addr2linePath = m_Addr2linePathBox->Text;
        addr2linePath.erase(0, addr2linePath.find_first_not_of(" \t"));
        addr2linePath.erase(addr2linePath.find_last_not_of(" \t") + 1);

        if (addr2linePath.empty())
        {
            m_StatusLabel->Text = "Please specify the path to addr2line executable.";
            return;
        }

        std::string actualPath = ResolveToolPath(addr2linePath);
        if (actualPath.empty() || !std::filesystem::exists(actualPath))
        {
            m_StatusLabel->Text = "addr2line not found at: " + addr2linePath;
            return;
        }

        // Get symbol files
        std::vector<std::string> symbolFiles = m_SymbolFilesDataSource->GetItems();

        // Persist the input before doing anything else (C# saves on parse).
        SaveSettings();

        if (symbolFiles.empty())
        {
            m_StatusLabel->Text = "Please add at least one .so symbol file.";
            return;
        }

        // Port of the C# "No frames matched" fallback.
        {
            std::regex frameRegex(R"(^#\d+\s+pc\s+[0-9a-fA-F]+\s+.+$)");
            bool hasFrame = false;
            size_t pos = 0;
            while (pos <= input.size())
            {
                const size_t newline = input.find('\n', pos);
                std::string line = input.substr(pos, (newline == std::string::npos) ? std::string::npos : newline - pos);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
                if (std::regex_match(line, frameRegex)) { hasFrame = true; break; }
                if (newline == std::string::npos) break;
                pos = newline + 1;
            }
            if (!hasFrame)
            {
                m_OutputTextArea->SetText("No stack frames found. Expected format:\n"
                                          "#01 pc 0000abcd  libexample.so\n\nRaw input:\n" + input);
                m_OutputTextArea->ScrollToCursor();
                m_StatusLabel->Text = "No frames matched.";
                return;
            }
        }

        m_StatusLabel->Text = "Parsing stack trace...";
        m_OutputTextArea->SetText("");

        try
        {
            bool showDiag = m_ShowDiagCheckBox->Checked;
            std::string output = ParseStackTrace(input, actualPath, symbolFiles, showDiag);
            
            m_OutputTextArea->SetText(output);
            m_OutputTextArea->ScrollToCursor();

            AppendLog(input, output);

            int lineCount = std::count(output.begin(), output.end(), '\n') + 1;
            m_StatusLabel->Text = "Stack trace parsed successfully. " + std::to_string(lineCount) + " lines.";
            
            SaveSettings();
        }
        catch (const std::exception& ex)
        {
            m_OutputTextArea->SetText("=== ERROR ===\n" + std::string(ex.what()));
            m_OutputTextArea->ScrollToCursor();
            m_StatusLabel->Text = "Error parsing stack trace.";
        }
    }

    void StackTraceApp::OnClear()
    {
        m_InputTextArea->SetText("");
        m_OutputTextArea->SetText("");
        m_StatusLabel->Text = "Ready.";
    }

    void StackTraceApp::OnCopyOutput()
    {
        std::string outputText = m_OutputTextArea->GetText();
        if (outputText.empty())
        {
            m_StatusLabel->Text = "Nothing to copy.";
            return;
        }

        if (ZSlate::GSlateSetClipboard)
        {
            ZSlate::GSlateSetClipboard(outputText.c_str(), ZSlate::GSlateClipboardUserData);
            m_StatusLabel->Text = "Output copied to clipboard.";
        }
        else
        {
            m_StatusLabel->Text = "Clipboard not available.";
        }
    }

    void StackTraceApp::OnTestAddr2line()
    {
        std::string addr2linePath = m_Addr2linePathBox->Text;
        addr2linePath.erase(0, addr2linePath.find_first_not_of(" \t"));
        addr2linePath.erase(addr2linePath.find_last_not_of(" \t") + 1);

        if (addr2linePath.empty())
        {
            m_StatusLabel->Text = "Please specify addr2line path first.";
            return;
        }

        std::string actualPath = ResolveToolPath(addr2linePath);
        if (actualPath.empty() || !std::filesystem::exists(actualPath))
        {
            m_StatusLabel->Text = "addr2line not found at: " + addr2linePath;
            return;
        }

        // Test version
        std::string version;
        try
        {
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            std::string args = actualPath + " --version";
            
            if (CreateProcessA(nullptr, const_cast<char*>(args.data()), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                // Wait for process to complete
                WaitForSingleObject(pi.hProcess, INFINITE);
                
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                
                version = "addr2line found";
            }
        }
        catch (...)
        {
            version = "Unable to get version";
        }

        std::string testResult = "=== addr2line Test ===\n";
        testResult += "Path: " + actualPath + "\n";
        testResult += "Version: " + version + "\n";

        if (actualPath.find("strawberry") != std::string::npos)
        {
            testResult += "WARNING: This is Strawberry Perl's addr2line, NOT the NDK version!\n";
        }
        else
        {
            testResult += "This appears to be a valid addr2line tool.\n";
        }

        m_OutputTextArea->SetText(testResult);
        m_OutputTextArea->ScrollToCursor();
        m_StatusLabel->Text = "addr2line test completed.";
    }

    void StackTraceApp::OnViewLog()
    {
        std::string logPath = GetLogPath();
        if (std::filesystem::exists(logPath))
        {
            std::string cmd = "notepad.exe \"" + logPath + "\"";
            system(cmd.c_str());
        }
        else
        {
            m_StatusLabel->Text = "No log file found.";
        }
    }

    void StackTraceApp::OnAddSymbolFile()
    {
        std::vector<std::string> files = OpenMultiFileDialog(
            "Select Symbol File (.so with debug symbols)",
            "Symbol Files (*.so)|*.so|All Files (*.*)|*.*");

        std::vector<std::string> currentItems = m_SymbolFilesDataSource->GetItems();
        int added = 0;
        
        for (const auto& file : files)
        {
            if (std::filesystem::exists(file))
            {
                bool exists = false;
                for (const auto& item : currentItems)
                {
                    if (item == file)
                    {
                        exists = true;
                        break;
                    }
                }
                if (!exists)
                {
                    currentItems.push_back(file);
                    added++;
                }
            }
        }
        
        m_SymbolFilesDataSource->SetItems(currentItems);

        m_StatusLabel->Text = "Added " + std::to_string(added) + " symbol file(s). Total: " + 
                              std::to_string(currentItems.size());
    }

    void StackTraceApp::OnRemoveSymbolFile()
    {
        // Remove selected items
        const auto& selectedIndices = m_SymbolFilesList->GetSelectedIndices();
        std::vector<std::string> currentItems = m_SymbolFilesDataSource->GetItems();
        
        // Sort and remove in reverse order
        std::vector<int32_t> toRemove(selectedIndices);
        std::sort(toRemove.begin(), toRemove.end());
        
        for (auto it = toRemove.rbegin(); it != toRemove.rend(); ++it)
        {
            if (*it >= 0 && *it < static_cast<int32_t>(currentItems.size()))
            {
                currentItems.erase(currentItems.begin() + *it);
            }
        }
        
        m_SymbolFilesDataSource->SetItems(currentItems);

        m_StatusLabel->Text = "Removed " + std::to_string(selectedIndices.size()) + " symbol file(s). Remaining: " +
                              std::to_string(currentItems.size());
    }

    void StackTraceApp::OnClearSymbolFiles()
    {
        m_SymbolFilesDataSource->SetItems(std::vector<std::string>{});
        m_StatusLabel->Text = "Cleared all symbol files.";
    }

    void StackTraceApp::OnBrowseAddr2line()
    {
        std::string file = OpenFileDialog(
            "Select llvm-addr2line.exe (or any addr2line)",
            "Executable Files (llvm-addr2line.exe;*.exe)|llvm-addr2line.exe;*.exe|Any Executable (*.exe)|*.exe|All Files (*.*)|*.*");

        if (!file.empty())
        {
            m_Addr2linePathBox->Text = file;
            m_StatusLabel->Text = "addr2line path set: " + file;
            SaveSettings();  // persist immediately
        }
    }

    void StackTraceApp::OnShowDiagChanged(bool checked)
    {
        m_Settings.ShowDiagnostics = checked;
    }

    // ============================================================================
    // Parsing
    // ============================================================================

    std::string StackTraceApp::ParseStackTrace(
        const std::string& input,
        const std::string& addr2linePath,
        const std::vector<std::string>& symbolFiles,
        bool showDiagnostics)
    {
        // Split input into lines
        std::vector<std::string> lines;
        std::string line;
        std::istringstream stream(input);
        while (std::getline(stream, line))
        {
            if (!line.empty())
                lines.push_back(line);
        }

        // Build symbol file dictionary
        std::unordered_map<std::string, std::string> symbolFileDict;
        std::string diagInfo;

        for (const auto& symbolFile : symbolFiles)
        {
            std::string fileName = symbolFile.substr(symbolFile.find_last_of("\\/") + 1);
            symbolFileDict[fileName] = symbolFile;

            try
            {
                auto size = std::filesystem::file_size(symbolFile);
                diagInfo += "  Symbol: " + fileName + " (" + std::to_string(size / (1024 * 1024)) + " MB)\n";
            }
            catch (...)
            {
                diagInfo += "  Symbol: " + fileName + " (cannot read file info)\n";
            }
        }

        int resolvedCount = 0;
        int noSymbolCount = 0;
        int errorCount = 0;

        // Build result lines
        std::string resultLines = "========== RESOLVED STACK TRACE ==========\n\n";

        std::regex stackRegex(R"(^)(#\d+)\s+pc\s+([0-9a-fA-F]+)\s+(.+?)(?:\(([0-9a-fA-F]+)\))?$)");

        for (const auto& line : lines)
        {
            std::string trimmedLine = line;
            trimmedLine.erase(0, trimmedLine.find_first_not_of(" \t"));
            trimmedLine.erase(trimmedLine.find_last_not_of(" \t") + 1);

            if (trimmedLine.empty())
            {
                resultLines += "\n";
                continue;
            }

            std::smatch match;
            if (std::regex_match(trimmedLine, match, stackRegex))
            {
                std::string frame = match[1].str();
                std::string address = match[2].str();
                std::string libraryPath = match[3].str();
                std::string buildId = match[4].matched ? match[4].str() : "";

                std::string libraryName = libraryPath.substr(libraryPath.find_last_of("\\/") + 1);

                resultLines += frame + " pc " + address + " ";

                if (libraryPath.length() > 50)
                    resultLines += "..." + libraryName;
                else
                    resultLines += libraryPath;

                if (!buildId.empty())
                    resultLines += "(" + buildId + ")";

                // Try to resolve — exact match first, then case-insensitive,
                // then suffix match (port of C# Addr2lineService.MapLibraryToSymbol).
                auto it = symbolFileDict.find(libraryName);
                if (it == symbolFileDict.end())
                {
                    auto lower = [](const std::string& value) {
                        std::string out = value;
                        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return out;
                    };
                    const std::string lowerName = lower(libraryName);

                    for (const auto& entry : symbolFileDict)
                    {
                        if (lower(entry.first) == lowerName) { it = symbolFileDict.find(entry.first); break; }
                    }
                    if (it == symbolFileDict.end())
                    {
                        for (const auto& entry : symbolFileDict)
                        {
                            const std::string lowerEntry = lower(entry.first);
                            if (lowerEntry.size() > lowerName.size() &&
                                lowerEntry.compare(lowerEntry.size() - lowerName.size(),
                                                   lowerName.size(), lowerName) == 0)
                            {
                                it = symbolFileDict.find(entry.first);
                                break;
                            }
                        }
                    }
                }
                if (it != symbolFileDict.end())
                {
                    try
                    {
                        ResolveResult result = ResolveAddress(addr2linePath, it->second, address);
                        if (!result.FunctionName.empty())
                        {
                            resultLines += " -> [OK] " + result.FunctionName;
                            if (!result.SourceLocation.empty())
                                resultLines += " [" + result.SourceLocation + "]";
                            resolvedCount++;
                        }
                        else if (!result.RawOutput.empty())
                        {
                            if (showDiagnostics)
                                resultLines += " -> [?? raw: " + result.RawOutput + "]";
                            else
                                resultLines += " -> [??]";
                            noSymbolCount++;
                        }
                        else
                        {
                            resultLines += " -> [??]";
                            noSymbolCount++;
                        }

                        if (showDiagnostics && !result.Diagnostic.empty())
                        {
                            resultLines += "\n     [diag: " + result.Diagnostic + "]";
                        }
                    }
                    catch (const std::exception& ex)
                    {
                        resultLines += " -> [Error: " + std::string(ex.what()) + "]";
                        errorCount++;
                    }
                }
                else
                {
                    resultLines += " -> [No symbol file for '" + libraryName + "']";
                    noSymbolCount++;
                }
            }
            else
            {
                resultLines += trimmedLine;
                if (showDiagnostics)
                    resultLines += "  <-- [unrecognized format]";
            }

            resultLines += "\n";
        }

        // Build final output
        std::string finalOutput;
        finalOutput += "========================================\n";
        time_t now = std::time(nullptr);
        tm* now_tm = std::localtime(&now);
        finalOutput += "  STACK TRACE RESULT (" + std::format("{:02d}:{:02d}:{:02d}", 
            now_tm->tm_hour,
            now_tm->tm_min,
            now_tm->tm_sec) + ")\n";
        finalOutput += "  Total: " + std::to_string(lines.size()) + 
                       " | Resolved: " + std::to_string(resolvedCount) + 
                       " | No Symbol: " + std::to_string(noSymbolCount) + 
                       " | Errors: " + std::to_string(errorCount) + "\n";
        finalOutput += "========================================\n\n";

        if (showDiagnostics)
        {
            finalOutput += "--- DIAGNOSTICS ---\n";
            finalOutput += "addr2line: " + addr2linePath + "\n";
            finalOutput += "Loaded symbol files:\n";
            finalOutput += diagInfo;
            
            std::string mappings;
            for (const auto& [name, path] : symbolFileDict)
            {
                if (!mappings.empty()) mappings += ", ";
                mappings += name;
            }
            finalOutput += "Library mappings: " + mappings + "\n";
            finalOutput += "--------------------\n\n";
        }

        finalOutput += resultLines;
        return finalOutput;
    }

    ResolveResult StackTraceApp::ResolveAddress(
        const std::string& addr2linePath,
        const std::string& symbolFile,
        const std::string& address)
    {
        std::string soFileName = symbolFile.substr(symbolFile.find_last_of("\\/") + 1);
        std::string logPrefix = "[Resolve] " + soFileName + " @ 0x" + address;

        Log(logPrefix + " - Start");
        Log(logPrefix + " - addr2line: " + addr2linePath);
        Log(logPrefix + " - symbol: " + symbolFile + " (exists=" + 
            (std::filesystem::exists(symbolFile) ? "True" : "False") + ")");

        std::string symbolFileName = symbolFile;
        std::replace(symbolFileName.begin(), symbolFileName.end(), '"', '"');
        std::string args = "-C -f -e \"" + symbolFileName + "\" 0x" + address;

        Log(logPrefix + " - Command: " + addr2linePath + " " + args);

        ResolveResult result;
        result.RawOutput = "";
        result.Diagnostic = "";

        try
        {
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi;

            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = CreateFileA("\\\\.\\pipe\\addr2line_output", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            si.hStdError = CreateFileA("\\\\.\\pipe\\addr2line_error", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);

            // Use a simpler approach with redirected output
            std::string output, error;
            FILE* pipe = _popen((addr2linePath + " " + args).c_str(), "r");
            if (pipe)
            {
                char buffer[256];
                while (fgets(buffer, sizeof(buffer), pipe))
                    output += buffer;
                _pclose(pipe);
            }

            Log(logPrefix + " - Exit code: 0");
            Log(logPrefix + " - Stdout: " + output);

            result.RawOutput = output;
            result.Diagnostic = "exit=0";

            // Parse output
            std::vector<std::string> outLines;
            std::string line;
            std::istringstream stream(output);
            while (std::getline(stream, line))
            {
                if (!line.empty())
                    outLines.push_back(line);
            }

            if (outLines.size() >= 2)
            {
                std::string functionName = outLines[0];
                std::string sourceLocation = outLines[1];

                Log(logPrefix + " - Function: " + functionName + ", Location: " + sourceLocation);

                if (functionName == "??" || functionName.empty())
                {
                    Log(logPrefix + " - No symbol found (??)");
                    return result;
                }

                result.FunctionName = functionName;
                result.SourceLocation = sourceLocation;
                Log(logPrefix + " - Resolved successfully");
                return result;
            }
            else
            {
                Log(logPrefix + " - Unexpected output format (lines=" + std::to_string(outLines.size()) + ")");
            }
        }
        catch (const std::exception& ex)
        {
            result.Diagnostic = "error=" + std::string(ex.what());
            Log(logPrefix + " - Error: " + ex.what());
        }

        return result;
    }

    std::string StackTraceApp::ResolveToolPath(const std::string& toolPath)
    {
        if (std::filesystem::exists(toolPath))
            return toolPath;

        // Search on PATH
        std::string fileName = toolPath.substr(toolPath.find_last_of("\\/") + 1);
        
        char pathEnv[32768];
        size_t len;
        getenv_s(&len, pathEnv, sizeof(pathEnv), "PATH");
        std::string path(pathEnv);

        std::istringstream iss(path);
        std::string dir;
        while (std::getline(iss, dir, ';'))
        {
            if (dir.empty()) continue;
            std::string candidate = dir + "\\" + fileName;
            if (std::filesystem::exists(candidate))
                return candidate;
        }

        return "";
    }

    std::string StackTraceApp::FindAddr2line()
    {
        // Try HarmonyOS paths first
        std::vector<std::string> candidates = {
            "C:\\Program Files\\Huawei\\DevEco Studio\\sdk\\default\\HarmonyOS-5.1.0\\openharmony\\native\\llvm\\bin\\llvm-addr2line.exe",
            "C:\\Program Files\\Huawei\\DevEcoStudio\\sdk\\default\\openharmony\\native\\llvm\\bin\\llvm-addr2line.exe",
            "C:\\Program Files (x86)\\Huawei\\DevEcoStudio\\sdk\\default\\harmonynative\\llvm\\bin\\llvm-addr2line.exe",
        };

        for (const auto& path : candidates)
        {
            if (std::filesystem::exists(path))
                return path;
        }

        // Search on PATH
        std::string fileName = "llvm-addr2line.exe";
        char pathEnv[32768];
        size_t len;
        getenv_s(&len, pathEnv, sizeof(pathEnv), "PATH");
        std::string path(pathEnv);

        std::istringstream iss(path);
        std::string dir;
        while (std::getline(iss, dir, ';'))
        {
            if (dir.empty()) continue;
            std::string candidate = dir + "\\" + fileName;
            if (std::filesystem::exists(candidate))
            {
                std::string lower = candidate;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find("strawberry") == std::string::npos)
                    return candidate;
            }
        }

        return "llvm-addr2line.exe";
    }

    std::vector<std::string> StackTraceApp::FindAddr2lineCandidates()
    {
        std::vector<std::string> result;

        // Known install locations
        std::vector<std::string> candidates = {
            // HarmonyOS
            "C:\\Program Files\\Huawei\\DevEco Studio\\sdk\\default\\HarmonyOS-5.1.0\\openharmony\\native\\llvm\\bin\\llvm-addr2line.exe",
            "C:\\Program Files\\Huawei\\DevEco Studio\\sdk\\default\\HarmonyOS-5.0.0\\openharmony\\native\\llvm\\bin\\llvm-addr2line.exe",
            // Android NDK common paths
            "C:\\Users\\" + std::string(std::getenv("USERNAME") ? std::getenv("USERNAME") : "") + "\\AppData\\Local\\Android\\Sdk\\ndk\\",
        };

        for (const auto& path : candidates) {
            if (path.find("ndk\\") != std::string::npos) {
                // Search Android NDK directories for llvm-addr2line.exe
                if (std::filesystem::exists(path)) {
                    for (auto& entry : std::filesystem::directory_iterator(path)) {
                        if (entry.is_directory()) {
                            auto cand = entry.path() / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-addr2line.exe";
                            if (std::filesystem::exists(cand)) result.push_back(cand.string());
                        }
                    }
                }
            } else if (std::filesystem::exists(path)) {
                result.push_back(path);
            }
        }

        // Search PATH for llvm-addr2line.exe (but skip Strawberry Perl)
        char pathEnv[32768]; size_t len;
        if (getenv_s(&len, pathEnv, sizeof(pathEnv), "PATH") == 0) {
            std::istringstream iss(pathEnv);
            std::string dir;
            while (std::getline(iss, dir, ';')) {
                if (dir.empty()) continue;
                std::string candidate = dir + "\\llvm-addr2line.exe";
                if (std::filesystem::exists(candidate)) {
                    std::string lower = candidate;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find("strawberry") == std::string::npos)
                        result.push_back(candidate);
                }
            }
        }

        return result;
    }

    // ============================================================================
    // File Operations (IFileDialog — modern Vista+ style)
    // ============================================================================

    static std::wstring ToWide(const std::string& s) {
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring ws(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
        return ws;
    }
    static std::string ToNarrow(const wchar_t* ws) {
        int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, nullptr, nullptr);
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }

    // Parse "Display|*.ext|Display2|*.ext2" into COMDLG_FILTERSPEC array
    static std::vector<COMDLG_FILTERSPEC> ParseFilter(const std::string& filter)
    {
        std::vector<COMDLG_FILTERSPEC> specs;
        std::string::size_type pos = 0;
        while (pos < filter.size()) {
            auto next = filter.find('|', pos);
            std::string name = filter.substr(pos, next - pos);
            pos = (next == std::string::npos) ? filter.size() : next + 1;
            next = filter.find('|', pos);
            std::string ext = (next == std::string::npos) ? filter.substr(pos) : filter.substr(pos, next - pos);
            pos = (next == std::string::npos) ? filter.size() : next + 1;
            specs.push_back({_wcsdup(ToWide(name).c_str()), _wcsdup(ToWide(ext).c_str())});
        }
        return specs;
    }

    std::string StackTraceApp::OpenFileDialog(const std::string& title, const std::string& filter)
    {
        std::string result;
        IFileOpenDialog* pDlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&pDlg))))
            return result;

        auto specs = ParseFilter(filter);
        pDlg->SetFileTypes((UINT)specs.size(), specs.data());
        pDlg->SetTitle(ToWide(title).c_str());

        DWORD flags;
        pDlg->GetOptions(&flags);
        pDlg->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

        if (SUCCEEDED(pDlg->Show(nullptr))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = ToNarrow(path);
                    CoTaskMemFree(path);
                }
                pItem->Release();
            }
        }

        for (auto& s : specs) { free((void*)s.pszName); free((void*)s.pszSpec); }
        pDlg->Release();
        return result;
    }

    std::vector<std::string> StackTraceApp::OpenMultiFileDialog(const std::string& title, const std::string& filter)
    {
        std::vector<std::string> result;
        IFileOpenDialog* pDlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&pDlg))))
            return result;

        auto specs = ParseFilter(filter);
        pDlg->SetFileTypes((UINT)specs.size(), specs.data());
        pDlg->SetTitle(ToWide(title).c_str());

        DWORD flags;
        pDlg->GetOptions(&flags);
        pDlg->SetOptions(flags | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT);

        if (SUCCEEDED(pDlg->Show(nullptr))) {
            IShellItemArray* pItems = nullptr;
            if (SUCCEEDED(pDlg->GetResults(&pItems))) {
                DWORD count = 0;
                pItems->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* pItem = nullptr;
                    if (SUCCEEDED(pItems->GetItemAt(i, &pItem))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            result.push_back(ToNarrow(path));
                            CoTaskMemFree(path);
                        }
                        pItem->Release();
                    }
                }
                pItems->Release();
            }
        }

        for (auto& s : specs) { free((void*)s.pszName); free((void*)s.pszSpec); }
        pDlg->Release();
        return result;
    }

    // ============================================================================
    // Settings
    // ============================================================================

    void StackTraceApp::LoadSettings()
    {
        // C#-compatible JSON: %USERPROFILE%\.client_proj_branches\stacktrace_settings.json
        {
            const Json::Object object = Json::LoadFromFile(GetSettingsPath());
            if (!object.empty())
            {
                m_InputTextArea->SetText(Json::OptStringCI(object, "input"));

                const std::string addr2line = Json::OptStringCI(object, "addr2line_path");
                if (!addr2line.empty()) m_Addr2linePathBox->Text = addr2line;

                m_ShowDiagCheckBox->Checked = Json::OptBoolCI(object, "show_diagnostics", true);

                if (const Json::Array* files = Json::OptArray(object, "symbol_files"))
                {
                    std::vector<std::string> items;
                    for (const auto& value : *files)
                    {
                        if (value && value->IsString() && std::filesystem::exists(value->AsString()))
                            items.push_back(value->AsString());
                    }
                    if (!items.empty()) m_SymbolFilesDataSource->SetItems(items);
                }

                m_StatusLabel->Text = "Settings loaded";
                return;
            }
        }

        // Legacy INI-style file written by older C++ builds.
        std::ifstream file(GetLegacySettingsPath());
        if (!file.is_open()) return;

        std::string line;
        std::string currentSection;
        while (std::getline(file, line))
        {
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t\n\r") + 1);
            if (line.empty()) continue;

            if (line.front() == '[' && line.back() == ']')
            {
                currentSection = line.substr(1, line.size() - 2);
                continue;
            }

            if (currentSection == "input")
            {
                std::string currentText = m_InputTextArea->GetText();
                if (currentText.empty())
                    m_InputTextArea->SetText(line);
                else
                    m_InputTextArea->SetText(currentText + "\n" + line);
            }
            else if (currentSection == "addr2line")
            {
                m_Addr2linePathBox->Text = line;
            }
            else if (currentSection == "symbol_files")
            {
                if (std::filesystem::exists(line))
                {
                    std::vector<std::string> items = m_SymbolFilesDataSource->GetItems();
                    items.push_back(line);
                    m_SymbolFilesDataSource->SetItems(items);
                }
            }
            else if (currentSection == "show_diag")
            {
                m_ShowDiagCheckBox->Checked = (line == "True" || line == "true");
            }
        }

        m_StatusLabel->Text = "Settings loaded";
    }

    void StackTraceApp::SaveSettings()
    {
        Json::Object object;
        object["input"]            = std::make_shared<Json::Value>(m_InputTextArea->GetText());
        object["addr2line_path"]   = std::make_shared<Json::Value>(m_Addr2linePathBox->Text);
        object["show_diagnostics"] = std::make_shared<Json::Value>(m_ShowDiagCheckBox->Checked);

        Json::Array files;
        for (const auto& item : m_SymbolFilesDataSource->GetItems())
            files.push_back(std::make_shared<Json::Value>(item));
        object["symbol_files"] = std::make_shared<Json::Value>(files);

        Json::SaveToFile(GetSettingsPath(), object);
        m_StatusLabel->Text = "Settings saved";
    }

    std::string StackTraceApp::GetSettingsPath()
    {
        // Same location/shape as the C# StackTraceSettingsStore.
        return (std::filesystem::path(ClientProj::Paths::ConfigDir()) / "stacktrace_settings.json").string();
    }

    std::string StackTraceApp::GetLegacySettingsPath()
    {
        char localAppData[32768];
        size_t len;
        getenv_s(&len, localAppData, sizeof(localAppData), "LOCALAPPDATA");
        return std::string(localAppData) + "\\ClientProjBranches\\stacktrace_settings.json";
    }

    std::string StackTraceApp::GetLogPath()
    {
        // C# writes its parse log here too.
        return (std::filesystem::path(ClientProj::Paths::ConfigDir()) / "stacktrace_log.txt").string();
    }

    std::string StackTraceApp::GetAppDataPath()
    {
        char appData[32768];
        size_t len;
        getenv_s(&len, appData, sizeof(appData), "APPDATA");
        std::string appDataStr(appData);
        
        return appDataStr + "\\ClientProjBranches";
    }

    void StackTraceApp::Log(const std::string& message)
    {
        try
        {
            std::string logPath = GetLogPath();
            std::filesystem::create_directories(logPath.substr(0, logPath.find_last_of("\\/")));

            time_t now = std::time(nullptr);
            tm* now_tm = std::localtime(&now);
            std::string timestamp = std::format("{:02d}:{:02d}:{:02d}", 
                now_tm->tm_hour, now_tm->tm_min, now_tm->tm_sec);

            std::ofstream file(logPath, std::ios_base::app);
            if (file.is_open())
            {
                file << "[" << timestamp << "] " << message << "\n";
            }
        }
        catch (...)
        {
            // Ignore log errors
        }
    }

    // ============================================================================
    // Frame / Input Handling
    // ============================================================================

    void StackTraceApp::OnFrame()
    {
        // Update UI on each frame
    }

    void StackTraceApp::OnMouseEvent(int button, int action)
    {
        // Forward to SlateApplication
    }

    void StackTraceApp::OnKeyboardEvent(int key, int scancode, int action, int mods)
    {
        // Forward to SlateApplication
    }

    void StackTraceApp::OnScrollEvent(double xoffset, double yoffset)
    {
        // Forward to SlateApplication
    }

    void StackTraceApp::OnResize(int width, int height)
    {
        // Forward to SlateApplication
    }
}
