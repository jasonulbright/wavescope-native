# Signed Windows releases

Version 1.1.1 introduced signing; 1.2.0 adds an MSI installer and explicit
portable filenames. Local ZIPs and MSIs remain unsigned; a workflow run is
required to obtain signatures.

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
   as the local graphics verification record. Run
   `./scripts/test-installer.ps1 -RunGraphicsTest` to verify the installer.
3. Commit/review the changes and tag that commit `v<version>`. Push the tag,
   or dispatch **Signed release** with an existing version tag.
4. Download the workflow's draft release ZIP, MSI and checksums. Verify them as
   below, extract to a fresh directory, and run that signed executable's
   `selftest` on a Windows desktop. Also run the installer lifecycle test
   against the downloaded MSI with `-Installer <path> -RunGraphicsTest`.
   This tests both actual hosted packages.
5. Review the release notes and publish the draft only after these checks pass.

The workflow uses commit-pinned actions and projectM sources. Its build job
has read-only repository access and no Azure token permission. Only the sign
job receives OIDC access, within `release`. A separate job can write the
draft release. A rerun can replace draft assets but refuses to modify a
published release.

The workflow first signs `WaveScope.exe`, `projectM-4.dll`, and
`projectM-4-playlist.dll`, retaining Microsoft's signatures on the VC runtime.
Before ZIP creation it requires valid Authenticode signatures, the expected
signer, and a timestamp on the three signed binaries. It builds the portable
ZIP and an MSI containing those signed files, then signs the MSI itself.
It verifies all four signatures and writes SHA-256 checksums after signing.
The MSI uses an embedded cabinet, covered by the package signature; it has
no separate uninstaller executable. Windows Installer handles removal.
Any signing, verification or installer lifecycle failure prevents release creation.

Hosted Windows runners do not provide the desktop OpenGL environment used
by the graphics/Spout selftest. CI therefore runs the unit/adversarial suite
and verifies staging and installer install/repair/uninstall behavior, with
an explicit `-SkipRenderTest`. The local graphics
gates in steps 2 and 4 remain required; the workflow does not enforce them.

## Verify a download

Download `WaveScope-v<version>-portable-win64.zip` and/or
`WaveScope-v<version>-installer-win64.msi`, plus each one's `.sha256` file,
from the same [GitHub release](https://github.com/jasonulbright/wavescope-native/releases).
In PowerShell, replace the version in this example as needed:

```powershell
$zip = 'WaveScope-v1.2.0-portable-win64.zip'
$expected = (Get-Content "$zip.sha256").Split(' ')[0]
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ine $expected) {
    throw 'ZIP checksum does not match'
}
Expand-Archive $zip -DestinationPath .\verified-download
$folder = '.\verified-download\WaveScope-v1.2.0-portable-win64'
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

For the MSI, use `Get-FileHash` against its `.msi.sha256` file and
`Get-AuthenticodeSignature .\WaveScope-v1.2.0-installer-win64.msi`.
It must also have `Valid` status, signer **Jason Ulbright**, and a timestamp.
The repository verifier accepts `-Installer <path>` to enforce these checks
alongside the extracted portable payload.

## Installer build and lifecycle tests

`package.bat` builds both formats. `scripts/build-installer.ps1` can rebuild
the MSI from an existing staged portable folder. It restores WiX 5.0.2 from
`.config/dotnet-tools.json` and the matching `WixToolset.UI.wixext` version.
A .NET 8+ SDK is required. No WiX tools or .NET runtime are needed by users.
See [WiX signing](https://docs.firegiant.com/wix/tools/signing/) and
[installer UI documentation](https://docs.firegiant.com/wix/tools/wixext/wixui/).

The MSI installs per-user, registers with Windows Installer / Settings →
Apps, and creates a Start menu shortcut. Its stable UpgradeCode and component
identifiers support future major upgrades; the stored install directory
is reused during maintenance. Downgrades are rejected. It does not enable
startup, launch the app automatically, or remove user settings/recordings.
Close the app before maintenance; disable opt-in startup before uninstall.

```powershell
./scripts/test-installer.ps1 -RunGraphicsTest
# Optional: supply a lower-version MSI to exercise upgrade and downgrade rejection.
./scripts/test-installer.ps1 -UpgradeFrom C:\path\to\older-installer.msi
```

The test refuses to modify an existing WaveScope installation or shortcut.
It installs into a unique `.qa` folder, checks registration and the shortcut,
repairs a deliberately removed README, uninstalls, and verifies preservation
of sentinel user files. With `-RunGraphicsTest` it also runs the installed
executable's full selftest. Logs stay in the test folder. Only the fresh
test installation and its sentinel files are removed.

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
