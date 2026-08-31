# Builds the profile-guided lhat. Two phases over one build tree: GENERATE
# instruments (compile /GL, link /GENPROFILE), a training run over bench/ and
# sample/ writes the profile beside the binaries, and flipping the cache to
# USE relinks with /USEPROFILE -- the /GL objects are shared, so the second
# build is a relink. 03 の 6.2改: this is the cure for the code-layout
# lottery, and the finishing bench run prints this machine's numbers.
#
# Usage (from the repo root, inside the dev shell):
#     . .\scripts\devshell.ps1
#     .\scripts\pgo.ps1
#
# The optimized binaries land in build\pgo (lhat.exe, lhat_bench.exe).
$ErrorActionPreference = "Stop"

if (-not $env:VCToolsInstallDir) {
    throw "MSVC environment is not loaded; dot-source scripts/devshell.ps1 first."
}

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    cmake --preset pgo
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    cmake --build --preset pgo
    if ($LASTEXITCODE -ne 0) { throw "instrumented build failed" }

    Write-Host "Training: bench cases..."
    & .\build\pgo\lhat_bench.exe | Out-Host
    & .\build\pgo\lhat_checkbench.exe | Out-Null

    Write-Host "Training: bench/train and sample..."
    Get-ChildItem bench\train\*.lh | ForEach-Object {
        & .\build\pgo\lhat.exe --run $_.FullName | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "training run failed: $($_.Name)" }
    }
    Get-ChildItem sample\*.lh | ForEach-Object {
        & .\build\pgo\lhat.exe --check $_.FullName | Out-Null
    }
    foreach ($s in "vector.lh", "asynctest.lh", "asyncpump.lh", "asyncthread.lh") {
        & .\build\pgo\lhat.exe --run (Join-Path "sample" $s) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "training run failed: $s" }
    }

    Write-Host "Relinking with the profile..."
    cmake -B build\pgo -DLHAT_PGO=USE
    if ($LASTEXITCODE -ne 0) { throw "reconfigure failed" }
    cmake --build --preset pgo
    if ($LASTEXITCODE -ne 0) { throw "optimized relink failed" }

    Write-Host "PGO build ready: build\pgo\lhat.exe -- this machine's numbers:"
    & .\build\pgo\lhat_bench.exe | Out-Host
}
finally {
    Pop-Location
}
