param(
    [string]$ZigPath = "zig",
    [switch]$EnableCommandScriptReload
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$include = Join-Path $root "include"
$src = Join-Path $root "src"
$build = Join-Path $root "build"
$debugDefines = @()
if ($EnableCommandScriptReload) {
    $debugDefines = @(
        "-DEM4_EXPERIMENTAL_SCRIPT_RELOAD",
        "-DEM4_ALLOW_UNSAFE_SCRIPT_RELOAD_TEST"
    )
}
New-Item -ItemType Directory -Path $build -Force | Out-Null

& $ZigPath cc -target x86-windows-gnu -O2 -shared "-I$include" `
    (Join-Path $src "dinput8_proxy.c") (Join-Path $src "dinput8.def") `
    -o (Join-Path $build "dinput8.dll") -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "Failed to build dinput8.dll" }

& $ZigPath cc -target x86-windows-gnu -O2 -shared "-I$include" `
    (Join-Path $src "borderless.c") (Join-Path $src "borderless.def") `
    -o (Join-Path $build "em4_borderless.dll") -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "Failed to build em4_borderless.dll" }

& $ZigPath cc -target x86-windows-gnu -O2 -shared @debugDefines "-I$include" `
    (Join-Path $src "debug_overlay.c") (Join-Path $src "hot_reload.c") `
    (Join-Path $src "auto_watch.c") `
    (Join-Path $src "debug_overlay.def") `
    -o (Join-Path $build "em4_debug.dll") -lkernel32 -luser32 -lgdi32 -lopengl32 -lpsapi -ldbghelp
if ($LASTEXITCODE -ne 0) { throw "Failed to build em4_debug.dll" }

Write-Host "DLLs built in $build"
