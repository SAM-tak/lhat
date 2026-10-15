# Regenerate the committed JIT stencils, then verify their source hash.
param(
    [string]$Clang
)

$ErrorActionPreference = 'Stop'

if (-not $Clang) {
    $command = Get-Command clang -ErrorAction SilentlyContinue
    if ($command) {
        $Clang = $command.Source
    } else {
        $llvm = Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe'
        if (Test-Path -LiteralPath $llvm) {
            $Clang = $llvm
        } else {
            $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
            if (Test-Path -LiteralPath $vswhere) {
                $Clang = & $vswhere -latest -products '*' -find 'VC\Tools\Llvm\x64\bin\clang.exe' |
                    Select-Object -First 1
            }
        }
    }
}

if (-not $Clang) {
    throw 'Clang was not found. Install LLVM or Visual Studio C++ Clang tools, or pass -Clang <path>.'
}

$generator = Join-Path $PSScriptRoot 'gen_stencils.py'
Write-Host "Generating JIT stencils with $Clang"
python $generator --clang $Clang
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python $generator --check
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host 'JIT stencils updated and source hash verified.'
