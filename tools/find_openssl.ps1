# Acha o openssl (PATH ou o que vem com o Git for Windows).
# Uso: . "$PSScriptRoot\find_openssl.ps1"; $ssl = Find-OpenSsl
function Find-OpenSsl {
    $cmd = Get-Command openssl -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        $p = Join-Path (Split-Path $git.Source -Parent) "..\usr\bin\openssl.exe"
        if (Test-Path $p) { return (Resolve-Path $p).Path }
    }
    throw "openssl nao encontrado (instale o Git for Windows ou coloque o openssl no PATH)"
}
