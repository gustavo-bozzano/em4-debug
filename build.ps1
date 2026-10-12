param(
    [string]$ZigPath = "zig"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$include = Join-Path $root "include"
$src = Join-Path $root "src"
$build = Join-Path $root "build"
New-Item -ItemType Directory -Path $build -Force | Out-Null

& $ZigPath cc -target x86-windows-gnu -O2 -shared "-I$include" `
    (Join-Path $src "dinput8_proxy.c") (Join-Path $src "dinput8.def") `
    -o (Join-Path $build "dinput8.dll") -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "Failed to build dinput8.dll" }

& $ZigPath cc -target x86-windows-gnu -O2 -shared "-I$include" `
    (Join-Path $src "borderless.c") (Join-Path $src "borderless.def") `
    -o (Join-Path $build "em4_borderless.dll") -lkernel32 -luser32
if ($LASTEXITCODE -ne 0) { throw "Failed to build em4_borderless.dll" }

& $ZigPath cc -target x86-windows-gnu -O2 -shared "-I$include" `
    (Join-Path $src "debug_overlay.c") (Join-Path $src "debug_overlay.def") `
    -o (Join-Path $build "em4_debug.dll") -lkernel32 -luser32 -lgdi32 -lopengl32 -lpsapi
if ($LASTEXITCODE -ne 0) { throw "Failed to build em4_debug.dll" }

Write-Host "DLLs built in $build"
