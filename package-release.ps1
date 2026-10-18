param(
    [string]$Version = '1.0.0',
    [string]$ZigPath = 'zig',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Version must use the format major.minor.patch.' }
$root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$dist = Join-Path $root 'dist'
$name = "EM4-Debug-v$Version-win32"
$stage = Join-Path $dist $name
$zip = Join-Path $dist "$name.zip"

if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -ZigPath $ZigPath -EnableCommandScriptReload
}

$dllNames = @('dinput8.dll', 'em4_borderless.dll', 'em4_debug.dll')
foreach ($namePart in $dllNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $root "build\$namePart") -PathType Leaf)) {
        throw "Missing build output: $namePart"
    }
}
$debugBytes = [IO.File]::ReadAllBytes((Join-Path $root 'build\em4_debug.dll'))
$debugText = [Text.Encoding]::ASCII.GetString($debugBytes)
if ($debugText.Contains('Multi-pass bloom ready') -or
    $debugText.Contains('Realistic visual preset ready.') -or
    $debugText.Contains('glCreateShader') -or
    $debugText.Contains('glGenFramebuffers')) {
    throw 'The debug DLL still contains a rejected graphics effect.'
}
if (-not $debugText.Contains('Ctrl+F7 command script reload')) {
    throw 'The release debug DLL is missing command-script reload.'
}

New-Item -ItemType Directory -Path $dist -Force | Out-Null
$fullRoot = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
$fullStage = [IO.Path]::GetFullPath($stage)
if (-not $fullStage.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The release staging folder is outside the project.'
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage 'build'),
    (Join-Path $stage 'examples') -Force | Out-Null

foreach ($file in @('Install EM4 Debug.cmd', 'Install-EM4Debug.ps1',
                   'install.ps1', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $stage $file)
}
Copy-Item -LiteralPath (Join-Path $root 'examples\reload_target.txt') -Destination (Join-Path $stage 'examples\reload_target.txt')
foreach ($namePart in $dllNames) {
    Copy-Item -LiteralPath (Join-Path $root "build\$namePart") -Destination (Join-Path $stage "build\$namePart")
}
$hashLines = foreach ($namePart in $dllNames) {
    $file = Join-Path $stage "build\$namePart"
    '{0}  build/{1}' -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant(),
        $namePart
}
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.txt'), $hashLines,
    (New-Object Text.UTF8Encoding($false)))
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Write-Output "Release archive: $zip"
Write-Output "SHA-256: $hash"
