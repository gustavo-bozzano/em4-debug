param(
    [Parameter(Mandatory = $true)]
    [string]$GamePath,
    [switch]$Uninstall,
    [switch]$UpgradeAll,
    [switch]$UpgradeDebug,
    [switch]$UpgradeBorderless
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
$reloadTarget = Join-Path $pluginDir 'reload_target.txt'
$reloadTemplate = Join-Path $PSScriptRoot 'examples\reload_target.txt'

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Em4.exe was not found in the specified directory.' }
if (Get-Process Em4 -ErrorAction SilentlyContinue) { throw 'Close the game before installing or uninstalling.' }
if ([int][bool]$Uninstall + [int][bool]$UpgradeAll + [int][bool]$UpgradeDebug +
    [int][bool]$UpgradeBorderless -gt 1) {
    throw 'Choose only one of -Uninstall, -UpgradeAll, -UpgradeDebug, or -UpgradeBorderless.'
}

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
if ($UpgradeAll -or $UpgradeDebug -or $UpgradeBorderless) {
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'Installation manifest not found. Upgrade stopped.'
    }
    $manifestText = Get-Content -LiteralPath $manifestPath -Raw
    $manifest = $manifestText | ConvertFrom-Json
    foreach ($entry in @(
        @{ Path = $proxy; Hash = $manifest.proxy_sha256 },
        @{ Path = $plugin; Hash = $manifest.plugin_sha256 },
        @{ Path = $debug; Hash = $manifest.debug_sha256 }
    )) {
        if (-not (Test-Path -LiteralPath $entry.Path -PathType Leaf) -or
            (Get-FileHash -LiteralPath $entry.Path -Algorithm SHA256).Hash -ne $entry.Hash) {
            throw "Installed file is missing or changed: $($entry.Path). Upgrade stopped."
        }
    }
    $entries = @()
    if ($UpgradeAll) {
        $entries = @(
            @{ Target = $proxy; Source = $sourceProxy; Property = 'proxy_sha256' },
            @{ Target = $plugin; Source = $sourcePlugin; Property = 'plugin_sha256' },
            @{ Target = $debug; Source = $sourceDebug; Property = 'debug_sha256' }
        )
    } elseif ($UpgradeDebug) {
        $entries = @(@{ Target = $debug; Source = $sourceDebug; Property = 'debug_sha256' })
    } else {
        $entries = @(@{ Target = $plugin; Source = $sourcePlugin; Property = 'plugin_sha256' })
    }
    $changed = @($entries | Where-Object {
        (Get-FileHash -LiteralPath $_.Source -Algorithm SHA256).Hash -ne $manifest.($_.Property)
    })
    if ($changed.Count -eq 0) {
        if (($UpgradeAll -or $UpgradeDebug) -and -not (Test-Path -LiteralPath $reloadTarget)) {
            Copy-Item -LiteralPath $reloadTemplate -Destination $reloadTarget
        }
        Write-Host 'The installed DLLs already match this build.'
        return
    }
    foreach ($entry in $changed) {
        if ((Test-Path -LiteralPath "$($entry.Target).new") -or
            (Test-Path -LiteralPath "$($entry.Target).pre-upgrade.bak")) {
            throw 'A previous upgrade staging file exists. Upgrade stopped.'
        }
    }
    $rollbackComplete = $false
    $upgradeComplete = $false
    try {
        foreach ($entry in $changed) {
            $entry.NewHash = (Get-FileHash -LiteralPath $entry.Source -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $entry.Source -Destination "$($entry.Target).new"
            if ((Get-FileHash -LiteralPath "$($entry.Target).new" -Algorithm SHA256).Hash -ne $entry.NewHash) {
                throw "Staged DLL hash mismatch: $($entry.Target)"
            }
            Copy-Item -LiteralPath $entry.Target -Destination "$($entry.Target).pre-upgrade.bak"
        }
        foreach ($entry in $changed) {
            Copy-Item -LiteralPath "$($entry.Target).new" -Destination $entry.Target -Force
            if ((Get-FileHash -LiteralPath $entry.Target -Algorithm SHA256).Hash -ne $entry.NewHash) {
                throw "Installed DLL hash mismatch: $($entry.Target)"
            }
            $manifest.($entry.Property) = $entry.NewHash
        }
        $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
        $upgradeComplete = $true
    } catch {
        $failure = $_
        try {
            foreach ($entry in $changed) {
                $backup = "$($entry.Target).pre-upgrade.bak"
                if (Test-Path -LiteralPath $backup) {
                    Copy-Item -LiteralPath $backup -Destination $entry.Target -Force
                }
            }
            Set-Content -LiteralPath $manifestPath -Value $manifestText -Encoding UTF8
            $rollbackComplete = $true
        } catch {
            throw "Upgrade failed and rollback could not finish. Backups remain beside the DLLs. Original error: $failure"
        }
        throw $failure
    } finally {
        foreach ($entry in $changed) {
            $staged = "$($entry.Target).new"
            $backup = "$($entry.Target).pre-upgrade.bak"
            if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged }
            if (($upgradeComplete -or $rollbackComplete) -and
                (Test-Path -LiteralPath $backup)) {
                Remove-Item -LiteralPath $backup
            }
        }
    }
    if (($UpgradeAll -or $UpgradeDebug) -and -not (Test-Path -LiteralPath $reloadTarget)) {
        Copy-Item -LiteralPath $reloadTemplate -Destination $reloadTarget
    }
    Write-Host 'Managed DLLs upgraded. Existing logs and reload settings were preserved.'
    return
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
$createdReloadTarget = -not (Test-Path -LiteralPath $reloadTarget)
try {
    Copy-Item -LiteralPath $sourceProxy -Destination $proxy
    Copy-Item -LiteralPath $sourcePlugin -Destination $plugin
    Copy-Item -LiteralPath $sourceDebug -Destination $debug
    if ($createdReloadTarget) {
        Copy-Item -LiteralPath $reloadTemplate -Destination $reloadTarget
    }
    foreach ($entry in @(
        @{ Source = $sourceProxy; Target = $proxy },
        @{ Source = $sourcePlugin; Target = $plugin },
        @{ Source = $sourceDebug; Target = $debug }
    )) {
        if ((Get-FileHash -LiteralPath $entry.Source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $entry.Target -Algorithm SHA256).Hash) {
            throw "Installed DLL hash mismatch: $($entry.Target)"
        }
    }
    @{
        proxy_sha256 = (Get-FileHash -LiteralPath $proxy -Algorithm SHA256).Hash
        plugin_sha256 = (Get-FileHash -LiteralPath $plugin -Algorithm SHA256).Hash
        debug_sha256 = (Get-FileHash -LiteralPath $debug -Algorithm SHA256).Hash
    } | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
} catch {
    $failure = $_
    foreach ($file in @($manifestPath, $debug, $plugin, $proxy)) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
    }
    if ($createdReloadTarget -and (Test-Path -LiteralPath $reloadTarget)) {
        Remove-Item -LiteralPath $reloadTarget
    }
    throw $failure
}
Write-Host 'Plugins installed. F11 toggles the window mode; F10 toggles the debug overlay.'
