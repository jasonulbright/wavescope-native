[CmdletBinding()]
param([switch]$SkipBuild, [switch]$SkipRenderTest, [switch]$StageOnly, [switch]$FinalizeSigned)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$versionText = Get-Content -LiteralPath "$repo\src\version.h" -Raw
if ($versionText -notmatch '#define WAVESCOPE_VERSION L"(\d+\.\d+\.\d+)"') { throw 'Invalid release version' }
$version = $Matches[1]
$dist = [IO.Path]::GetFullPath((Join-Path $repo 'dist'))
$name = "WaveScope-v$version-portable-win64"
$stage = [IO.Path]::GetFullPath((Join-Path $dist $name))
if ((Split-Path -Parent $stage) -ne $dist) { throw 'Staging path escaped dist' }
foreach ($path in @($dist, $stage)) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing a staging directory that is a reparse point: $path"
    }
}
if (!$FinalizeSigned) {
    if (!$SkipBuild) {
        & "$repo\build.bat"
        if ($LASTEXITCODE -ne 0) { throw 'Build or unit tests failed' }
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C++ tools not found' }
    $crt = Get-ChildItem -Path "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
    if (!$crt) { throw 'App-local VC runtime not found' }
    # The exact generated path above is verified before recursive removal.
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    foreach ($binary in @('WaveScope.exe', 'projectM-4.dll', 'projectM-4-playlist.dll')) {
        Copy-Item -LiteralPath (Join-Path $repo $binary) -Destination $stage
    }
    Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $stage
    # Ship tracked assets only: never personal presets, tokens or recordings.
    $files = & git -c core.quotepath=false ls-files -- presets presets-classic textures assets/screenshot-deck.png assets/screenshot-lab.png README.md LICENSE CHANGELOG.md THIRD-PARTY-NOTICES.txt
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate tracked release files' }
    foreach ($file in $files) {
        $target = Join-Path $stage $file
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $target
    }
    New-Item -ItemType Directory -Path "$stage\docs" -Force | Out-Null
    foreach ($doc in @('RELEASING.md', 'ADVERSARIAL-REVIEW.md')) {
        Copy-Item -LiteralPath "$repo\docs\$doc" -Destination "$stage\docs"
    }
    if (!$SkipRenderTest) {
        $process = Start-Process -FilePath "$stage\WaveScope.exe" -ArgumentList selftest -WorkingDirectory $stage -WindowStyle Hidden -PassThru
        if (!$process.WaitForExit(300000)) { $process.Kill(); throw 'Staged selftest exceeded five minutes' }
        if ($process.ExitCode -ne 0) { throw "Staged selftest failed: $stage\wavescope-selftest.log" }
        Copy-Item -LiteralPath "$stage\wavescope-selftest.log" -Destination "$dist\selftest-v$version.log"
        Get-ChildItem -LiteralPath $stage -File | Where-Object {
            $_.Name -like 'selftest-*.png' -or $_.Name -eq 'wavescope-selftest.log' -or $_.Name -like 'wavescope-*.mp4'
        } | Remove-Item -Force
    } else { Write-Warning 'Graphics selftest skipped; run package.bat on a Windows desktop with OpenGL 3.3 before releasing.' }
}
if (!(Test-Path -LiteralPath "$stage\WaveScope.exe")) { throw 'Staged executable is missing' }
if (Test-Path -LiteralPath "$stage\wavescope.ini") { throw 'User settings must never ship in a release' }
if (Test-Path -LiteralPath "$stage\installed.flag") { throw 'Installed-edition marker must never ship in the portable ZIP' }
if ($StageOnly) { Write-Output "Staged $stage"; exit 0 }
if ($FinalizeSigned) { & "$PSScriptRoot\verify-signatures.ps1" -Directory $stage }
$zip = Join-Path $dist "$name.zip"
Compress-Archive -LiteralPath $stage -DestinationPath $zip -Force
& "$PSScriptRoot\build-installer.ps1" -Stage $stage -Version $version
& "$PSScriptRoot\write-release-checksums.ps1"
Write-Output "Packaged $zip"
