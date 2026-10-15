#Requires -Version 5
<#
  组装汉化补丁发布包（解压即覆盖）。

  用法：
    powershell -ExecutionPolicy Bypass -File package.ps1 -GameDir <游戏目录>

  前置：
    1. 已运行 build.bat（生成 out\SDL2.dll）
    2. -GameDir 指向**已打过补丁**的游戏目录（其中含 SDL2_orig.dll，
       即原版 SDL2 的字节）
    3. 首次运行会自动从 GitHub 下载 Noto CJK 字体到 cache\fonts\
#>
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$OutZip = "$PSScriptRoot\dist\5D国际象棋-简体中文汉化补丁.zip"
)

$ErrorActionPreference = 'Stop'
$GameDir = (Resolve-Path $GameDir).Path

# ---- 校验输入 ----
$proxy = Join-Path $PSScriptRoot 'out\SDL2.dll'
if (-not (Test-Path $proxy)) { throw "缺少 $proxy —— 请先运行 build.bat" }
$orig = Join-Path $GameDir 'SDL2_orig.dll'
if (-not (Test-Path $orig)) {
    throw "缺少 $orig`n包内需要原版 SDL2 字节（代理的转发目标）。请在已打补丁的目录打包，或先把原版 SDL2.dll 改名为 SDL2_orig.dll。"
}
$tsv = Join-Path $PSScriptRoot 'translation.tsv'
if (-not (Test-Path $tsv)) { throw "缺少 translation.tsv" }
$readme = Join-Path $PSScriptRoot '安装说明.txt'
if (-not (Test-Path $readme)) { throw "缺少 安装说明.txt" }

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

# ---- 暂存 ----
$stage = Join-Path $PSScriptRoot 'dist\payload'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$stage\zhpatch", "$stage\assets\fonts" | Out-Null

Copy-Item $proxy            "$stage\SDL2.dll"
Copy-Item $orig             "$stage\SDL2_orig.dll"
Copy-Item $tsv              "$stage\zhpatch\translation.tsv"
Copy-Item (Join-Path $PSScriptRoot 'config.example.ini') "$stage\zhpatch\config.ini"
Copy-Item $readme           "$stage\安装说明.txt"

# 字体映射（5 个槽位 ← 4 个 Noto OTF）
$map = @{
    'CrimsonText-Regular.ttf'  = 'NotoSerifCJKsc-Regular.otf'
    'CrimsonText-Italic.ttf'   = 'NotoSerifCJKsc-Regular.otf'   # CJK 无斜体，同字重
    'CrimsonText-SemiBold.ttf' = 'NotoSerifCJKsc-Bold.otf'
    'PublicSans-ExtraBold.ttf' = 'NotoSansCJKsc-Bold.otf'
    'Inconsolata-Regular.ttf'  = 'NotoSansCJKsc-Regular.otf'
}
foreach ($k in $map.Keys) {
    Copy-Item (Join-Path $fontCache $map[$k]) "$stage\assets\fonts\$k"
}

# ---- 打包 ----
New-Item -ItemType Directory -Force -Path (Split-Path $OutZip) | Out-Null
Remove-Item $OutZip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $OutZip -CompressionLevel Optimal
Write-Host "OK -> $OutZip  ($([math]::Round((Get-Item $OutZip).Length/1MB,1)) MB)"
