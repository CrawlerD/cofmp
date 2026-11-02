@echo off
rem Gera dist\CoF_Tunel.exe e copia o steam_api.dll do jogo para o lado dele.
setlocal
set HERE=%~dp0
set GAME=C:\Program Files (x86)\Steam\steamapps\common\Cry of Fear
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist "%HERE%..\dist" mkdir "%HERE%..\dist"
if not exist "%HERE%..\build" mkdir "%HERE%..\build"
cd /d "%HERE%"
cl /nologo /MT /O2 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS tunnel.cpp ^
  /Fo"%HERE%..\build\\" /Fe"%HERE%..\dist\CoF_Tunel.exe" ^
  /link /MANIFESTUAC:"level='asInvoker' uiAccess='false'" || exit /b 1
copy /y "%GAME%\steam_api.dll" "%HERE%..\dist\" >nul || exit /b 1
echo OK: %HERE%..\dist\CoF_Tunel.exe
