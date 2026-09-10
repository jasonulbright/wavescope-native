[CmdletBinding()]
param([string]$Stage, [string]$Version)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
if (!$Version) {
    if ((Get-Content "$repo\src\version.h" -Raw) -notmatch '#define WAVESCOPE_VERSION L"(\d+\.\d+\.\d+)"') { throw 'Invalid version' }
    $Version = $Matches[1]
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid MSI version' }
if (!$Stage) { $Stage = "$repo\dist\WaveScope-v$Version-portable-win64" }
$Stage = (Resolve-Path -LiteralPath $Stage).Path
if (!(Test-Path -LiteralPath "$Stage\WaveScope.exe")) { throw 'Stage the release before building its installer' }
if (Test-Path -LiteralPath "$Stage\installed.flag") { throw 'Portable staging contains an installed-edition marker' }
$work = "$repo\dist\installer-build"
New-Item -ItemType Directory -Path $work -Force | Out-Null
& dotnet tool restore
if ($LASTEXITCODE -ne 0) { throw 'Cannot restore pinned WiX tool' }
& dotnet tool run wix extension add WixToolset.UI.wixext/5.0.2
if ($LASTEXITCODE -ne 0) { throw 'Cannot restore pinned WiX UI extension' }
# Stable component identifiers and registry key paths support per-user repair/upgrades.
function Get-Id([string]$value) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($value.ToLowerInvariant()))).Replace('-', '').Substring(0, 32) }
    finally { $sha.Dispose() }
}
function Escape-Xml([string]$value) { return [Security.SecurityElement]::Escape($value) }
$dirs = @{ '.' = 'INSTALLFOLDER' }
$directoryXml = [Collections.Generic.List[string]]::new()
$componentXml = [Collections.Generic.List[string]]::new()
foreach ($file in (Get-ChildItem -LiteralPath $Stage -Recurse -File | Sort-Object FullName)) {
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in installer payload' }
    $relative = $file.FullName.Substring($Stage.Length + 1)
    if ($relative -match '(^|\\)(wavescope\.ini|selftest-[^\\]*|wavescope-selftest\.log)$|\.mp4$|^presets\\user\\') { throw "User/test file in installer: $relative" }
    $parts = $relative.Split('\')
    $parent = '.'
    for ($i = 0; $i -lt $parts.Length - 1; $i++) {
        $path = if ($parent -eq '.') { $parts[$i] } else { "$parent\$($parts[$i])" }
        if (!$dirs.ContainsKey($path)) {
            $id = 'D_' + (Get-Id $path)
            $directoryXml.Add('<DirectoryRef Id="' + $dirs[$parent] + '"><Directory Id="' + $id + '" Name="' + (Escape-Xml $parts[$i]) + '" /></DirectoryRef>')
            $dirs[$path] = $id
        }
        $parent = $path
    }
    $id = Get-Id $relative
    $guid = ([Guid]::ParseExact((Get-Id ('WaveScope/file/' + $relative)), 'N')).ToString()
    $componentXml.Add('<Component Id="C_' + $id + '" Directory="' + $dirs[$parent] + '" Guid="' + $guid + '"><File Id="F_' + $id + '" Source="' + (Escape-Xml $file.FullName) + '" /><RegistryValue Root="HKCU" Key="Software\Signal Ridge Labs\WaveScope\Installer\Files" Name="' + $id + '" Type="integer" Value="1" KeyPath="yes" /></Component>')
}
# Remove only empty installed asset directories, never recursive user-data trees.
foreach ($dir in $dirs.Keys | Where-Object { $_ -ne '.' } | Sort-Object) {
    $id = Get-Id $dir
    $componentXml.Add('<Component Id="R_' + $id + '" Directory="' + $dirs[$dir] + '" Guid="*"><RemoveFolder Id="R_' + $id + '" On="uninstall" /><RegistryValue Root="HKCU" Key="Software\Signal Ridge Labs\WaveScope\Installer\Directories" Name="' + $id + '" Type="integer" Value="1" KeyPath="yes" /></Component>')
}
$xml = '<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs"><Fragment>' + ($directoryXml -join "`n") + '<ComponentGroup Id="ReleaseFiles">' + ($componentXml -join "`n") + '</ComponentGroup></Fragment></Wix>'
[IO.File]::WriteAllText("$work\Files.wxs", $xml, [Text.UTF8Encoding]::new($false))
$license = (Get-Content "$repo\LICENSE" -Raw).Replace('\', '\\').Replace('{', '\{').Replace('}', '\}').Replace("`r", '').Replace("`n", '\par ')
[IO.File]::WriteAllText("$work\License.rtf", '{\rtf1\ansi\deff0{\fonttbl{\f0 Segoe UI;}}\f0\fs18 ' + $license + '}', [Text.Encoding]::UTF8)
$msi = "$repo\dist\WaveScope-v$Version-installer-win64.msi"
& dotnet tool run wix build "$repo\installer\WaveScope.wxs" "$work\Files.wxs" -arch x64 -ext WixToolset.UI.wixext/5.0.2 -d "Version=$Version" -d "Repo=$repo" -d "LicenseRtf=$work\License.rtf" -out $msi -intermediateFolder "$work\obj"
if ($LASTEXITCODE -ne 0) { throw 'MSI build or validation failed' }
Write-Output "Built $msi"
