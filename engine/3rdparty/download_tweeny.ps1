# PowerShell script to download tweeny library
# Tweeny is a header-only C++ tweening library

$TWEENY_VERSION = "3.2.0"
$TWEENY_URL = "https://github.com/mobius3/tweeny/archive/refs/tags/v${TWEENY_VERSION}.zip"
$TARGET_DIR = "$PSScriptRoot\tweeny"
$TEMP_ZIP = "$env:TEMP\tweeny-${TWEENY_VERSION}.zip"

Write-Host "Downloading tweeny v${TWEENY_VERSION}..." -ForegroundColor Green

# Create target directory
if (Test-Path $TARGET_DIR) {
    Write-Host "Removing existing tweeny directory..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $TARGET_DIR
}

New-Item -ItemType Directory -Path $TARGET_DIR -Force | Out-Null

# Download the zip file
try {
    Invoke-WebRequest -Uri $TWEENY_URL -OutFile $TEMP_ZIP
    Write-Host "Download completed." -ForegroundColor Green
} catch {
    Write-Host "Failed to download tweeny: $_" -ForegroundColor Red
    exit 1
}

# Extract the zip file
Write-Host "Extracting tweeny..." -ForegroundColor Green
try {
    Expand-Archive -Path $TEMP_ZIP -DestinationPath "$env:TEMP" -Force
    
    # Move the include directory to the target location
    $EXTRACTED_DIR = "$env:TEMP\tweeny-${TWEENY_VERSION}"
    if (Test-Path "$EXTRACTED_DIR\include") {
        Move-Item -Path "$EXTRACTED_DIR\include" -Destination $TARGET_DIR\include
        Write-Host "tweeny extracted successfully." -ForegroundColor Green
    } else {
        Write-Host "Error: include directory not found in extracted files." -ForegroundColor Red
        exit 1
    }
    
    # Clean up
    Remove-Item -Recurse -Force $EXTRACTED_DIR
    Remove-Item -Force $TEMP_ZIP
    
    Write-Host "tweeny has been successfully installed to: $TARGET_DIR" -ForegroundColor Green
    Write-Host "You can now build the project." -ForegroundColor Green
} catch {
    Write-Host "Failed to extract tweeny: $_" -ForegroundColor Red
    exit 1
}

