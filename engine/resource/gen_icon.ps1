# =============================================================================
# gen_icon.ps1 -- the single generator for every ZEngine GUI tool's app icon.
#
# Four tools each used to carry their own copy of this script. The copies were
# identical except for the motif they drew, which is exactly the split kept
# here: one rounded tile + one ICO writer, four motif functions.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File gen_icon.ps1 `
#       -Name <ToolName> -Motif <bars|branch|check|checker> -OutDir <dir>
#
# The four tools (run from this directory):
# All four GUI tools live in engine/tools/ (AGENTS.md section 8), so every
# -OutDir is one level up from here.
#   .\gen_icon.ps1 -Name ClientProjBranches    -Motif branch  -OutDir ..\tools\ClientProjBranches\resource
#   .\gen_icon.ps1 -Name LuaMemPreview         -Motif bars    -OutDir ..\tools\LuaMemPreview\resource
#   .\gen_icon.ps1 -Name TexPreview            -Motif checker -OutDir ..\tools\TexPreview\resource
#   .\gen_icon.ps1 -Name ZEnvironmentCollector -Motif check   -OutDir ..\tools\ZEnvironmentCollector\resource
#   .\gen_icon.ps1 -Name ZInsights             -Motif flame   -OutDir ..\Source\Insights\resource
#
# -OutDir is where the tool's .ico is committed; it must be the same directory
# z_embed_app_icon() reads (resource/ next to the target's CMakeLists by default).
# No path in this script is absolute, so it survives the repo moving.
#
# Motifs:
#   bars    -- three ascending bars; the type-bar memory distribution (LuaMemPreview)
#   branch  -- a trunk forking in two with a commit dot at each end (ClientProjBranches)
#   checker -- a 4x4 checkerboard; a block-compressed texture (TexPreview)
#   check   -- a tick; a passing environment diagnosis (ZEnvironmentCollector)
#   flame   -- four stacked rows narrowing downwards; the flame chart (ZInsights)
#
# Output: <OutDir>/<Name>.ico, multi-size (16/32/48/256) with PNG-compressed
# entries, so it stays legible from the taskbar to Explorer's largest view.
# Deps: .NET System.Drawing (Windows PowerShell 5.1).
# =============================================================================

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [Parameter(Mandatory = $true)]
    [ValidateSet('bars', 'branch', 'check', 'checker', 'flame')]
    [string]$Motif,

    [string]$OutDir = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# --- shared palette ---------------------------------------------------------
# One tile colour and one accent family for all four tools, so they read as a
# set; the motif and its accent are what tell them apart in the taskbar.
$TileBg = [System.Drawing.Color]::FromArgb(255, 22, 22, 25)
# Built element by element: PS 5.1 mis-parses `@(FromArgb(...), ...)` and tries
# to treat the call's arguments as the array.
$Accent = New-Object 'System.Drawing.Color[]' 3
$Accent[0] = [System.Drawing.Color]::FromArgb(86, 180, 233)     # blue
$Accent[1] = [System.Drawing.Color]::FromArgb(230, 145, 70)     # orange
$Accent[2] = [System.Drawing.Color]::FromArgb(150, 120, 220)    # violet
$CheckGreen = [System.Drawing.Color]::FromArgb(110, 200, 130)   # check motif only
$CheckerLight = [System.Drawing.Color]::FromArgb(150, 154, 162) # checker motif only

