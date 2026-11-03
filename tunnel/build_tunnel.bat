@echo off
rem Gera dist\CoF_Tunel.exe e copia o steam_api.dll do jogo para o lado dele (se existir).
rem COF_GAME troca a pasta do jogo; sem o steam_api.dll o build continua (a CI nao tem o jogo).
setlocal
set HERE=%~dp0
if "%COF_GAME%"=="" (set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Cry of Fear") else (set "GAME=%COF_GAME%")
call "%HERE%..\tools\setup_msvc.bat" || exit /b 1
if not exist "%HERE%..\dist" mkdir "%HERE%..\dist"
if not exist "%HERE%..\build" mkdir "%HERE%..\build"
cd /d "%HERE%"
set /p VER=<"%HERE%..\VERSION"
cl /nologo /MT /O2 /EHsc /W3 /utf-8 /Brepro /D_CRT_SECURE_NO_WARNINGS /DCOFMP_VERSION=\"%VER%\" tunnel.cpp ^
  /Fo"%HERE%..\build\\" /Fe"%HERE%..\dist\CoF_Tunel.exe" ^
  /link /MANIFESTUAC:"level='asInvoker' uiAccess='false'" || exit /b 1
if exist "%GAME%\steam_api.dll" (
  copy /y "%GAME%\steam_api.dll" "%HERE%..\dist\" >nul || exit /b 1
) else (
  echo AVISO: steam_api.dll nao encontrado em "%GAME%"; copie o arquivo para dist\ antes de publicar.
)
echo OK: %HERE%..\dist\CoF_Tunel.exe
