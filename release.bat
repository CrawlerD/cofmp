@echo off
rem Publica uma versao nova no GitHub. Uso: release.bat 1.2 "o que mudou"
rem Compila tudo com a versao embutida, monta o zip e o SHA256SUMS (assinado se houver chave),
rem faz commit/push e cria a release v1.2.
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

rem o zip e para quem baixa a primeira vez; so entram os arquivos que existem em dist
set ZIPLIST=%TEMP%\cofmp_zip_%RANDOM%.txt
del "%ZIPLIST%" 2>nul
for %%f in (%FILES%) do if exist "%D%\%%f" echo %%f>>"%ZIPLIST%"
tar -a -c -f "%D%\CoF_Coop_Mod.zip" -C "%D%" -T "%ZIPLIST%" || exit /b 1
del "%ZIPLIST%" 2>nul

rem manifesto com o hash dos arquivos soltos e a assinatura que o instalador exige
py "%HERE%tools\make_manifest.py" "%D%" %FILES% || exit /b 1
del "%D%\SHA256SUMS.sig" 2>nul
set KEY=%COFMP_UPDATE_KEY%
if "%KEY%"=="" set KEY=%HERE%tools\update_key.pem
if not exist "%KEY%" (
  echo ERRO: chave privada do updater nao encontrada ^(tools\update_key.pem ou COFMP_UPDATE_KEY^).
  echo        O instalador so aceita release com SHA256SUMS assinado; sem a chave ninguem atualiza.
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%tools\sign_release.ps1" -Key "%KEY%" -File "%D%\SHA256SUMS" || exit /b 1

cd /d "%HERE%"
git add -A || exit /b 1
git commit -q -m "v%VER%" && echo commit v%VER%
git push -q || exit /b 1

set ASSETS="%D%\CoF_Coop_Mod.zip"
for %%f in (%FILES%) do if exist "%D%\%%f" call set ASSETS=%%ASSETS%% "%D%\%%f"
if exist "%D%\SHA256SUMS" call set ASSETS=%%ASSETS%% "%D%\SHA256SUMS"
if exist "%D%\SHA256SUMS.sig" call set ASSETS=%%ASSETS%% "%D%\SHA256SUMS.sig"
if "%NOTES%"=="" (
  %GH% release create v%VER% %ASSETS% --title "v%VER%" --generate-notes || exit /b 1
) else (
  %GH% release create v%VER% %ASSETS% --title "v%VER%" --notes "%NOTES%" || exit /b 1
)
echo OK: release v%VER% publicada
