@echo off
chcp 65001 >nul 2>nul
setlocal EnableExtensions
cd /d "%~dp0"
set "GAME_EXE=5dchesswithmultiversetimetravel.exe"

if not exist "%GAME_EXE%" (
  echo [错误] 当前目录没有 %GAME_EXE%
  echo 请先把本包的全部内容解压到游戏安装目录，再运行 安装.bat
  goto fail
)

for %%A in ("%GAME_EXE%") do (
  if not "%%~zA"=="1363456" (
    echo [警告] 游戏 exe 大小为 %%~zA，与 Steam v1.0（1363456）不符，补丁可能无效
  )
)

if not exist "patch\SDL2.dll" (
  echo [错误] 缺少 patch\SDL2.dll
  goto fail
)

rem ---- 1) 备份原版 SDL2.dll ----
if exist "SDL2_orig.dll" (
  echo [1/4] SDL2_orig.dll 已存在，跳过备份
) else (
  if exist "SDL2.dll" (
    ren "SDL2.dll" "SDL2_orig.dll" || goto fail
    echo [1/4] 已备份 SDL2.dll -^> SDL2_orig.dll
  ) else (
    echo [错误] 找不到 SDL2.dll
    goto fail
  )
)

rem ---- 2) 备份原版字体 ----
if not exist "assets\fonts\orig" mkdir "assets\fonts\orig"
for %%F in (CrimsonText-Regular.ttf CrimsonText-SemiBold.ttf CrimsonText-Italic.ttf PublicSans-ExtraBold.ttf Inconsolata-Regular.ttf) do (
  if exist "assets\fonts\%%F" if not exist "assets\fonts\orig\%%F" (
    copy /y "assets\fonts\%%F" "assets\fonts\orig\%%F" >nul
  )
)
echo [2/4] 原版字体已备份到 assets\fonts\orig\

rem ---- 3) 覆盖补丁文件 ----
xcopy "patch\*" . /E /I /Y >nul || goto fail
echo [3/4] 补丁文件已覆盖

echo [4/4] 安装完成，现在双击游戏即可运行中文版
echo.
pause
exit /b 0

:fail
echo.
echo 安装失败。若是覆盖被占用导致，请先关闭游戏再重试。
pause
exit /b 1
