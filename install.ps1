param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$game = (Resolve-Path -LiteralPath $GamePath).Path
$exe = Join-Path $game 'Em4.exe'
$proxy = Join-Path $game 'dinput8.dll'
$pluginDir = Join-Path $game 'EM4Plugins'
$plugin = Join-Path $pluginDir 'em4_borderless.dll'
$debug = Join-Path $pluginDir 'em4_debug.dll'
$manifestPath = Join-Path $pluginDir 'install-manifest.json'
$sourceProxy = Join-Path $PSScriptRoot 'build\dinput8.dll'
$sourcePlugin = Join-Path $PSScriptRoot 'build\em4_borderless.dll'
$sourceDebug = Join-Path $PSScriptRoot 'build\em4_debug.dll'

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Em4.exe was not found in the specified directory.' }
if (Get-Process Em4 -ErrorAction SilentlyContinue) { throw 'Close the game before installing or uninstalling.' }

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'Installation manifest not found. Uninstall stopped to preserve unknown DLLs.'
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if (Test-Path -LiteralPath $proxy) {
        if ((Get-FileHash -LiteralPath $proxy -Algorithm SHA256).Hash -ne $manifest.proxy_sha256) {
            throw 'dinput8.dll has changed. Uninstall stopped to preserve the file.'
        }
    }
    if (Test-Path -LiteralPath $plugin) {
        if ((Get-FileHash -LiteralPath $plugin -Algorithm SHA256).Hash -ne $manifest.plugin_sha256) {
            throw 'em4_borderless.dll has changed. Uninstall stopped to preserve the file.'
        }
    }
    if (Test-Path -LiteralPath $debug) {
        if ((Get-FileHash -LiteralPath $debug -Algorithm SHA256).Hash -ne $manifest.debug_sha256) {
            throw 'em4_debug.dll has changed. Uninstall stopped to preserve the file.'
        }
    }
    if (Test-Path -LiteralPath $proxy) { Remove-Item -LiteralPath $proxy }
    if (Test-Path -LiteralPath $plugin) { Remove-Item -LiteralPath $plugin }
    if (Test-Path -LiteralPath $debug) { Remove-Item -LiteralPath $debug }
    Remove-Item -LiteralPath $manifestPath
    Write-Host 'Plugins removed. The game configuration was not changed.'
    return
}

if (-not (Test-Path -LiteralPath $sourceProxy -PathType Leaf) -or
    -not (Test-Path -LiteralPath $sourcePlugin -PathType Leaf) -or
    -not (Test-Path -LiteralPath $sourceDebug -PathType Leaf)) {
    throw 'Built DLLs were not found in the build directory. Run build.ps1 first.'
}
if (Test-Path -LiteralPath $proxy) {
    throw 'dinput8.dll already exists in the game directory. Installation stopped to avoid a mod conflict.'
}
if (Test-Path -LiteralPath $plugin) {
    throw 'em4_borderless.dll already exists in the game directory. Installation stopped.'
}
if (Test-Path -LiteralPath $debug) {
    throw 'em4_debug.dll already exists in the game directory. Installation stopped.'
}
if (Test-Path -LiteralPath $manifestPath) {
    throw 'An installation manifest already exists. Installation stopped.'
}

New-Item -ItemType Directory -Path $pluginDir -Force | Out-Null
Copy-Item -LiteralPath $sourceProxy -Destination $proxy
Copy-Item -LiteralPath $sourcePlugin -Destination $plugin
Copy-Item -LiteralPath $sourceDebug -Destination $debug
@{
    proxy_sha256 = (Get-FileHash -LiteralPath $proxy -Algorithm SHA256).Hash
    plugin_sha256 = (Get-FileHash -LiteralPath $plugin -Algorithm SHA256).Hash
    debug_sha256 = (Get-FileHash -LiteralPath $debug -Algorithm SHA256).Hash
} | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host 'Plugins installed. F11 toggles the window mode; F10 toggles the debug overlay.'
