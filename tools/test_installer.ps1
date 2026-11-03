# Testa o instalador sem tocar no jogo de verdade: hw.dll sintetico, assinatura RSA e parser da API.
# Uso: powershell -ExecutionPolicy Bypass -File tools\test_installer.ps1 [-Exe dist\CoF_Coop_Mod.exe]
param(
    [string]$Exe = "dist\CoF_Coop_Mod.exe"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "find_openssl.ps1")

function Assert($cond, $msg) {
    if (-not $cond) { throw "FALHOU: $msg" }
}

$Exe = (Resolve-Path $Exe).Path
$ssl = Find-OpenSsl
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("cofmp-test-" + [Guid]::NewGuid().ToString("N"))
try {
    # selftest do SHA-256
    & $Exe --selftest
    Assert ($LASTEXITCODE -eq 0) "selftest"

    # jogo sintetico (IsGameDir: cof.exe + cryoffear\liblist.gam + cryoffear\cl_dlls\hl.dll)
    $cry = Join-Path $tmp "cryoffear"
    New-Item -ItemType Directory -Path (Join-Path $cry "cl_dlls") -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $tmp "cof.exe") -Force | Out-Null
    Set-Content -Path (Join-Path $cry "liblist.gam") -Value 'gamedll "cl_dlls/hl.dll"' -Encoding ASCII
    New-Item -ItemType File -Path (Join-Path $cry "cl_dlls\hl.dll") -Force | Out-Null

    $sigJe = [byte[]](0x74, 0x2C, 0xD9, 0x45, 0xFC, 0xDC, 0x1D)
    $hw = New-Object byte[] 4096
    for ($i = 0; $i -lt $hw.Length; $i++) { $hw[$i] = 0x90 }
    [Array]::Copy($sigJe, 0, $hw, 1000, $sigJe.Length)
    $hwPath = Join-Path $tmp "hw.dll"
    [IO.File]::WriteAllBytes($hwPath, $hw)

    # aplica o patch
    & $Exe --fps --game $tmp --no-update
    Assert ($LASTEXITCODE -eq 0) "--fps"
    $patched = [IO.File]::ReadAllBytes($hwPath)
    Assert ($patched[1000] -eq 0xEB) "hw.dll nao recebeu EB"
    $mod = Join-Path $cry "Mod coop"
    Assert (Test-Path (Join-Path $mod "fps.on")) "marcador fps.on"
    Assert (Test-Path (Join-Path $mod "hw.dll.orig")) "backup hw.dll.orig"

    # repetir nao estraga (idempotente)
    & $Exe --fps --game $tmp --no-update
    Assert ($LASTEXITCODE -eq 0) "--fps repetido"
    $again = [IO.File]::ReadAllBytes($hwPath)
    Assert ($again[1000] -eq 0xEB) "segundo --fps mexeu no byte"

    # restaura
    & $Exe --fps-off --game $tmp --no-update
    Assert ($LASTEXITCODE -eq 0) "--fps-off"
    $restored = [IO.File]::ReadAllBytes($hwPath)
    Assert ($restored[1000] -eq 0x74) "hw.dll nao voltou para 74"
    Assert (-not (Test-Path (Join-Path $mod "fps.on"))) "fps.on ficou para tras"
    Assert (-not (Test-Path (Join-Path $mod "hw.dll.orig"))) "hw.dll.orig ficou para tras"

    # hw.dll sem a assinatura: nao pode mexer
    $blank = New-Object byte[] 4096
    [IO.File]::WriteAllBytes($hwPath, $blank)
    & $Exe --fps --game $tmp --no-update | Out-Null
    Assert ($LASTEXITCODE -ne 0) "--fps devia falhar sem a assinatura"
    $untouched = [IO.File]::ReadAllBytes($hwPath)
    Assert ($untouched.Length -eq $blank.Length) "hw.dll desconhecido mudou de tamanho"
    Assert (-not (Test-Path (Join-Path $mod "hw.dll.orig"))) "backup criado para hw.dll desconhecido"

    # parser do JSON da API (nome + digest)
    $json = '{"assets":[{"name":"CoF_Coop_Mod.exe","browser_download_url":"https://github.com/x/y/releases/download/v1/CoF_Coop_Mod.exe","digest":"sha256:abc123"},{"name":"CoF_Tunel.exe","browser_download_url":"https://github.com/x/y/releases/download/v1/CoF_Tunel.exe","digest":"sha256:def456"}]}'
    $jsonPath = Join-Path $tmp "release.json"
    [IO.File]::WriteAllText($jsonPath, $json)
    $out = (& $Exe --dump-assets $jsonPath) -join "`n"
    Assert ($LASTEXITCODE -eq 0) "--dump-assets"
    Assert ($out -match "CoF_Coop_Mod\.exe\s+sha256:abc123") "dump-assets sem o digest do instalador"
    Assert ($out -match "CoF_Tunel\.exe\s+sha256:def456") "dump-assets sem o digest do tunel"

    # assinatura RSA: manifesto + .sig, confere e rejeita adulteracao
    $manifest = Join-Path $tmp "SHA256SUMS"
    Set-Content -Path $manifest -Value "abc  CoF_Coop_Mod.exe" -Encoding ASCII
    $key = Join-Path $tmp "key.pem"
    $pub = Join-Path $tmp "key.pub.pem"
    $sig = Join-Path $tmp "SHA256SUMS.sig"
    # o openssl escreve o progresso do gerador de chave no stderr; com ErrorActionPreference=Stop
    # isso vira erro terminante, entao as chamadas rodam com "Continue"
    $ErrorActionPreference = "Continue"
    & $ssl genpkey -algorithm RSA -pkeyopt "rsa_keygen_bits:2048" -out $key 2>$null
    Assert ($LASTEXITCODE -eq 0) "openssl genpkey"
    & $ssl pkey -in $key -pubout -out $pub 2>$null
    Assert ($LASTEXITCODE -eq 0) "openssl pkey"
    & $ssl dgst -sha256 -sign $key -out $sig $manifest 2>$null
    Assert ($LASTEXITCODE -eq 0) "openssl dgst"
    $ErrorActionPreference = "Stop"

    & $Exe --verify-signature $pub $manifest $sig
    Assert ($LASTEXITCODE -eq 0) "--verify-signature devia aceitar"
    [IO.File]::WriteAllBytes($sig, [byte[]](1, 2, 3, 4))
    & $Exe --verify-signature $pub $manifest $sig | Out-Null
    Assert ($LASTEXITCODE -eq 1) "--verify-signature devia rejeitar assinatura invalida"

    Write-Host "OK: instalador, patch de FPS, dump-assets e assinatura"
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
