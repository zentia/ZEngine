param(
  [int]$BootSeconds = 35,
  [int]$WaitAfterClose = 30,
  [string]$Rhi = "vulkan"
)

$ErrorActionPreference = 'Stop'
$root = "e:\Engine\ZEngine"
$exe  = Join-Path $root "bin\RelWithDebInfo\ZEditor.exe"
$proj = "E:\ZEngineDemo\ZEngineDemo.zproject"

Push-Location $root

# Make sure validation layer is on for this child process.
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"

Write-Host "[run] launching ZEditor with --rhi $Rhi ..."
$proc = Start-Process -FilePath $exe -ArgumentList @('-p', $proj, '--rhi', $Rhi) -PassThru
Write-Host "[run] PID = $($proc.Id)"

# Boot wait. We can't easily detect "done initialising", so just give it
# enough time to walk through R/P creation. The previous failed run showed
# the editor reaching the particle compute layout step at ~T+25s.
Start-Sleep -Seconds $BootSeconds
Write-Host "[run] boot wait complete; sending WM_CLOSE ..."

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
  [DllImport("user32.dll")]
  public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
  [DllImport("user32.dll", SetLastError=true)]
  [return: MarshalAs(UnmanagedType.Bool)]
  public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll", SetLastError=true)]
  public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
}
"@

$WM_CLOSE = 0x0010
$targetPid = [uint32]$proc.Id
$sent = 0
$cb = [Win32+EnumWindowsProc] {
  param($hWnd, $lParam)
  $wpid = 0
  [Win32]::GetWindowThreadProcessId($hWnd, [ref]$wpid) | Out-Null
  if ($wpid -eq $script:targetPid) {
    [Win32]::PostMessage($hWnd, $script:WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $script:sent++
  }
  return $true
}
[Win32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
Write-Host "[run] WM_CLOSE posted to $sent window(s); waiting up to ${WaitAfterClose}s for graceful exit ..."

# Wait for process to exit gracefully.
$deadline = (Get-Date).AddSeconds($WaitAfterClose)
while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 250
}

if ($proc.HasExited) {
  Write-Host "[run] editor exited gracefully, code=$($proc.ExitCode)"
} else {
  Write-Host "[run] WARNING: editor still running after $WaitAfterClose s, force killing"
  Stop-Process -Id $proc.Id -Force
  Start-Sleep -Seconds 2
}

# Find the newest log file and report its size.
$log = Get-ChildItem (Join-Path $root "bin\RelWithDebInfo\cache\Logs_*.log") |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1
if ($log) {
  Write-Host "[run] newest log: $($log.Name) size=$($log.Length)"
  Write-Host $log.FullName
}

Pop-Location
