param(
    [string]$GamePath,
    [switch]$PlanOnly,
    [switch]$DetectOnly,
    [switch]$UiSmokeTest
)

$ErrorActionPreference = 'Stop'

function Get-GameState([string]$Path) {
    if (-not $Path) {
        return [pscustomobject]@{ Kind = 'Invalid'; Detail = 'Select the folder containing Em4.exe.' }
    }
    try { $exe = [IO.Path]::Combine($Path, 'Em4.exe') }
    catch { return [pscustomobject]@{ Kind = 'Invalid'; Detail = 'Select the folder containing Em4.exe.' } }
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf -ErrorAction SilentlyContinue)) {
        return [pscustomobject]@{ Kind = 'Invalid'; Detail = 'Select the folder containing Em4.exe.' }
    }
    $pluginDir = Join-Path $Path 'EM4Plugins'
    if (Test-Path -LiteralPath (Join-Path $pluginDir 'install-manifest.json') -PathType Leaf) {
        return [pscustomobject]@{ Kind = 'Managed'; Detail = 'An EM4 Debug installation was found. The wizard can update or remove it.' }
    }
    foreach ($file in @(
        (Join-Path $Path 'dinput8.dll'),
        (Join-Path $pluginDir 'em4_debug.dll'),
        (Join-Path $pluginDir 'em4_borderless.dll')
    )) {
        if (Test-Path -LiteralPath $file) {
            return [pscustomobject]@{ Kind = 'Conflict'; Detail = "An unmanaged DLL already exists: $file" }
        }
    }
    return [pscustomobject]@{ Kind = 'Fresh'; Detail = 'EM4 Debug is ready to install.' }
}

function Find-GameFolders {
    $steamRoots = New-Object 'System.Collections.Generic.List[string]'
    foreach ($key in @('HKCU:\Software\Valve\Steam',
                       'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        try {
            $value = (Get-ItemProperty -Path $key -ErrorAction Stop).SteamPath
            if ($value) { $steamRoots.Add($value) }
        } catch { }
    }
    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    if ($programFilesX86) { $steamRoots.Add((Join-Path $programFilesX86 'Steam')) }
    foreach ($drive in Get-PSDrive -PSProvider FileSystem) {
        $steamRoots.Add((Join-Path $drive.Root 'SteamLibrary'))
        $steamRoots.Add((Join-Path $drive.Root 'Steam'))
    }
    $libraries = New-Object 'System.Collections.Generic.List[string]'
    foreach ($root in ($steamRoots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container -ErrorAction SilentlyContinue)) { continue }
        $libraries.Add($root)
        $libraryFile = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (-not (Test-Path -LiteralPath $libraryFile -PathType Leaf)) { continue }
        try {
            $contents = Get-Content -LiteralPath $libraryFile -Raw
            foreach ($match in [regex]::Matches($contents, '"path"\s*"([^"]+)"')) {
                $library = $match.Groups[1].Value -replace '\\\\', '\'
                if ($library) { $libraries.Add($library) }
            }
        } catch { }
    }
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $candidate = [IO.Path]::Combine($library, 'steamapps', 'common', '911 First Responders')
        $exe = [IO.Path]::Combine($candidate, 'Em4.exe')
        if (Test-Path -LiteralPath $exe -PathType Leaf -ErrorAction SilentlyContinue) {
            Write-Output $candidate
        }
    }
}

if ($DetectOnly) {
    Find-GameFolders
    exit 0
}

