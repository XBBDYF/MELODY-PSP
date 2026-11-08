# Run one A/B variant of the MV frame-capture harness in PPSSPP.
#
#   powershell -NoProfile -File tools/mvtest/run_variant.ps1 -Variant A
#   ... -Variant B -Start 400 -Count 60 -Step 1     # drift window
#
# Deploys tools/mvtest/build/<Variant>/EBOOT.PBP to the PPSSPP memstick
# (C:\Users\...\Documents\PPSSPP\PSP\GAME\MVTest), writes capcfg.txt, waits for
# the harness to write result.txt, kills PPSSPP and copies the captured frames
# into tools/mvtest/ab/<Variant>/. Never touches the real memory stick (H:).
param(
    [Parameter(Mandatory = $true)][string]$Variant,
    [int]$Start = 400,
    [int]$Count = 1,
    [int]$Step = 1,
    [int]$TimeoutSec = 300,
    [string]$Tag = ''
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$ms = 'C:\Users\Administrator\Documents\PPSSPP\PSP\GAME\MVTest'
$ppsspp = 'E:\PPSSPP-TLS\PPSSPPWindows.exe'
$build = Join-Path $here "build\$Variant\EBOOT.PBP"
if (-not (Test-Path -LiteralPath $build)) { throw "missing build: $build" }
if (-not $Tag) { $Tag = $Variant }

$out = Join-Path $here "ab\$Tag"
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path $ms | Out-Null

function Stop-Ppsspp {
    foreach ($name in 'PPSSPPWindows', 'PPSSPPWindows64') {
        Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}

Stop-Ppsspp
foreach ($f in 'result.txt', 'cap.txt', 'capcfg.txt') {
    Remove-Item -LiteralPath (Join-Path $ms $f) -Force -ErrorAction SilentlyContinue
}
Get-ChildItem -LiteralPath $ms -Filter 'frame*.ppm' -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -LiteralPath $out -File -ErrorAction SilentlyContinue | Remove-Item -Force

Set-Content -LiteralPath (Join-Path $ms 'capcfg.txt') -Value "$Start $Count $Step" -Encoding ascii
Copy-Item -LiteralPath $build -Destination (Join-Path $ms 'EBOOT.PBP') -Force

$resultPath = Join-Path $ms 'result.txt'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $ppsspp -ArgumentList "`"$(Join-Path $ms 'EBOOT.PBP')`"" -PassThru
$ok = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $resultPath) {
        # 文件写完再读：等它大小稳定
        $size = (Get-Item -LiteralPath $resultPath).Length
        Start-Sleep -Milliseconds 400
        if ((Get-Item -LiteralPath $resultPath).Length -eq $size -and $size -gt 0) { $ok = $true; break }
    }
    if ($proc.HasExited -and -not (Test-Path -LiteralPath $resultPath)) { Start-Sleep -Seconds 1; break }
}
Stop-Ppsspp
$sw.Stop()

if (-not $ok) { Write-Output "RUN_TIMEOUT ${Tag} after $([int]$sw.Elapsed.TotalSeconds)s" }

foreach ($f in 'result.txt', 'cap.txt') {
    $src = Join-Path $ms $f
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $out $f) -Force }
}
Get-ChildItem -LiteralPath $ms -Filter 'frame*.ppm' -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $out $_.Name) -Force }

Write-Output "RUN_DONE ${Tag} $([int]$sw.Elapsed.TotalSeconds)s"
Get-Content -LiteralPath (Join-Path $out 'result.txt') -ErrorAction SilentlyContinue
Get-Content -LiteralPath (Join-Path $out 'cap.txt') -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $out -Filter 'frame*.ppm' | Measure-Object | ForEach-Object { Write-Output ("FRAMES " + $_.Count) }
