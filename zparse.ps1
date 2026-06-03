# ZEngine ZParser Execution Script
# This script runs the ZParser tool for meta data parsing and code generation

param(
    [string]$ProjectFile = "",
    [string]$SourceIncludeFile = "",
    [string]$IncludePath = "",
    [string]$SysInclude = "*",
    [string]$ModuleName = "Z",
    [int]$ShowErrors = 0
)

# Get script directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$EngineDir = Join-Path $ScriptDir "engine"
$ParserExe = Join-Path $EngineDir "bin\ZParser.exe"

# Default values if not provided
if ([string]::IsNullOrEmpty($ProjectFile)) {
    $ProjectFile = Join-Path $EngineDir "bin\precompile.json"
}

if ([string]::IsNullOrEmpty($SourceIncludeFile)) {
    $BuildDir = Join-Path $ScriptDir "build"
    if (-not (Test-Path $BuildDir)) {
        $BuildDir = Join-Path $ScriptDir "cmake-build"
    }
    $SourceIncludeFile = Join-Path $BuildDir "parser_header.h"
}

if ([string]::IsNullOrEmpty($IncludePath)) {
    $IncludePath = Join-Path $EngineDir "source"
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "ZEngine ZParser Execution" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Check if ZParser exists
if (-not (Test-Path $ParserExe)) {
    Write-Host "[ERROR] ZParser.exe not found at: $ParserExe" -ForegroundColor Red
    Write-Host "[INFO] Please build the project first to generate ZParser.exe" -ForegroundColor Yellow
    exit 1
}

# Check if project file exists
if (-not (Test-Path $ProjectFile)) {
    Write-Host "[WARNING] Project file not found at: $ProjectFile" -ForegroundColor Yellow
    Write-Host "[INFO] Attempting to continue anyway..." -ForegroundColor Yellow
}

# Check if include path exists
if (-not (Test-Path $IncludePath)) {
    Write-Host "[WARNING] Include path not found: $IncludePath" -ForegroundColor Yellow
}

Write-Host "[INFO] Project File:      $ProjectFile" -ForegroundColor Green
Write-Host "[INFO] Source Include:   $SourceIncludeFile" -ForegroundColor Green
Write-Host "[INFO] Include Path:     $IncludePath" -ForegroundColor Green
Write-Host "[INFO] System Include:   $SysInclude" -ForegroundColor Green
Write-Host "[INFO] Module Name:      $ModuleName" -ForegroundColor Green
Write-Host "[INFO] Show Errors:      $ShowErrors" -ForegroundColor Green
Write-Host ""

# Clean up _generated directory before execution
$GeneratedDir = Join-Path $IncludePath "_generated"
if (Test-Path $GeneratedDir) {
    Write-Host "[INFO] Cleaning up _generated directory: $GeneratedDir" -ForegroundColor Yellow
    try {
        Remove-Item -Path $GeneratedDir -Recurse -Force
        Write-Host "[INFO] _generated directory removed successfully" -ForegroundColor Green
    } catch {
        Write-Host "[WARNING] Failed to remove _generated directory: $_" -ForegroundColor Yellow
        Write-Host "[INFO] Attempting to continue anyway..." -ForegroundColor Yellow
    }
} else {
    Write-Host "[INFO] _generated directory does not exist, skipping cleanup" -ForegroundColor Green
}
Write-Host ""

# Execute ZParser
Write-Host "[INFO] Executing ZParser..." -ForegroundColor Cyan
Write-Host ""

$startTime = Get-Date

try {
    & $ParserExe $ProjectFile $SourceIncludeFile $IncludePath $SysInclude $ModuleName $ShowErrors
    
    if ($LASTEXITCODE -eq 0) {
        $duration = (Get-Date) - $startTime
        Write-Host ""
        Write-Host "[SUCCESS] ZParser completed successfully!" -ForegroundColor Green
        Write-Host "[INFO] Execution time: $($duration.TotalMilliseconds) ms" -ForegroundColor Green
        exit 0
    } else {
        Write-Host ""
        Write-Host "[ERROR] ZParser failed with exit code: $LASTEXITCODE" -ForegroundColor Red
        exit $LASTEXITCODE
    }
} catch {
    Write-Host ""
    Write-Host "[ERROR] Failed to execute ZParser: $_" -ForegroundColor Red
    exit 1
}

