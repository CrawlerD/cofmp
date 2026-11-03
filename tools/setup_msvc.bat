@echo off
rem Prepara o ambiente x86 do MSVC para os builds. Em CI (msvc-dev-cmd arch: x86) ja esta pronto.
rem Uso: call tools\setup_msvc.bat
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 0
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  where cl >nul 2>nul && exit /b 0
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if defined VS (
  call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul && exit /b 0
)

rem caminho conhecido das maquinas de desenvolvimento
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
