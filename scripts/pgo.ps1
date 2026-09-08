# Builds the profile-guided lhat. Two phases over one build tree: GENERATE
# instruments, a training run over bench/ and sample/ writes the profile, and
# flipping the cache to USE builds again with it. 03 の 6.2改: this is the
# cure for the code-layout lottery, and the finishing bench run prints this
# machine's numbers.
#
# Usage (from the repo root, inside the dev shell):
#     . .\scripts\devshell.ps1
#     .\scripts\pgo.ps1
#     .\scripts\pgo.ps1 -Compiler "$env:VCToolsInstallDir..\..\Llvm\x64\bin\clang-cl.exe"
#
# The optimized binaries land in build\pgo (lhat.exe, lhat_bench.exe).
#
# The two compilers differ in where the instrumentation lives. MSVC puts it
# in the link, so its /GL objects serve both phases and USE is a relink.
# Clang puts it in the compile and writes one raw profile per process, which
# llvm-profdata merges into the single file that -fprofile-use is handed --
# so a Clang run has a step in the middle and rebuilds rather than relinks.
param(
    # Empty: whatever the preset finds, which is MSVC inside the dev shell.
    # A path to clang-cl.exe builds with Clang instead.
    [string]$Compiler = ""
)
$ErrorActionPreference = "Stop"

if (-not $env:VCToolsInstallDir) {
    throw "MSVC environment is not loaded; dot-source scripts/devshell.ps1 first."
}

# clang-cl still needs the MSVC headers and libraries the dev shell exports;
# what changes is which driver compiles and how the profile is gathered.
$clang = $Compiler -and (Split-Path -Leaf $Compiler) -match "clang"
if ($Compiler -and -not (Test-Path $Compiler)) {
    throw "no such compiler: $Compiler"
}

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    # From scratch: the profile must come from this build, and CMake will not
    # change compilers in a tree already configured for another.
    if (Test-Path "build\pgo") { Remove-Item -Recurse -Force "build\pgo" }

    $configure = @("--preset", "pgo")
    if ($Compiler) {
        # Forward slashes: a Windows path with backslashes does not survive
        # being handed in on the command line.
        $configure += "-DCMAKE_C_COMPILER=$($Compiler -replace '\\', '/')"
    }
    cmake @configure
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    cmake --build --preset pgo
    if ($LASTEXITCODE -ne 0) { throw "instrumented build failed" }

    # One raw profile per process; %p keeps them from overwriting each other.
    $prof = Join-Path (Resolve-Path ".").Path "build\pgo\prof"
    $merged = Join-Path (Resolve-Path ".").Path "build\pgo.profdata"
    if ($clang) {
        if (Test-Path $prof) { Remove-Item -Recurse -Force $prof }
        New-Item -ItemType Directory -Force -Path $prof | Out-Null
        $env:LLVM_PROFILE_FILE = Join-Path $prof "lhat-%p.profraw"
    }

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

    $use = @("-B", "build\pgo", "-DLHAT_PGO=USE")
    if ($clang) {
        Remove-Item Env:\LLVM_PROFILE_FILE
        $raw = @(Get-ChildItem "$prof\*.profraw")
        if ($raw.Count -eq 0) { throw "the training wrote no profile" }
        Write-Host "Merging $($raw.Count) profiles..."
        $profdata = Join-Path (Split-Path -Parent $Compiler) "llvm-profdata.exe"
        if (-not (Test-Path $profdata)) { throw "no llvm-profdata beside $Compiler" }
        & $profdata merge -output="$merged" @($raw.FullName)
        if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed" }
        if (-not (Test-Path $merged)) { throw "merge wrote no $merged" }
        $use += "-DLHAT_PGO_PROFILE=$($merged -replace '\\', '/')"
    }

    Write-Host "Building with the profile..."
    cmake @use
    if ($LASTEXITCODE -ne 0) { throw "reconfigure failed" }
    cmake --build --preset pgo
    if ($LASTEXITCODE -ne 0) { throw "optimized build failed" }

    Write-Host "PGO build ready: build\pgo\lhat.exe -- this machine's numbers:"
    & .\build\pgo\lhat_bench.exe | Out-Host
}
finally {
    Pop-Location
}
