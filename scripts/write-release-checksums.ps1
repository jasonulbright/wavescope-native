$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if ((Get-Content "$repo\src\version.h" -Raw) -notmatch '#define WAVESCOPE_VERSION L"(\d+\.\d+\.\d+)"') { throw 'Invalid version' }
$version = $Matches[1]
foreach ($name in @("WaveScope-v$version-portable-win64.zip", "WaveScope-v$version-installer-win64.msi")) {
    $file = Join-Path "$repo\dist" $name
    $stream = [IO.File]::OpenRead($file)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha256.Dispose() }
    [IO.File]::WriteAllText("$file.sha256", "$hash  $name`n", [Text.Encoding]::ASCII)
}
