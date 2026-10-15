@echo off
setlocal
rem Visual Studio vcvars64.bat path - override with the VCVARS env var if different
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || (echo [ERROR] vcvars64.bat not found: %VCVARS% & exit /b 1)
cd /d "%~dp0"
if not exist src\forward.cpp (
  echo [ERROR] src\forward.cpp not found.
  echo         Generate it first: python tools\gen_def.py --game-dir ^<game dir^>
  exit /b 1
)
if not exist build mkdir build
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /EHsc /MT /c src\dllmain.cpp /Fobuild\dllmain.obj || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /c src\forward.cpp /Fobuild\forward.obj || exit /b 1
link /nologo /DLL /OPT:REF /OUT:out\SDL2.dll build\dllmain.obj build\forward.obj || exit /b 1
echo BUILD OK: out\SDL2.dll
