@echo off
rem Gera dist\CoF_Coop_Mod.exe com o build\cofmp.dll embutido. Rode o build.bat antes.
setlocal
set HERE=%~dp0
call "%HERE%..\tools\setup_msvc.bat" || exit /b 1
if not exist "%HERE%..\build\cofmp.dll" (echo rode o build.bat primeiro & exit /b 1)
set /p VER=<"%HERE%..\VERSION"
if not exist "%HERE%..\dist" mkdir "%HERE%..\dist"
cd /d "%HERE%"
rc /nologo /fo "%HERE%..\build\installer.res" installer.rc || exit /b 1
cl /nologo /MT /O2 /EHsc /W3 /utf-8 /Brepro /D_CRT_SECURE_NO_WARNINGS /DCOFMP_VERSION=\"%VER%\" installer.cpp "%HERE%..\build\installer.res" ^
  /Fo"%HERE%..\build\\" /Fe"%HERE%..\dist\CoF_Coop_Mod.exe" ^
  /link advapi32.lib bcrypt.lib crypt32.lib shell32.lib user32.lib winhttp.lib /MANIFESTUAC:"level='asInvoker' uiAccess='false'" || exit /b 1
echo OK: %HERE%..\dist\CoF_Coop_Mod.exe (v%VER%)
