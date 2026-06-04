# Install Linkingooo/renderdoc-mcp (Python, scheme B) and configure Cursor MCP.
# Requires Python 3.10+ and a RenderDoc install that ships renderdoc.pyd.
#
# Usage (from repo root):
#   powershell -ExecutionPolicy Bypass -File tools/setup_renderdoc_mcp.ps1
#   powershell -ExecutionPolicy Bypass -File tools/setup_renderdoc_mcp.ps1 -RebuildPythonModule

param(
    [switch]$RebuildPythonModule
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$McpRepo = Join-Path $RepoRoot "tools\renderdoc-mcp"
$CursorMcp = Join-Path $RepoRoot ".cursor\mcp.json"
$RenderDocRoot = Join-Path $RepoRoot "tools\renderdoc"
$RenderDocDev = Join-Path $RenderDocRoot "x64\Development"
$RenderDocPyModules = Join-Path $RenderDocDev "pymodules"

function Get-PythonPrefix {
    $prefix = python -c "import sys; print(sys.prefix)" 2>$null
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($prefix)) {
        return $null
    }
    return $prefix.Trim()
}

function Ensure-PythonZip($pythonPrefix) {
    $ver = python -c "import sys; print(f'{sys.version_info.major}{sys.version_info.minor}')" 2>$null
    if ($LASTEXITCODE -ne 0) { $ver = "310" }
    $zipName = "python$ver.zip"
    $zipPath = Join-Path $pythonPrefix $zipName
    if (Test-Path $zipPath) {
        return
    }

    $pyFull = python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}')" 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Could not determine Python version for python$ver.zip"
    }

    Write-Host "Missing $zipName under $pythonPrefix - fetching embeddable zip ..."
    $embedUrl = "https://www.python.org/ftp/python/$pyFull/python-$pyFull-embed-amd64.zip"
    $tempZip = Join-Path $env:TEMP "python-$pyFull-embed-amd64.zip"
    $tempDir = Join-Path $env:TEMP "pyembed-$pyFull"
    curl.exe -fL $embedUrl -o $tempZip
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to download $embedUrl (needed to build renderdoc.pyd against your Python)"
    }
    if (Test-Path $tempDir) { Remove-Item -Recurse -Force $tempDir }
    Expand-Archive -LiteralPath $tempZip -DestinationPath $tempDir -Force
    Copy-Item (Join-Path $tempDir $zipName) $zipPath -Force
    Write-Host "  Installed $zipPath"
}

