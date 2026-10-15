#Requires -Version 5
<#
  组装汉化补丁发布包（安装脚本负责重命名原版 DLL，包内不带任何游戏文件）。

  用法：
    powershell -ExecutionPolicy Bypass -File package.ps1

  前置：
    1. 已运行 build.bat（生成 out\SDL2.dll）
    2. 首次运行会自动从 GitHub 下载 Noto CJK 字体到 cache\fonts\
#>
param(
    [string]$OutZip = "$PSScriptRoot\dist\5D国际象棋-简体中文汉化补丁.zip"
)

$ErrorActionPreference = 'Stop'

$proxy = Join-Path $PSScriptRoot 'out\SDL2.dll'
if (-not (Test-Path $proxy)) { throw "缺少 $proxy —— 请先运行 build.bat" }
$tsv = Join-Path $PSScriptRoot 'translation.tsv'
if (-not (Test-Path $tsv)) { throw "缺少 translation.tsv" }

# ---- 字体（缺失则下载，OFL） ----
$fontCache = Join-Path $PSScriptRoot 'cache\fonts'
New-Item -ItemType Directory -Force -Path $fontCache | Out-Null
$base = 'https://github.com/notofonts/noto-cjk/raw/main'
$need = @(
    @{ n = 'NotoSerifCJKsc-Regular.otf'; u = "$base/Serif/OTF/SimplifiedChinese/NotoSerifCJKsc-Regular.otf" },
    @{ n = 'NotoSerifCJKsc-Bold.otf';    u = "$base/Serif/OTF/SimplifiedChinese/NotoSerifCJKsc-Bold.otf" },
    @{ n = 'NotoSansCJKsc-Regular.otf';  u = "$base/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf" },
    @{ n = 'NotoSansCJKsc-Bold.otf';     u = "$base/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Bold.otf" }
)
foreach ($f in $need) {
    $p = Join-Path $fontCache $f.n
    if (-not (Test-Path $p) -or (Get-Item $p).Length -lt 1MB) {
        Write-Host "downloading $($f.n) ..."
        & curl.exe -sL --retry 3 -o $p $f.u
        if (-not (Test-Path $p) -or (Get-Item $p).Length -lt 1MB) { throw "download failed: $($f.u)" }
    }
}

# ---- 暂存（patch\ 子目录承载覆盖文件，安装.bat 负责重命名原版） ----
$stage = Join-Path $PSScriptRoot 'dist\payload'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$stage\patch\zhpatch", "$stage\patch\assets\fonts" | Out-Null

Copy-Item $proxy "$stage\patch\SDL2.dll"
Copy-Item $tsv   "$stage\patch\zhpatch\translation.tsv"
Copy-Item (Join-Path $PSScriptRoot 'config.example.ini') "$stage\patch\zhpatch\config.ini"
Copy-Item (Join-Path $PSScriptRoot '安装.bat') "$stage\安装.bat"
Copy-Item (Join-Path $PSScriptRoot '还原.bat') "$stage\还原.bat"
Copy-Item (Join-Path $PSScriptRoot '安装说明.txt') "$stage\安装说明.txt"

$map = @{
    'CrimsonText-Regular.ttf'  = 'NotoSerifCJKsc-Regular.otf'
    'CrimsonText-Italic.ttf'   = 'NotoSerifCJKsc-Regular.otf'
    'CrimsonText-SemiBold.ttf' = 'NotoSerifCJKsc-Bold.otf'
    'PublicSans-ExtraBold.ttf' = 'NotoSansCJKsc-Bold.otf'
    'Inconsolata-Regular.ttf'  = 'NotoSansCJKsc-Regular.otf'
}
foreach ($k in $map.Keys) {
    Copy-Item (Join-Path $fontCache $map[$k]) "$stage\patch\assets\fonts\$k"
}

# ---- 打包 ----
New-Item -ItemType Directory -Force -Path (Split-Path $OutZip) | Out-Null
Remove-Item $OutZip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $OutZip -CompressionLevel Optimal
Write-Host "OK -> $OutZip  ($([math]::Round((Get-Item $OutZip).Length/1MB,1)) MB)"
