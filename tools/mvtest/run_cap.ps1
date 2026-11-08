# Fast capture runner: deploy one variant, wait only until cap.txt records the
# capture, then kill PPSSPP. Much faster than waiting for the sample budget.
#
#   powershell -NoProfile -File tools/mvtest/run_cap.ps1 -Variant A -Start 420
param(
    [Parameter(Mandatory = $true)][string]$Variant,
    [int]$Start = 420,
    [int]$Count = 1,
    [int]$Step = 1,
    [int]$TargetMs = 0,
    [string]$Tag = '',
    [int]$TimeoutSec = 200
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$ms = 'C:\Users\Administrator\Documents\PPSSPP\PSP\GAME\MVTest'
$ppsspp = 'E:\PPSSPP-TLS\PPSSPPWindows.exe'
$build = Join-Path $here "build\$Variant\EBOOT.PBP"
if (-not (Test-Path -LiteralPath $build)) { throw "missing build: $build" }

if (-not $Tag) { $Tag = "$Variant-$Start" }
$tag = $Tag
$out = Join-Path $here "ab\$tag"
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path $ms | Out-Null

foreach ($name in 'PPSSPPWindows', 'PPSSPPWindows64') {
    Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 600

foreach ($f in 'result.txt', 'cap.txt', 'capcfg.txt') {
    Remove-Item -LiteralPath (Join-Path $ms $f) -Force -ErrorAction SilentlyContinue
}
Get-ChildItem -LiteralPath $ms -Filter 'frame*.ppm' -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -LiteralPath $out -File -ErrorAction SilentlyContinue | Remove-Item -Force

Set-Content -LiteralPath (Join-Path $ms 'capcfg.txt') -Value "$Start $Count $Step $TargetMs" -Encoding ascii
Copy-Item -LiteralPath $build -Destination (Join-Path $ms 'EBOOT.PBP') -Force
$srcHash = (Get-FileHash -LiteralPath $build).Hash
$dstHash = (Get-FileHash -LiteralPath (Join-Path $ms 'EBOOT.PBP')).Hash
if ($srcHash -ne $dstHash) { throw "deploy hash mismatch" }
Write-Output "DEPLOY ${Variant} $srcHash"

$capPath = Join-Path $ms 'cap.txt'
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $ppsspp -ArgumentList "`"$(Join-Path $ms 'EBOOT.PBP')`"" -PassThru
$got = $false
while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
    Start-Sleep -Milliseconds 400
    if (Test-Path -LiteralPath $capPath) {
        $txt = Get-Content -LiteralPath $capPath -Raw -ErrorAction SilentlyContinue
        if ($txt) {
            $n = ([regex]::Matches($txt, 'cap idx=')).Count
            if ($n -ge $Count) { $got = $true; break }
        }
    }
}
Start-Sleep -Milliseconds 400
foreach ($name in 'PPSSPPWindows', 'PPSSPPWindows64') {
    Get-Process -Name $name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
$sw.Stop()
Write-Output ("CAPTURE {0} {1} {2}s" -f $Variant, ($(if ($got) { 'OK' } else { 'TIMEOUT' })), [int]$sw.Elapsed.TotalSeconds)

foreach ($f in 'result.txt', 'cap.txt') {
    $src = Join-Path $ms $f
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $out $f) -Force }
}
Get-ChildItem -LiteralPath $ms -Filter 'frame*.ppm' -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $out $_.Name) -Force }

Get-Content -LiteralPath (Join-Path $out 'cap.txt') -ErrorAction SilentlyContinue
Get-Content -LiteralPath (Join-Path $out 'result.txt') -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $out -Filter 'frame*.ppm' | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
