# Generates LuaMemPreview.ico (multi-size, PNG-compressed entries).
# Purpose : the tool had no icon at all -- WNDCLASS set only hCursor, so both
#           the window and the exe fell back to the system default icon.
# Usage   : powershell -NoProfile -ExecutionPolicy Bypass -File gen_icon.ps1
# Output  : E:\Engine\ZEngine\engine\Source\Tools\LuaMemPreview\resource\LuaMemPreview.ico
# Deps    : .NET System.Drawing (Windows PowerShell 5.1)
#
# Design: a rounded dark tile (the viewer's own background colour 22,22,25) with
# three ascending bars -- the type-bar memory distribution the tool is about.
# Sizes 16/32/48/256 so it stays legible from the taskbar to Explorer's largest
# view. Entries are PNG-compressed, which every OS this tool runs on supports.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$out = 'E:\Engine\ZEngine\engine\Source\Tools\LuaMemPreview\resource\LuaMemPreview.ico'

function New-Tile([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # No smoothing: at 16px a crisp aliased bar reads far better than a blur.
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::None
    $g.Clear([System.Drawing.Color]::Transparent)

    $bg = [System.Drawing.Color]::FromArgb(255, 22, 22, 25)
    $bar = @(
        [System.Drawing.Color]::FromArgb(86, 180, 233),
        [System.Drawing.Color]::FromArgb(230, 145, 70),
        [System.Drawing.Color]::FromArgb(150, 120, 220)
    )

    $r = $size * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($size - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($size - $r * 2, $size - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $size - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseAllFigures()
    $g.FillPath((New-Object System.Drawing.SolidBrush($bg)), $path)

    # Three bars, bottom-aligned, ascending -- the type-bar distribution.
    $margin = $size * 0.20
    $gap = $size * 0.07
    $barW = ($size - 2 * $margin - 2 * $gap) / 3.0
    $base = $size - $margin
    # Built element by element: PS 5.1 mis-parses `@($size * 0.26, ...)` and
    # tries to multiply the array itself.
    $heights = New-Object 'double[]' 3
    $heights[0] = $size * 0.26
    $heights[1] = $size * 0.42
    $heights[2] = $size * 0.60
    for ($i = 0; $i -lt 3; $i++) {
        $x = $margin + $i * ($barW + $gap)
        $y = $base - $heights[$i]
        # Round to whole pixels so bars never end up with fuzzy edges.
        $g.FillRectangle((New-Object System.Drawing.SolidBrush($bar[$i])),
                         [float][Math]::Floor($x), [float][Math]::Floor($y),
                         [float][Math]::Max(1.0, [Math]::Floor($barW)),
                         [float][Math]::Max(1.0, [Math]::Floor($heights[$i])))
    }

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