if ($PlanOnly) {
    $state = Get-GameState $GamePath
    Write-Output "$($state.Kind): $($state.Detail)"
    if ($state.Kind -eq 'Invalid' -or $state.Kind -eq 'Conflict') { exit 2 }
    exit 0
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

function Add-Text($Parent, [string]$Value, [int]$X, [int]$Y,
                  [int]$Width, [int]$Height, [bool]$Bold = $false) {
    $label = New-Object System.Windows.Forms.Label
    $label.Text = $Value
    $label.Location = New-Object System.Drawing.Point($X, $Y)
    $label.Size = New-Object System.Drawing.Size($Width, $Height)
    if ($Bold) { $label.Font = New-Object System.Drawing.Font('Segoe UI', 12, [System.Drawing.FontStyle]::Bold) }
    $Parent.Controls.Add($label)
    return $label
}

$script:step = 0
$script:finished = $false
$script:selectedPath = $null
$script:action = $null
$form = New-Object System.Windows.Forms.Form
$form.Text = 'EM4 Debug Setup 1.0'
$form.ClientSize = New-Object System.Drawing.Size(640, 380)
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedDialog
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$panels = @()
for ($i = 0; $i -lt 3; $i++) {
    $panel = New-Object System.Windows.Forms.Panel
    $panel.Location = New-Object System.Drawing.Point(18, 16)
    $panel.Size = New-Object System.Drawing.Size(604, 295)
    $panel.Visible = $false
    $form.Controls.Add($panel)
    $panels += $panel
}

Add-Text $panels[0] 'Find your game' 8 8 560 34 $true | Out-Null
Add-Text $panels[0] 'Select the 911 First Responders folder that contains Em4.exe.' 8 52 565 25 | Out-Null
Add-Text $panels[0] 'Game folder' 8 94 550 22 | Out-Null
$pathBox = New-Object System.Windows.Forms.TextBox
$pathBox.Location = New-Object System.Drawing.Point(8, 120)
$pathBox.Size = New-Object System.Drawing.Size(480, 27)
$panels[0].Controls.Add($pathBox)
$browse = New-Object System.Windows.Forms.Button
$browse.Text = 'Browse...'
$browse.Location = New-Object System.Drawing.Point(498, 118)
$browse.Size = New-Object System.Drawing.Size(92, 30)
$panels[0].Controls.Add($browse)
$pathHint = Add-Text $panels[0] '' 8 165 580 72
$browse.Add_Click({
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select the folder containing Em4.exe'
    if (Test-Path -LiteralPath $pathBox.Text -PathType Container) {
        $dialog.SelectedPath = $pathBox.Text
    }
    if ($dialog.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
        $pathBox.Text = $dialog.SelectedPath
    }
    $dialog.Dispose()
})
$pathBox.Add_TextChanged({
    $state = Get-GameState $pathBox.Text.Trim()
    $pathHint.Text = $state.Detail
    $pathHint.ForeColor = if ($state.Kind -eq 'Conflict') {
        [System.Drawing.Color]::DarkRed
    } else { [System.Drawing.Color]::DimGray }
})

Add-Text $panels[1] 'Choose an action' 8 8 560 34 $true | Out-Null
$installState = Add-Text $panels[1] '' 8 55 575 55
$installRadio = New-Object System.Windows.Forms.RadioButton
$installRadio.Location = New-Object System.Drawing.Point(12, 126)
$installRadio.Size = New-Object System.Drawing.Size(550, 28)
$installRadio.Checked = $true
$panels[1].Controls.Add($installRadio)
$removeRadio = New-Object System.Windows.Forms.RadioButton
$removeRadio.Text = 'Remove EM4 Debug from this game'
$removeRadio.Location = New-Object System.Drawing.Point(12, 170)
$removeRadio.Size = New-Object System.Drawing.Size(550, 28)
$panels[1].Controls.Add($removeRadio)
Add-Text $panels[1] 'The game must be closed. Your mods, configuration, logs, and reload settings are kept.' 8 228 580 50 | Out-Null

Add-Text $panels[2] 'Ready' 8 8 560 34 $true | Out-Null
$summary = Add-Text $panels[2] '' 8 60 580 88
$resultText = Add-Text $panels[2] '' 8 168 580 88

$back = New-Object System.Windows.Forms.Button
$back.Text = '< Back'
$back.Location = New-Object System.Drawing.Point(359, 327)
$back.Size = New-Object System.Drawing.Size(82, 30)
$form.Controls.Add($back)
$next = New-Object System.Windows.Forms.Button
$next.Text = 'Next >'
$next.Location = New-Object System.Drawing.Point(449, 327)
$next.Size = New-Object System.Drawing.Size(82, 30)
$form.Controls.Add($next)
$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = 'Cancel'
$cancel.Location = New-Object System.Drawing.Point(539, 327)
$cancel.Size = New-Object System.Drawing.Size(82, 30)
$form.Controls.Add($cancel)
$form.CancelButton = $cancel

function Show-Step([int]$Number) {
    $script:step = $Number
    for ($i = 0; $i -lt $panels.Count; $i++) { $panels[$i].Visible = $i -eq $Number }
    $back.Enabled = $Number -gt 0 -and -not $script:finished
    $next.Text = if ($Number -eq 2) { 'Apply' } else { 'Next >' }
}

$back.Add_Click({ if ($script:step -gt 0) { Show-Step ($script:step - 1) } })
$cancel.Add_Click({ $form.Close() })
$next.Add_Click({
    if ($script:finished) { $form.Close(); return }
    if ($script:step -eq 0) {
        $state = Get-GameState $pathBox.Text.Trim()
        if ($state.Kind -eq 'Invalid') {
            [System.Windows.Forms.MessageBox]::Show($state.Detail, 'EM4 Debug Setup') | Out-Null
            return
        }
        $script:selectedPath = (Resolve-Path -LiteralPath $pathBox.Text.Trim()).Path
        $installState.Text = $state.Detail
        $installRadio.Text = if ($state.Kind -eq 'Managed') {
            'Update all EM4 Debug DLLs to version 1.0'
        } else { 'Install EM4 Debug 1.0' }
        $installRadio.Enabled = $state.Kind -ne 'Conflict'
        $removeRadio.Enabled = $state.Kind -eq 'Managed'
        $installRadio.Checked = $installRadio.Enabled
        $removeRadio.Checked = $false
        Show-Step 1
        return
    }
    if ($script:step -eq 1) {
        if (-not $installRadio.Checked -and -not $removeRadio.Checked) {
            [System.Windows.Forms.MessageBox]::Show('This folder contains another plugin. Choose a different game folder or remove that plugin manually.', 'EM4 Debug Setup') | Out-Null
            return
        }
        $script:action = if ($removeRadio.Checked) { 'Remove' } elseif (
            (Get-GameState $script:selectedPath).Kind -eq 'Managed') { 'Update' } else { 'Install' }
        $summary.Text = "Action: $($script:action)`r`nGame folder: $($script:selectedPath)`r`nOnly files owned by EM4 Debug will be changed."
        $resultText.Text = 'Click Apply to continue.'
        $resultText.ForeColor = [System.Drawing.Color]::DimGray
        Show-Step 2
        return
    }
    $next.Enabled = $false
    $back.Enabled = $false
    $cancel.Enabled = $false
    $resultText.Text = 'Working...'
    $form.Refresh()
    try {
        if ($script:action -eq 'Remove') {
            & (Join-Path $PSScriptRoot 'install.ps1') -GamePath $script:selectedPath -Uninstall | Out-Null
        } elseif ($script:action -eq 'Update') {
            & (Join-Path $PSScriptRoot 'install.ps1') -GamePath $script:selectedPath -UpgradeAll | Out-Null
        } else {
            & (Join-Path $PSScriptRoot 'install.ps1') -GamePath $script:selectedPath | Out-Null
        }
        $resultText.Text = 'Complete. Open the game through Steam to use EM4 Debug.'
        $resultText.ForeColor = [System.Drawing.Color]::DarkGreen
        $script:finished = $true
        $next.Text = 'Finish'
    } catch {
        $resultText.Text = "Could not finish: $($_.Exception.Message)"
        $resultText.ForeColor = [System.Drawing.Color]::DarkRed
        $back.Enabled = $true
        $next.Text = 'Retry'
    } finally {
        $next.Enabled = $true
        $cancel.Enabled = $true
    }
})

if ($GamePath) { $pathBox.Text = $GamePath }
else {
    $detected = @(Find-GameFolders)
    if ($detected.Count) { $pathBox.Text = $detected[0] }
}
Show-Step 0
if ($UiSmokeTest) {
    Write-Output "Wizard initialized with $($panels.Count) pages."
    $form.Dispose()
    return
}
[void]$form.ShowDialog()
$form.Dispose()
