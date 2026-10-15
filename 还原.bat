@echo off
chcp 65001 >nul 2>nul
setlocal EnableExtensions
cd /d "%~dp0"

rem ---- 1) 还原 SDL2.dll ----
if exist "SDL2_orig.dll" (
  del "SDL2.dll" 2>nul
  ren "SDL2_orig.dll" "SDL2.dll" || goto fail
  echo [1/3] 已还原原版 SDL2.dll
) else (
  echo [1/3] 未找到 SDL2_orig.dll，跳过
)

rem ---- 2) 删除补丁目录 ----
if exist "zhpatch" rd /s /q "zhpatch"
echo [2/3] 已删除 zhpatch\

rem ---- 3) 还原字体 ----
if exist "assets\fonts\orig" (
  for %%F in (CrimsonText-Regular.ttf CrimsonText-SemiBold.ttf CrimsonText-Italic.ttf PublicSans-ExtraBold.ttf Inconsolata-Regular.ttf) do (
    if exist "assets\fonts\orig\%%F" copy /y "assets\fonts\orig\%%F" "assets\fonts\%%F" >nul
  )
  rd /s /q "assets\fonts\orig"
  echo [3/3] 原版字体已还原
) else (
  echo [3/3] 未找到字体备份，可在 Steam 中验证游戏完整性恢复
)

echo.
echo 还原完成。建议在 Steam 中右键游戏 - 属性 - 已安装文件 - 验证完整性。
pause
exit /b 0

:fail
echo.
echo 还原失败，请先关闭游戏再重试。
pause
exit /b 1
