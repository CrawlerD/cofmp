# Assina o SHA256SUMS da release com a chave privada RSA (PKCS#1 v1.5 + SHA-256).
# O instalador confere com a chave publica embutida em installer.cpp (kUpdatePubKeyPem).
# Uso: powershell -ExecutionPolicy Bypass -File tools\sign_release.ps1 -Key tools\update_key.pem
param(
    [Parameter(Mandatory = $true)][string]$Key,
    [string]$File = "dist\SHA256SUMS",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_openssl.ps1")

if (-not (Test-Path $Key)) { throw "chave privada nao encontrada: $Key" }
if (-not (Test-Path $File)) { throw "manifesto nao encontrado: $File" }
if (-not $Out) { $Out = "$File.sig" }

$ssl = Find-OpenSsl
& $ssl dgst -sha256 -sign $Key -out $Out $File
if ($LASTEXITCODE -ne 0) { throw "openssl falhou ao assinar $File" }
Write-Host "assinatura: $Out"