function Build-RenderDocPythonModule {
    param([string]$PythonPrefix)

    Ensure-PythonZip $PythonPrefix

    $msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
        -latest -requires Microsoft.Component.MSBuild `
        -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    if (-not $msbuild) {
        throw "MSBuild not found. Install VS 2022 C++ workload."
    }

    Write-Host "Building pyrenderdoc_module against $PythonPrefix ..."
    $env:RENDERDOC_PYTHON_PREFIX64 = $PythonPrefix
    $proj = Join-Path $RenderDocRoot "qrenderdoc\Code\pyrenderdoc\pyrenderdoc_module.vcxproj"
    $args = @(
        $proj,
        "/m", "/nologo", "/v:m",
        "/p:SolutionDir=$RenderDocRoot\",
        "/p:Configuration=Development",
        "/p:Platform=x64",
        "/p:PlatformToolset=v143"
    )

    $vctools = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC\14.44.35207"
    if (Test-Path $vctools) {
        $args += "/p:VCToolsVersion=14.44.35207"
    }

    & $msbuild @args
    if ($LASTEXITCODE -ne 0) {
        throw "pyrenderdoc_module build failed"
    }
}

function Find-RenderDocModulePaths {
    if ($env:RENDERDOC_MODULE_PATH -and (Test-Path (Join-Path $env:RENDERDOC_MODULE_PATH "renderdoc.pyd"))) {
        $pydDir = $env:RENDERDOC_MODULE_PATH
        $dllDir = if ($env:RENDERDOC_DLL_PATH) { $env:RENDERDOC_DLL_PATH } else { Split-Path $pydDir -Parent }
        return @{ PydDir = $pydDir; DllDir = $dllDir }
    }

    $pydCandidates = @(
        $RenderDocPyModules,
        $RenderDocDev,
        "C:\Program Files\RenderDoc",
        "C:\Program Files (x86)\RenderDoc"
    )

    foreach ($pydDir in $pydCandidates) {
        if (Test-Path (Join-Path $pydDir "renderdoc.pyd")) {
            $dllDir = if ($pydDir -like "*\pymodules") { Split-Path $pydDir -Parent } else { $pydDir }
            return @{ PydDir = $pydDir; DllDir = $dllDir }
        }
    }

    return $null
}

function Test-RenderDocImport {
    param([string]$PydDir, [string]$DllDir)

    $pydForward = ($PydDir -replace '\\', '/')
    $dllForward = ($DllDir -replace '\\', '/')
    $code = @"
import os, sys
sys.path.insert(0, r'$PydDir')
if hasattr(os, 'add_dll_directory'):
    os.add_dll_directory(r'$PydDir')
    os.add_dll_directory(r'$DllDir')
import renderdoc
print(renderdoc.GetVersionString())
"@
    $out = python -c $code 2>&1
    return ($LASTEXITCODE -eq 0), $out
}

Write-Host "Checking Python..."
$pyVersion = python --version 2>&1
Write-Host "  $pyVersion"
if ($LASTEXITCODE -ne 0) {
    throw "python not found on PATH"
}

if (-not (Test-Path $McpRepo)) {
    Write-Host "Cloning Linkingooo/renderdoc-mcp into tools/renderdoc-mcp ..."
    git clone --depth 1 https://github.com/Linkingooo/renderdoc-mcp.git $McpRepo
}
else {
    Write-Host "Updating tools/renderdoc-mcp ..."
    Push-Location $McpRepo
    git pull --ff-only
    Pop-Location
}

Write-Host "Installing renderdoc-mcp (editable) ..."
Push-Location $McpRepo
python -m pip install -e .
Pop-Location

$paths = Find-RenderDocModulePaths
if (-not $paths -or $RebuildPythonModule) {
    if (-not (Test-Path (Join-Path $RenderDocRoot "renderdoc.sln"))) {
        Write-Warning "Vendored RenderDoc not found; install official RenderDoc or init submodule."
        exit 1
    }
    $pythonPrefix = Get-PythonPrefix
    if (-not $pythonPrefix) {
        throw "Could not resolve python prefix"
    }
    Build-RenderDocPythonModule $pythonPrefix
    $paths = Find-RenderDocModulePaths
}

if (-not $paths) {
    Write-Warning @"
renderdoc.pyd not found.

Build vendored RenderDoc (tools\build_renderdoc.bat) or install official RenderDoc,
then re-run this script. Typical paths:
  tools\renderdoc\x64\Development\pymodules\renderdoc.pyd   (vendored source build)
  C:\Program Files\RenderDoc\renderdoc.pyd                  (official installer)

You can also set RENDERDOC_MODULE_PATH to the directory containing renderdoc.pyd.
To force a rebuild against your active Python: -RebuildPythonModule
"@
    exit 1
}

$ok, $importOut = Test-RenderDocImport $paths.PydDir $paths.DllDir
if (-not $ok) {
    Write-Host "renderdoc.pyd found but import failed; rebuilding for active Python ..."
    Write-Host "  $importOut"
    $pythonPrefix = Get-PythonPrefix
    Build-RenderDocPythonModule $pythonPrefix
    $paths = Find-RenderDocModulePaths
    $ok, $importOut = Test-RenderDocImport $paths.PydDir $paths.DllDir
    if (-not $ok) {
        throw "renderdoc import still failing after rebuild:`n$importOut"
    }
}

Write-Host "renderdoc import OK: $importOut"

$pydForward = ($paths.PydDir -replace '\\', '/')
$dllForward = ($paths.DllDir -replace '\\', '/')
Write-Host "Using RENDERDOC_MODULE_PATH=$pydForward"
Write-Host "Using RENDERDOC_DLL_PATH=$dllForward"

New-Item -ItemType Directory -Force -Path (Split-Path $CursorMcp) | Out-Null
$mcpJson = @{
    mcpServers = @{
        renderdoc = @{
            command = "python"
            args = @("-m", "renderdoc_mcp")
            env = @{
                RENDERDOC_MODULE_PATH = $pydForward
                RENDERDOC_DLL_PATH = $dllForward
            }
        }
    }
} | ConvertTo-Json -Depth 6
Set-Content -Path $CursorMcp -Value $mcpJson -Encoding UTF8

Write-Host "Wrote $CursorMcp"
Write-Host "Restart Cursor (or reload MCP servers) to enable the renderdoc MCP."
Write-Host "After Ctrl+Shift+F12 capture, check Console for: RenderDoc: capture saved to ..."
