@echo off
rem Publica uma versao nova no GitHub. Uso: release.bat 1.2 "o que mudou"
rem Compila tudo com a versao embutida, monta o zip, faz commit/push e cria a release v1.2.
rem Quem ja tem o CoF_Coop_Mod.exe recebe a versao nova sozinho na proxima vez que abrir o instalador.
setlocal
set HERE=%~dp0
set VER=%~1
set NOTES=%~2
if "%VER%"=="" (echo uso: release.bat ^<versao^> ["o que mudou"] & exit /b 1)
set GH=gh
where gh >nul 2>nul || set GH="C:\Program Files\GitHub CLI\gh.exe"
%GH% release view v%VER% >nul 2>nul && (echo a release v%VER% ja existe & exit /b 1)

> "%HERE%VERSION" echo %VER%
call "%HERE%build.bat" || exit /b 1
call "%HERE%installer\build_installer.bat" || exit /b 1
call "%HERE%tunnel\build_tunnel.bat" || exit /b 1

set D=%HERE%dist
set FILES=CoF_Coop_Mod.exe CoF_Tunel.exe steam_api.dll steam_appid.txt COMO_JOGAR.txt ATUALIZAR.bat
del "%D%\CoF_Coop_Mod.zip" 2>nul
tar -a -c -f "%D%\CoF_Coop_Mod.zip" -C "%D%" %FILES% || exit /b 1

cd /d "%HERE%"
git add -A || exit /b 1
git commit -q -m "v%VER%" && echo commit v%VER%
git push -q || exit /b 1

rem o zip e para quem baixa a primeira vez; os arquivos soltos sao o que o instalador baixa para se atualizar
set ASSETS="%D%\CoF_Coop_Mod.zip"
for %%f in (%FILES%) do call set ASSETS=%%ASSETS%% "%D%\%%f"
if "%NOTES%"=="" (
  %GH% release create v%VER% %ASSETS% --title "v%VER%" --generate-notes || exit /b 1
) else (
  %GH% release create v%VER% %ASSETS% --title "v%VER%" --notes "%NOTES%" || exit /b 1
)
echo OK: release v%VER% publicada
