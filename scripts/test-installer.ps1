[CmdletBinding()]
param([string]$Installer, [string]$UpgradeFrom, [switch]$RunGraphicsTest)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$Installer) {
    if ((Get-Content "$repo\src\version.h" -Raw) -notmatch '#define WAVESCOPE_VERSION L"(\d+\.\d+\.\d+)"') { throw 'Invalid version' }
    $Installer = "$repo\dist\WaveScope-v$($Matches[1])-installer-win64.msi"
}
$Installer = (Resolve-Path -LiteralPath $Installer).Path
if ($UpgradeFrom) { $UpgradeFrom = (Resolve-Path -LiteralPath $UpgradeFrom).Path }
$engine = New-Object -ComObject WindowsInstaller.Installer
function Get-MsiProperty([string]$path, [string]$name) {
    $db = $engine.OpenDatabase($path, 0)
    $view = $db.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''' + $name + '''')
    try {
        [void]$view.Execute()
        $record = $view.Fetch()
        if (!$record) { throw "Missing MSI property $name" }
        try { return $record.StringData(1) } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record) }
    } finally {
        [void]$view.Close()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($db)
    }
}
$upgradeCode = Get-MsiProperty $Installer 'UpgradeCode'
$productCode = Get-MsiProperty $Installer 'ProductCode'
$version = Get-MsiProperty $Installer 'ProductVersion'
if (@($engine.RelatedProducts($upgradeCode, 0)).Count) { throw 'WaveScope is already installed. This test refuses to modify an existing installation.' }
if ($UpgradeFrom -and (Get-MsiProperty $UpgradeFrom 'UpgradeCode') -ne $upgradeCode) { throw 'Upgrade fixture belongs to another product' }
$testRoot = [IO.Path]::GetFullPath("$repo\.qa\msi-test-$([Guid]::NewGuid().ToString('N'))")
$app = Join-Path $testRoot 'app'
New-Item -ItemType Directory -Path $testRoot | Out-Null
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'WaveScope\WaveScope.lnk'
if (Test-Path -LiteralPath $startMenu) { throw 'An existing WaveScope shortcut would be overwritten; test stopped.' }
function Invoke-Msi([string]$arguments, [string]$log) {
    $process = Start-Process msiexec.exe -ArgumentList "$arguments /qn /norestart /l*v `"$testRoot\$log.log`"" -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(180000)) { throw "Installer exceeded three minutes; inspect $testRoot" }
    if ($process.ExitCode -notin @(0, 3010)) { throw "Installer failed ($($process.ExitCode)); inspect $testRoot\$log.log" }
}
$data = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'WaveScope'
$hadData = Test-Path -LiteralPath $data
$sentinel = Join-Path $data ("installer-test-" + [Guid]::NewGuid().ToString('N') + '.txt')
$unmanaged = Join-Path $app 'installer-test-user-file.txt'
$installed = $false
try {
    $first = if ($UpgradeFrom) { $UpgradeFrom } else { $Installer }
    $installed = $true # cleanup also covers a partially completed installation
    Invoke-Msi "/i `"$first`" INSTALLFOLDER=`"$app`"" 'install'
    foreach ($name in @('WaveScope.exe', 'projectM-4.dll', 'projectM-4-playlist.dll', 'installed.flag')) {
        if (!(Test-Path -LiteralPath (Join-Path $app $name))) { throw "Missing installed $name" }
    }
    if (!(Test-Path -LiteralPath $startMenu)) { throw 'Start menu shortcut missing' }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($startMenu)
    if ($shortcut.TargetPath -ne "$app\WaveScope.exe") { throw 'Start menu shortcut target is incorrect' }
    New-Item -ItemType Directory -Path $data -Force | Out-Null
    [IO.File]::WriteAllText($sentinel, 'preserve-user-data')
    [IO.File]::WriteAllText($unmanaged, 'preserve-unmanaged-file')
    if ($UpgradeFrom) {
        Invoke-Msi "/i `"$Installer`" INSTALLFOLDER=`"$app`"" 'upgrade'
        # A downgrade must fail, without disturbing the upgraded installation.
        $downgrade = Start-Process msiexec.exe -ArgumentList "/i `"$UpgradeFrom`" /qn /norestart /l*v `"$testRoot\downgrade.log`"" -WindowStyle Hidden -PassThru
        if (!$downgrade.WaitForExit(180000) -or $downgrade.ExitCode -eq 0) { throw 'Downgrade was not rejected' }
    }
    if ($engine.ProductState($productCode) -ne 5) { throw 'Windows Installer product registration missing' }
    if ($engine.ProductInfo($productCode, 'AssignmentType') -ne '0') { throw 'MSI was not installed per-user' }
    if ($engine.ProductInfo($productCode, 'VersionString') -ne $version) { throw 'Installed version mismatch' }
    if (@($engine.RelatedProducts($upgradeCode, 0)).Count -ne 1) { throw 'Upgrade left multiple registered products' }
    # Exercise real MSI repair by removing one known installed asset.
    Remove-Item -LiteralPath "$app\README.md"
    Invoke-Msi "/fa `"$Installer`"" 'repair'
    if (!(Test-Path -LiteralPath "$app\README.md")) { throw 'Repair did not restore the missing file' }
    if ((Get-Item -LiteralPath "$app\WaveScope.exe").VersionInfo.ProductVersion -ne "$version.0") { throw 'Installed executable version mismatch' }
    if ($RunGraphicsTest) {
        $process = Start-Process "$app\WaveScope.exe" -ArgumentList selftest -WorkingDirectory $app -WindowStyle Hidden -PassThru
        if (!$process.WaitForExit(300000)) { $process.Kill(); throw 'Installed graphics selftest timed out' }
        Copy-Item -LiteralPath "$app\wavescope-selftest.log" -Destination "$testRoot\selftest.log"
        if ($process.ExitCode -ne 0 -or !(Select-String -LiteralPath "$testRoot\selftest.log" -SimpleMatch 'selftest: ALL PASS')) { throw 'Installed graphics selftest failed' }
        # Only the test's generated outputs in its isolated app directory.
        Get-ChildItem -LiteralPath $app -File | Where-Object { $_.Name -like 'selftest-*.png' -or $_.Name -eq 'wavescope-selftest.log' -or $_.Name -like 'wavescope-*.mp4' } | Remove-Item -Force
    }
    Invoke-Msi "/x $productCode" 'uninstall'
    $installed = $false
    if ((Test-Path -LiteralPath "$app\WaveScope.exe") -or (Test-Path -LiteralPath $startMenu) -or @($engine.RelatedProducts($upgradeCode, 0)).Count) { throw 'Uninstall left managed files, shortcut, or registration' }
    if ([IO.File]::ReadAllText($sentinel) -ne 'preserve-user-data' -or [IO.File]::ReadAllText($unmanaged) -ne 'preserve-unmanaged-file') { throw 'User files were changed or removed' }
    "Installer lifecycle: PASS (install, shortcut, registration, repair, uninstall, user-data preservation; upgrade fixture: $([bool]$UpgradeFrom)). Logs: $testRoot"
} finally {
    if ($installed) {
        foreach ($code in @($engine.RelatedProducts($upgradeCode, 0))) { Invoke-Msi "/x $code" 'cleanup' }
    }
    if (Test-Path -LiteralPath $sentinel) { Remove-Item -LiteralPath $sentinel }
    if (Test-Path -LiteralPath $unmanaged) { Remove-Item -LiteralPath $unmanaged }
    if (!$hadData -and (Test-Path -LiteralPath $data) -and !(Get-ChildItem -LiteralPath $data -Force)) { Remove-Item -LiteralPath $data }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine)
}
