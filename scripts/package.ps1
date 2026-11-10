<#
.SYNOPSIS
  打包发布 zip（DESIGN.md §14）：out\<Config>\MotionCalm.exe + src\res\MotionCalm.default.ini（改名 MotionCalm.ini）
  + LICENSE + docs\user-guide.{zh-CN,zh-TW,en}.md（改名 USER-GUIDE.<语言>.md：简体 / 繁體 / English 使用说明）
  → out\MotionCalm-v<Version>-win64.zip（文件放在 zip 根目录）。

.DESCRIPTION
  缺少的文件跳过并给出警告；一个文件都没有时返回 1。先运行 build.cmd all release。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\package.ps1
  powershell -ExecutionPolicy Bypass -File scripts\package.ps1 -Version 1.2.0
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z.\-]*$')]
    [string]$Version = '1.2.0',
    [ValidateSet('debug', 'release')]
    [string]$Config = 'release'
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$OutRoot = Join-Path $Root 'out'
$Name = "MotionCalm-v$Version-win64"
$Zip = Join-Path $OutRoot "$Name.zip"
$Stage = Join-Path $OutRoot "package\$Name"

$Items = @(
    @{ Src = "out\$Config\MotionCalm.exe";      Dst = 'MotionCalm.exe' },
    @{ Src = 'src\res\MotionCalm.default.ini';  Dst = 'MotionCalm.ini' },
    @{ Src = 'LICENSE';                         Dst = 'LICENSE' },
    @{ Src = 'docs\user-guide.zh-CN.md';        Dst = 'USER-GUIDE.zh-CN.md' },
    @{ Src = 'docs\user-guide.zh-TW.md';        Dst = 'USER-GUIDE.zh-TW.md' },
    @{ Src = 'docs\user-guide.en.md';           Dst = 'USER-GUIDE.en.md' }
)

if ($Config -ne 'release') {
    Write-Host "[警告] 正在打包 $Config 版本（发布请用 release）" -ForegroundColor Yellow
}

if (Test-Path $Stage) { Remove-Item -LiteralPath $Stage -Recurse -Force }
New-Item -ItemType Directory -Path $Stage -Force | Out-Null

$packed = @()
foreach ($it in $Items) {
    $src = Join-Path $Root $it.Src
    if (Test-Path -LiteralPath $src -PathType Leaf) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $Stage $it.Dst) -Force
        $packed += $it.Dst
        Write-Host ("  + {0,-20} <- {1}" -f $it.Dst, $it.Src)
    } else {
        Write-Host ("[警告] 缺少 {0}，已跳过" -f $it.Src) -ForegroundColor Yellow
    }
}

if ($packed.Count -eq 0) {
    Remove-Item -LiteralPath (Join-Path $OutRoot 'package') -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '[错误] 没有任何可打包的文件（先运行 build.cmd all release）' -ForegroundColor Red
    exit 1
}

if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $Zip -CompressionLevel Optimal
Remove-Item -LiteralPath (Join-Path $OutRoot 'package') -Recurse -Force -ErrorAction SilentlyContinue

$size = (Get-Item -LiteralPath $Zip).Length
Write-Host ("[完成] {0}  ({1:N0} 字节，{2} 个文件：{3})" -f $Zip, $size, $packed.Count, ($packed -join ', ')) -ForegroundColor Green
if ($packed.Count -lt $Items.Count) {
    Write-Host '[警告] zip 不完整，见上面的缺失文件' -ForegroundColor Yellow
}
exit 0
