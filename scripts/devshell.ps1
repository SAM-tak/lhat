# Import the MSVC x64 build environment into the CURRENT PowerShell session.
#
# The Ninja presets invoke cl.exe directly and therefore need INCLUDE / LIB /
# PATH set up by vcvars64.bat.  The Visual Studio generator locates the
# toolchain on its own and does not need this.
#
# Usage (note the leading dot -- it must be dot-sourced):
#
#     . .\scripts\devshell.ps1
#     cmake --preset debug
#     cmake --build --preset debug

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Is Visual Studio installed?"
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the C/C++ toolchain was found."
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}

cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}

Write-Host "MSVC x64 environment loaded: $((Get-Command cl).Source)"
