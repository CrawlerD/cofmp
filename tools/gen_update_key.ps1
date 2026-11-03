# Gera o par de chaves do updater. A privada assina a release (nunca vai para o repo);
# a publica vai para kUpdatePubKeyPem em installer/installer.cpp.
# Uso: powershell -ExecutionPolicy Bypass -File tools\gen_update_key.ps1 [-OutDir tools] [-Bits 3072]
param(
    [string]$OutDir = "tools",
    [int]$Bits = 3072,
    [switch]$Force
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_openssl.ps1")

$priv = Join-Path $OutDir "update_key.pem"
$pub = Join-Path $OutDir "update_key.pub.pem"
if (((Test-Path $priv) -or (Test-Path $pub)) -and -not $Force) {
    throw "ja existe chave em $OutDir; use -Force para trocar (as releases antigas deixam de verificar)"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$ssl = Find-OpenSsl
& $ssl genpkey -algorithm RSA -pkeyopt "rsa_keygen_bits:$Bits" -out $priv
if ($LASTEXITCODE -ne 0) { throw "openssl falhou ao gerar a chave privada" }
& $ssl pkey -in $priv -pubout -out $pub
if ($LASTEXITCODE -ne 0) { throw "openssl falhou ao gerar a chave publica" }

Write-Host ""
Write-Host "chave privada: $priv  (NAO commitar; guarde com backup)"
Write-Host "chave publica: $pub"
Write-Host ""
Write-Host "Cole o conteudo de $pub em kUpdatePubKeyPem (installer/installer.cpp) e recompile o instalador."
