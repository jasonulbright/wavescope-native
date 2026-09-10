param([Parameter(Mandatory)][string]$Directory)
$ErrorActionPreference = 'Stop'
foreach ($name in @('WaveScope.exe', 'projectM-4.dll', 'projectM-4-playlist.dll')) {
    $sig = Get-AuthenticodeSignature -LiteralPath (Join-Path $Directory $name)
    if ($sig.Status -ne 'Valid' -or !$sig.TimeStamperCertificate) { throw "$name must have a valid timestamped signature (status: $($sig.Status))" }
    $signer = $sig.SignerCertificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($signer -ne 'Jason Ulbright') { throw "Unexpected signer on ${name}: $signer" }
}
# Preserve and verify Microsoft's signatures on the app-local runtime.
Get-ChildItem -LiteralPath $Directory -Filter '*.dll' | Where-Object { $_.Name -notlike 'projectM-*' } | ForEach-Object {
    $sig = Get-AuthenticodeSignature -LiteralPath $_.FullName
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Invalid Microsoft runtime signature: $($_.Name)"
    }
}
Write-Output 'All release binaries have valid signatures.'
