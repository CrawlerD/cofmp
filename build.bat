@echo off
rem Compila cofmp.dll (x86). Uso: build.bat [caminho do hl.dll]
setlocal
set HERE=%~dp0
set SDK=%HERE%..\deps\halflife
set HLDLL=%~1
if "%HLDLL%"=="" set HLDLL=C:\Program Files (x86)\Steam\steamapps\common\Cry of Fear\cryoffear\cl_dlls\hl.dll
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
set PYTHONPATH=%LOCALAPPDATA%\Temp\cofre_re
if not exist "%HERE%build" mkdir "%HERE%build"
py "%HERE%gen_def.py" "%HLDLL%" "%HERE%build" || exit /b 1
cl /nologo /LD /MT /O2 /EHsc /W3 /DWIN32 /D_CRT_SECURE_NO_WARNINGS ^
  /I"%HERE%build" /I"%HERE%sdk_cof" /I"%SDK%\dlls" /I"%SDK%\engine" /I"%SDK%\common" /I"%SDK%\public" /I"%SDK%\pm_shared" ^
  "%HERE%cofmp.cpp" /Fo"%HERE%build\\" /Fe"%HERE%build\cofmp.dll" ^
  /link /DEF:"%HERE%build\cofmp.def" || exit /b 1
echo OK: %HERE%build\cofmp.dll
