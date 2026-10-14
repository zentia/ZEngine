# Generates the LuaMemPreview --dump-ui fixture.
# Purpose : produce a deterministic `_lua.csv` in the exporter's schema so the
#           headless UI assertions can run without a real profiler snapshot.
# Usage   : powershell -NoProfile -ExecutionPolicy Bypass -File gen_fixture.ps1
# Output  : <tool>/TestData/LuaProfile_fixture_lua.csv
# Deps    : none
$ErrorActionPreference = 'Stop'

# Bucket names must match ltracker.h exactly; an unknown name would trip the
# "every bucket documented" assertion.
$types = [ordered]@{
    'proto'        = 47448000
    'table'        = 21200000
    'string'       = 14300000
    'thread'       =  9100000
    'lua_closure'  =  4600000
    'c_closure'    =  2100000
    'string_table' =  1570000
    'upval'        =   820000
    'userdata'     =   410000
    'vm_other'     =        0
}

$rowsPerType = 13
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('Count,Size,Type,Stack')

$total = 0L
$totalIdx = 0
foreach ($t in $types.Keys) {
    $budget = [int64]$types[$t]
    # vm_other is the reconciliation residual; zero is the healthy state and the
    # fixture has to reproduce it, so it gets one empty row rather than a split.
    if ($budget -eq 0) {
        $lines.Add(('0,0,{0},"[vm internal] no residual"' -f $t))
        $totalIdx = $total
        continue
    }
    for ($i = 0; $i -lt $rowsPerType; $i++) {
        # Geometric-ish split: a few large rows then a long tail, which is what
        # the real snapshot looks like and what the sort/virtualisation see.
        $share = [int64][math]::Floor($budget / [math]::Pow(1.7, $i) / 2.4)
        if ($i -eq $rowsPerType - 1) { $share = $budget - ($total - $totalIdx) }
        if ($share -lt 16) { $share = 16 }
        $count = [int][math]::Max(1, [math]::Floor($share / 96))
        $stack = "[logic/battle/skill_$t.lua:$(100 + $i * 7)] Apply()`n" +
                 "[framework/mvc/dispatch.lua:$(40 + $i)] Send()`n" +
                 "[main.lua:1] boot()"
        $lines.Add(('{0},{1},{2},"{3}"' -f $count, $share, $t, $stack))
        $total += $share
    }
    $totalIdx = $total
}

# The declared total must equal the sum of the rows, otherwise the status bar
# raises its [!] reconciliation warning.
$lines.Add((',{0},TOTAL,' -f $total))

$outDir = Join-Path $PSScriptRoot 'TestData'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir 'LuaProfile_fixture_lua.csv'
# No BOM: the parser accepts one, but the real exporter does not write one.
[System.IO.File]::WriteAllText($outFile, ($lines -join "`r`n") + "`r`n",
                               (New-Object System.Text.UTF8Encoding $false))
Write-Output ("wrote {0} ({1} rows, total {2} bytes)" -f $outFile, ($lines.Count - 2), $total)
