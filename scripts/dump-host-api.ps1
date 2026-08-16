# L^ (lhat) -- write lhat-host.json for the language server.
#
# 05 の 8.7: the server has to be told what the checker was told, and no one
# host knows all of it. The cli registers stdlib and links no engine; the
# GDExtension registers `godot` and is built without stdlib. So both are asked
# and the answers are merged -- the file the server reads sits at the
# workspace root, and there is only one of it (lsp/workspace.c).
#
#     . .\scripts\devshell.ps1
#     .\scripts\dump-host-api.ps1 -Godot D:\path\to\Godot_console.exe
#
# Leaving out -Godot writes the cli's half alone, which is what a checkout
# with no Godot around can do.

[CmdletBinding()]
param(
    [string]$Godot = "",
    [string]$Cli = "",
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Out -eq "") { $Out = Join-Path $root "lhat-host.json" }
if ($Cli -eq "") { $Cli = Join-Path $root "build\debug\lhat.exe" }

if (-not (Test-Path -LiteralPath $Cli)) {
    throw "no cli at $Cli -- build it, or pass -Cli"
}

$halves = @()

$fromCli = [System.IO.Path]::GetTempFileName()
& $Cli --dump-host-api $fromCli | Out-Null
if ($LASTEXITCODE -ne 0) { throw "the cli would not dump its registrations" }
$halves += $fromCli

$fromGodot = ""
if ($Godot -ne "") {
    if (-not (Test-Path -LiteralPath $Godot)) { throw "no Godot at $Godot" }
    # user:// rather than a path of our own: the demo project is what has an
    # extension loaded, and its user directory is writable wherever it runs.
    $demo = Join-Path $root "godot\demo"
    & $Godot --headless --path $demo --script dump_host_api.gd -- "user://godot-host.json" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "the extension would not dump its registrations" }
    $fromGodot = Join-Path $env:APPDATA "Godot\app_userdata\L^ GDExtension\godot-host.json"
    if (-not (Test-Path -LiteralPath $fromGodot)) {
        throw "the extension wrote nothing to $fromGodot"
    }
    $halves += $fromGodot
}

# 8.7 makes a second registration of the same name an added arm rather than a
# replacement, so two entries that differ are both kept on purpose -- only an
# entry written twice over is dropped.
$types = [ordered]@{}
$functions = [ordered]@{}
$bindings = [ordered]@{}
$annotations = [ordered]@{}

foreach ($half in $halves) {
    $parsed = Get-Content -LiteralPath $half -Raw -Encoding utf8 | ConvertFrom-Json
    foreach ($entry in $parsed.types) {
        $types[($entry | ConvertTo-Json -Compress -Depth 5)] = $entry
    }
    foreach ($entry in $parsed.functions) {
        $functions[($entry | ConvertTo-Json -Compress -Depth 5)] = $entry
    }
    foreach ($entry in $parsed.bindings) {
        $bindings[($entry | ConvertTo-Json -Compress -Depth 5)] = $entry
    }
    foreach ($entry in $parsed.annotations) {
        $annotations[($entry | ConvertTo-Json -Compress -Depth 5)] = $entry
    }
}

$merged = [ordered]@{
    types       = @($types.Values)
    functions   = @($functions.Values)
    annotations = @($annotations.Values)
    bindings    = @($bindings.Values)
}
$merged | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Out -Encoding utf8

Remove-Item -LiteralPath $fromCli -ErrorAction SilentlyContinue
"$Out : $($merged.types.Count) types, $($merged.functions.Count) functions, $($merged.annotations.Count) annotations, $($merged.bindings.Count) bindings"
