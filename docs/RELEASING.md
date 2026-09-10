# Signed Windows releases

Version 1.1.1 introduces the signing workflow. Local builds and locally
created ZIPs remain unsigned; a workflow run is required to obtain signatures.

## Azure and GitHub configuration

The `sign` job uses the GitHub environment **release**, which makes its OIDC
subject `repo:jasonulbright/wavescope-native:environment:release`.
It logs in with `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, and
`AZURE_SUBSCRIPTION_ID`; no client secret or signing key is stored in GitHub.

- Signing account: `signalridgelabs`
- Region endpoint: `https://eus.codesigning.azure.net/` (East US)
- Certificate profile: `SRL-Public`
- Expected certificate subject name: `Jason Ulbright`

The federated identity needs the **Artifact Signing Certificate Profile
Signer** role on this profile (the role may still appear as Trusted Signing
Certificate Profile Signer in older Azure interfaces). Federation alone
does not grant signing permission. A successful sign job verifies the
federation and role assignment against Azure; local builds do not exercise
either. The resulting signature must match the expected signer above.

References: [Azure signing action](https://github.com/Azure/artifact-signing-action),
[Azure OIDC login](https://learn.microsoft.com/en-us/azure/developer/github/connect-from-azure-openid-connect),
[signing account setup and endpoints](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart).

## Release procedure

1. Update `src/version.h` (including numeric and Windows version strings) and
   the changelog. Close any running instance from the checkout.
2. Run `package.bat` on a Windows desktop with OpenGL 3.3. This builds and
   runs unit/adversarial tests, stages the portable files, and runs the
   staged executable's full selftest. Keep `dist/selftest-v<version>.log`
   as the local graphics verification record.
3. Commit/review the changes and tag that commit `v<version>`. Push the tag,
   or dispatch **Signed release** with an existing version tag.
4. Download the workflow's draft release ZIP and checksum. Verify them as
   below, extract to a fresh directory, and run that signed executable's
   `selftest` on a Windows desktop. This tests the actual hosted build too.
5. Review the release notes and publish the draft only after these checks pass.

The workflow uses commit-pinned actions and projectM sources. Its build job
has read-only repository access and no Azure token permission. Only the sign
job receives OIDC access, within `release`. A separate job can write the
draft release. A rerun can replace draft assets but refuses to modify a
published release.

The workflow signs `WaveScope.exe`, `projectM-4.dll`, and
`projectM-4-playlist.dll`, retaining Microsoft's signatures on the VC runtime.
Before ZIP creation it requires valid Authenticode signatures, the expected
signer, and a timestamp on the three signed binaries. It hashes the final ZIP
with SHA-256. Any signing or verification failure prevents release creation.

Hosted Windows runners do not provide the desktop OpenGL environment used
by the graphics/Spout selftest. CI therefore runs the unit/adversarial suite
and verifies staging, with an explicit `-SkipRenderTest`. The local graphics
gates in steps 2 and 4 remain required; the workflow does not enforce them.

## Verify a download

Download both `WaveScope-v<version>-win64.zip` and its `.zip.sha256` file
from the same [GitHub release](https://github.com/jasonulbright/wavescope-native/releases).
In PowerShell, replace the version in this example as needed:

```powershell
$zip = 'WaveScope-v1.1.1-win64.zip'
$expected = (Get-Content "$zip.sha256").Split(' ')[0]
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ine $expected) {
    throw 'ZIP checksum does not match'
}
Expand-Archive $zip -DestinationPath .\verified-download
$folder = '.\verified-download\WaveScope-v1.1.1-win64'
Get-AuthenticodeSignature "$folder\WaveScope.exe", "$folder\projectM-4.dll", "$folder\projectM-4-playlist.dll" |
    Select-Object Path, Status, @{Name='Signer'; Expression={$_.SignerCertificate.Subject}},
        @{Name='Timestamp'; Expression={$_.TimeStamperCertificate.Subject}}
```

All three statuses must be `Valid`, each signer must identify **Jason
Ulbright**, and each timestamp must be present. From a repo checkout,
`./scripts/verify-signatures.ps1 -Directory $folder` enforces these checks
and verifies the bundled Microsoft runtime DLLs. You can also inspect the
app's **Properties → Digital Signatures** in Explorer. A checksum detects a
changed download; Authenticode verifies the binary's publisher and integrity.

## Build dependency from the pinned source

From this repository's root, check out the exact pinned source and its
submodules, then build (choose an unused checkout directory):

```powershell
git clone --no-checkout https://github.com/projectM-visualizer/projectm.git C:\path\to\projectm
git -C C:\path\to\projectm checkout 149bfc43963e54e397ccdb42f27019d22b659098
git -C C:\path\to\projectm submodule update --init --recursive
./scripts/build-projectm.ps1 -SourcePath C:\path\to\projectm
$env:PMROOT = 'C:\path\to\projectm\install-native'
./build.bat
```

The script supports Visual Studio 2022 and 2026. The workflow performs this
checkout automatically. Local packaging includes only tracked preset and
texture assets, documentation and README screenshots, so `presets/user`,
personal settings and recordings cannot accidentally enter the release ZIP.
