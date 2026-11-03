@echo off
rem Compila cofmp.dll (x86). Uso: build.bat [caminho do hl.dll]
rem Sem hl.dll, usa tools\hl_exports.txt (a CI nao tem o jogo instalado). HL_SDK troca o SDK.
setlocal
set HERE=%~dp0
if "%HL_SDK%"=="" (set "SDK=%HERE%..\deps\halflife") else (set "SDK=%HL_SDK%")
set "HLDLL=%~1"
if "%HLDLL%"=="" set "HLDLL=%HL_DLL%"
if "%HLDLL%"=="" set "HLDLL=C:\Program Files (x86)\Steam\steamapps\common\Cry of Fear\cryoffear\cl_dlls\hl.dll"
call "%HERE%tools\setup_msvc.bat" || exit /b 1
set "PY=py"
where py >nul 2>nul || set "PY=python"
set "PYTHONPATH=%LOCALAPPDATA%\Temp\cofre_re"
set /p VER=<"%HERE%VERSION"
if not exist "%HERE%build" mkdir "%HERE%build"
set "GEN=-"
if exist "%HLDLL%" set "GEN=%HLDLL%"
%PY% "%HERE%gen_def.py" "%GEN%" "%HERE%build" || exit /b 1
cl /nologo /utf-8 /wd4828 /LD /MT /O2 /EHsc /W3 /Brepro /DWIN32 /D_CRT_SECURE_NO_WARNINGS /DCOFMP_VERSION=\"%VER%\" ^
  /I"%HERE%build" /I"%HERE%sdk_cof" /I"%SDK%\dlls" /I"%SDK%\engine" /I"%SDK%\common" /I"%SDK%\public" /I"%SDK%\pm_shared" ^
  "%HERE%cofmp.cpp" "%HERE%hud.cpp" /Fo"%HERE%build\\" /Fe"%HERE%build\cofmp.dll" ^
  /link /DEF:"%HERE%build\cofmp.def" /MANIFEST:EMBED || exit /b 1
echo OK: %HERE%build\cofmp.dll