# --- shared tile ------------------------------------------------------------
function New-Tile([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # No smoothing: at 16px a crisp aliased shape reads far better than a blur.
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::None
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = $size * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($size - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($size - $r * 2, $size - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $size - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseAllFigures()
    $brush = New-Object System.Drawing.SolidBrush($TileBg)
    $g.FillPath($brush, $path)
    $brush.Dispose()

    return $bmp
}

# --- motifs -----------------------------------------------------------------
# Three ascending bars, bottom-aligned: the type-bar memory distribution.
function Draw-Bars([System.Drawing.Graphics]$g, [int]$size) {
    $margin = $size * 0.20
    $gap = $size * 0.07
    $barW = ($size - 2 * $margin - 2 * $gap) / 3.0
    $base = $size - $margin
    # Element by element for the same PS 5.1 array-literal reason as above.
    $heights = New-Object 'double[]' 3
    $heights[0] = $size * 0.26
    $heights[1] = $size * 0.42
    $heights[2] = $size * 0.60
    for ($i = 0; $i -lt 3; $i++) {
        $x = $margin + $i * ($barW + $gap)
        $y = $base - $heights[$i]
        # Rounded to whole pixels so bars never end up with fuzzy edges.
        $b = New-Object System.Drawing.SolidBrush($Accent[$i])
        $g.FillRectangle($b,
                         [float][Math]::Floor($x), [float][Math]::Floor($y),
                         [float][Math]::Max(1.0, [Math]::Floor($barW)),
                         [float][Math]::Max(1.0, [Math]::Floor($heights[$i])))
        $b.Dispose()
    }
}

# A trunk forking into two, with a commit dot at each of the three ends.
function Draw-Branch([System.Drawing.Graphics]$g, [int]$size) {
    $line = [System.Drawing.Color]::FromArgb(255, 120, 124, 132)
    # $dots[0] right tip, [1] left tip, [2] trunk base.
    $dots = New-Object 'System.Drawing.Color[]' 3
    $dots[0] = $Accent[0]
    $dots[1] = $Accent[1]
    $dots[2] = $Accent[2]

    $margin = [float][Math]::Floor($size * 0.18)
    $t      = [float][Math]::Max(1.0, [Math]::Floor($size * 0.11))
    $dot    = [float][Math]::Max(2.0, [Math]::Floor($size * 0.17))
    $cx     = [float]($size / 2.0)
    $forkY  = [float][Math]::Floor($size * 0.50)
    $leftCx  = [float][Math]::Floor($cx - $size * 0.22)
    $rightCx = [float][Math]::Floor($cx + $size * 0.22)

    $lineBrush = New-Object System.Drawing.SolidBrush($line)

    # Trunk: below the fork, down to the bottom commit dot.
    $trunkBottom = [float]($size - $margin - $dot) + [float][Math]::Floor($dot * 0.5)
    $g.FillRectangle($lineBrush,
                     [float][Math]::Floor($cx - $t / 2.0), $forkY,
                     $t, $trunkBottom - $forkY)

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
}

# A tick: a passing environment diagnosis.
function Draw-Check([System.Drawing.Graphics]$g, [int]$size) {
    $t = [float][Math]::Max(2.0, [Math]::Floor($size * 0.16))
    # Short stroke down, then the long stroke back up. Both go into one path so
    # the corner gets a miter join instead of the notch two separate lines
    # leave, and one wide pen keeps the thickness constant on both strokes.
    $p1x = [float][Math]::Round($size * 0.24)
    $p1y = [float][Math]::Round($size * 0.52)
    $p2x = [float][Math]::Round($size * 0.44)
    $p2y = [float][Math]::Round($size * 0.72)
    $p3x = [float][Math]::Round($size * 0.78)
    $p3y = [float][Math]::Round($size * 0.30)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddLine($p1x, $p1y, $p2x, $p2y)
    $path.AddLine($p2x, $p2y, $p3x, $p3y)

    # NB: `New-Object System.Drawing.PointF($x, $y - $h)` is NOT usable here --
    # PS 5.1 parses the parenthesised form as `-ArgumentList ($x, $y) - $h` and
    # fails with "Object[] does not contain a method named op_Subtraction".
    $pen = New-Object System.Drawing.Pen($CheckGreen, $t)
    $pen.LineJoin   = [System.Drawing.Drawing2D.LineJoin]::Miter
    $pen.MiterLimit = 4.0
    $pen.StartCap   = [System.Drawing.Drawing2D.LineCap]::Square
    $pen.EndCap     = [System.Drawing.Drawing2D.LineCap]::Square
    $g.DrawPath($pen, $path)
    $pen.Dispose()
    $path.Dispose()
}

# A 4x4 checkerboard: a block-compressed texture.
function Draw-Checker([System.Drawing.Graphics]$g, [int]$size) {
    $margin = [float][Math]::Floor($size * 0.18)
    $cell   = ($size - 2 * $margin) / 4.0
    for ($r = 0; $r -lt 4; $r++) {
        for ($c = 0; $c -lt 4; $c++) {
            $x0 = [float][Math]::Floor($margin + $c * $cell)
            $y0 = [float][Math]::Floor($margin + $r * $cell)
            $x1 = [float][Math]::Floor($margin + ($c + 1) * $cell)
            $y1 = [float][Math]::Floor($margin + ($r + 1) * $cell)
            $w  = [float][Math]::Max(1.0, $x1 - $x0)
            $h  = [float][Math]::Max(1.0, $y1 - $y0)
            $colour = if ((($r + $c) % 2) -eq 0) { $CheckerLight } else { $Accent[0] }
            $b = New-Object System.Drawing.SolidBrush($colour)
            $g.FillRectangle($b, $x0, $y0, $w, $h)
            $b.Dispose()
        }
    }
}

# Four stacked rows, each narrower than the one above: the call-stack flame
# chart a .ztrace capture is drawn as.
function Draw-Flame([System.Drawing.Graphics]$g, [int]$size) {
    $margin = [float][Math]::Floor($size * 0.18)
    $rowH   = [float][Math]::Max(1.0, [Math]::Floor($size * 0.13))
    $gap    = [float][Math]::Max(1.0, [Math]::Floor($size * 0.035))

    # Element by element for the same PS 5.1 array-literal reason as above.
    $widths = New-Object 'double[]' 4
    $widths[0] = $size * 0.58
    $widths[1] = $size * 0.46
    $widths[2] = $size * 0.34
    $widths[3] = $size * 0.22
    # The top row is the widest, so it carries the blue the other motifs lead
    # with; the two middle rows step through the shared accents.
    $rows = New-Object 'System.Drawing.Color[]' 4
    $rows[0] = $Accent[0]
    $rows[1] = $Accent[1]
    $rows[2] = $Accent[2]
    $rows[3] = $Accent[0]

    for ($i = 0; $i -lt 4; $i++) {
        $y = [float][Math]::Floor($margin + $i * ($rowH + $gap))
        $w = [float][Math]::Max(1.0, [Math]::Floor($widths[$i]))
        $b = New-Object System.Drawing.SolidBrush($rows[$i])
        $g.FillRectangle($b, [float][Math]::Floor($margin), $y, $w, $rowH)
        $b.Dispose()
    }
}

# --- ICO writer -------------------------------------------------------------
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
$pngs = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($s in $sizes) {
    $bmp = New-Tile $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::None
    switch ($Motif) {
        'bars'    { Draw-Bars    $g $s }
        'branch'  { Draw-Branch  $g $s }
        'check'   { Draw-Check   $g $s }
        'checker' { Draw-Checker $g $s }
        'flame'   { Draw-Flame   $g $s }
    }
    $g.Dispose()
    $pngs.Add((Get-PngBytes $bmp))
    $bmp.Dispose()
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

$out = Join-Path $OutDir "$Name.ico"
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
$w.Dispose()

Write-Output ("wrote {0} ({1} bytes, motif '{2}')" -f $out, (Get-Item $out).Length, $Motif)
