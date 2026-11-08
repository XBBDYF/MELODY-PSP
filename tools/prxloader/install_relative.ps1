param([string]$TargetPath,[switch]$VerifyOnly)
$ErrorActionPreference='Stop'
$Source=Join-Path $PSScriptRoot 'MELODY'
$Manifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if(-not $TargetPath){
 $found=@(Get-PSDrive -PSProvider FileSystem | Where-Object {$_.Name -ne 'C'} | ForEach-Object {
  $games=Join-Path $_.Root 'PSP\GAME'
  if(Test-Path -LiteralPath $games){Get-ChildItem -LiteralPath $games -Filter melody.prx -File -Recurse -Depth 4 -ErrorAction SilentlyContinue | ForEach-Object {$_.DirectoryName}}
 })
 if($found.Count -eq 1){$TargetPath=$found[0]}else{$TargetPath=Read-Host '输入PSP上原MELODY文件夹路径（支持分类子目录）'}
}
$TargetPath=(Resolve-Path -LiteralPath $TargetPath).Path
if(-not(Test-Path -LiteralPath (Join-Path $TargetPath 'melody.prx'))){throw '目标不是现有MELODY安装目录，请选择包含melody.prx的文件夹。'}
$Files=@()
if((Test-Path -LiteralPath (Join-Path $Source "update.txt")) -and -not(Test-Path -LiteralPath (Join-Path $TargetPath "update.txt"))){$Files+="update.txt"}
foreach($item in $Manifest.manual_files.PSObject.Properties){
 $p=Join-Path $Source $item.Name
 if((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() -ne $item.Value.sha256){throw "安装包校验失败: $($item.Name)"}
 $Files+=$item.Name
}
$Files+=@(Get-ChildItem -LiteralPath (Join-Path $Source 'skins') -File -Recurse | ForEach-Object {$_.FullName.Substring($Source.Length+1)})
if($VerifyOnly){Write-Output "已核验 $($Files.Count) 个待替换文件；目标: $TargetPath";exit 0}
$Backup=Join-Path $PSScriptRoot ('backup-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path $Backup | Out-Null
$Written=@()
try{
 foreach($rel in $Files){
  $src=Join-Path $Source $rel;$dst=Join-Path $TargetPath $rel;$old=Join-Path $Backup $rel
  $exists=Test-Path -LiteralPath $dst
  if($exists){New-Item -ItemType Directory -Force -Path (Split-Path $old) | Out-Null;Copy-Item -LiteralPath $dst -Destination $old}
  $Written+=@{Rel=$rel;Existed=$exists}
  New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
  Copy-Item -LiteralPath $src -Destination $dst -Force
  if((Get-FileHash -LiteralPath $src).Hash -ne (Get-FileHash -LiteralPath $dst).Hash){throw "写入校验失败: $rel"}
 }
 Write-Output "替换完成：$TargetPath。账号、配置、缓存、下载断点和当前主题保留。备份：$Backup"
}catch{
 foreach($entry in $Written){$dst=Join-Path $TargetPath $entry.Rel;if($entry.Existed){Copy-Item -LiteralPath (Join-Path $Backup $entry.Rel) -Destination $dst -Force}}
 throw
}
