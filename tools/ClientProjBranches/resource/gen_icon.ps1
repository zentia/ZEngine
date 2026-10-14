# Generates ClientProjBranches.ico (multi-size, PNG-compressed entries).
# Purpose : the tool had no icon at all -- the window class registered with no
#           hIcon, so the window, the exe and the taskbar all fell back to the
#           system default icon.
# Usage   : powershell -NoProfile -ExecutionPolicy Bypass -File gen_icon.ps1
# Output  : <this dir>\ClientProjBranches.ico
# Deps    : .NET System.Drawing (Windows PowerShell 5.1)
#
# Output path is derived from $PSScriptRoot, not hardcoded, so the script still
# works if the repo moves. (LuaMemPreview's copy hardcodes E:\Engine\ZEngine\...)
#
# Design: a rounded dark tile (22,22,25 -- the same family as LuaMemPreview's)
# carrying a branch graph: one trunk forking into two, with a commit dot at each
# of the three ends. The fork is the motif because this tool is a branch and
# stack-trace viewer.
# Sizes 16/32/48/256 so it stays legible from the taskbar to Explorer's largest
# view. Entries are PNG-compressed, which every OS this tool runs on supports.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$out = Join-Path $PSScriptRoot 'ClientProjBranches.ico'

function New-Tile([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # No smoothing: at 16px a crisp aliased bar reads far better than a blur.
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::None
    $g.Clear([System.Drawing.Color]::Transparent)

    $bg   = [System.Drawing.Color]::FromArgb(255, 22, 22, 25)
    $line = [System.Drawing.Color]::FromArgb(255, 120, 124, 132)
    # Built element by element: PS 5.1 mis-parses `@(FromArgb(...), ...)` and
    # tries to treat the call's arguments as the array.
    $dots = New-Object 'System.Drawing.Color[]' 3
    $dots[0] = [System.Drawing.Color]::FromArgb(86, 180, 233)    # right tip
    $dots[1] = [System.Drawing.Color]::FromArgb(230, 145, 70)    # left tip
    $dots[2] = [System.Drawing.Color]::FromArgb(150, 120, 220)   # trunk base

    $r = $size * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($size - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($size - $r * 2, $size - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $size - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseAllFigures()
    $bgBrush = New-Object System.Drawing.SolidBrush($bg)
    $g.FillPath($bgBrush, $path)
    $bgBrush.Dispose()

    # Rounded to whole pixels so nothing ends up with fuzzy edges.
    $margin = [float][Math]::Floor($size * 0.18)
    $t      = [float][Math]::Max(1.0, [Math]::Floor($size * 0.11))
    $dot    = [float][Math]::Max(2.0, [Math]::Floor($size * 0.17))
    $cx     = [float]($size / 2.0)
    $forkY  = [float][Math]::Floor($size * 0.50)
    $leftCx  = [float][Math]::Floor($cx - $size * 0.22)
    $rightCx = [float][Math]::Floor($cx + $size * 0.22)

    $lineBrush = New-Object System.Drawing.SolidBrush($line)

    # Trunk: below the fork, down to the bottom commit dot.
    $trunkTop    = $forkY
    $trunkBottom = [float]($size - $margin - $dot) + [float][Math]::Floor($dot * 0.5)
    $g.FillRectangle($lineBrush,
                     [float][Math]::Floor($cx - $t / 2.0), $trunkTop,
                     $t, $trunkBottom - $trunkTop)

    # Two branch stubs rising from the fork.
    $stubTop = [float]($margin + $dot) - [float][Math]::Floor($dot * 0.5)
    $g.FillRectangle($lineBrush,
                     [float][Math]::Floor($leftCx - $t / 2.0), $stubTop,
                     $t, $forkY - $stubTop + $t)
    $g.FillRectangle($lineBrush,
                     [float][Math]::Floor($rightCx - $t / 2.0), $stubTop,
                     $t, $forkY - $stubTop + $t)

    # Connector across the fork.
    $g.FillRectangle($lineBrush,
                     [float][Math]::Floor($leftCx - $t / 2.0), $forkY,
                     [float][Math]::Floor($rightCx - $leftCx + $t), $t)
    $lineBrush.Dispose()

    # Commit dots at the three ends.
    $half = [float][Math]::Floor($dot / 2.0)
    $b1 = New-Object System.Drawing.SolidBrush($dots[1])
    $g.FillRectangle($b1, [float][Math]::Floor($leftCx - $half), $margin, $dot, $dot)
    $b1.Dispose()
    $b0 = New-Object System.Drawing.SolidBrush($dots[0])
    $g.FillRectangle($b0, [float][Math]::Floor($rightCx - $half), $margin, $dot, $dot)
    $b0.Dispose()
    $b2 = New-Object System.Drawing.SolidBrush($dots[2])
    $g.FillRectangle($b2, [float][Math]::Floor($cx - $half), [float]($size - $margin - $dot), $dot, $dot)
    $b2.Dispose()

    $g.Dispose()
    return $bmp
}

function Get-PngBytes([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    # Unary comma: without it PowerShell enumerates the byte[] into the
    # pipeline and the caller ends up with an object[] instead of a byte[].
    return ,$bytes
}

$sizes = @(16, 32, 48, 256)
$bitmaps = New-Object System.Collections.Generic.List[System.Drawing.Bitmap]
$pngs = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($s in $sizes) {
    $b = New-Tile $s
    $bitmaps.Add($b)
    $pngs.Add((Get-PngBytes $b))
}

$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($ms)
$w.Write([uint16]0)                  # reserved
$w.Write([uint16]1)                  # type 1 = icon
$w.Write([uint16]$sizes.Count)

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    # 256 is stored as 0 in the ICO directory.
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim)
    $w.Write([byte]$dim)
    $w.Write([byte]0)                # palette colours (0 = >256)
    $w.Write([byte]0)                # reserved
    $w.Write([uint16]1)              # colour planes
    $w.Write([uint16]32)             # bits per pixel
    $w.Write([uint32]$pngs[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Flush()

New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
foreach ($b in $bitmaps) { $b.Dispose() }
$w.Dispose()

Write-Output ("wrote {0} ({1} bytes)" -f $out, (Get-Item $out).Length)
