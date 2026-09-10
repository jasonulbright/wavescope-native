param([string]$SourcePath = "$PSScriptRoot\..\.deps\projectm")
$ErrorActionPreference = 'Stop'
$pin = '149bfc43963e54e397ccdb42f27019d22b659098'
$source = (Resolve-Path -LiteralPath $SourcePath).Path
$head = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $pin) { throw 'projectM checkout does not match the audited pin' }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (!$vs) { throw 'Visual Studio C++ toolchain not found' }
$major = [int]$vs.installationVersion.Split('.')[0]
$generator = switch ($major) { 17 { 'Visual Studio 17 2022' } 18 { 'Visual Studio 18 2026' } default { throw "Unsupported VS version: $major" } }
$cmake = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$install = Join-Path $source 'install-native'
& $cmake -S $source -B "$source\build-release" -G $generator -A x64 `
    -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DENABLE_SDL_UI=OFF -DENABLE_PLAYLIST=ON `
    -DENABLE_SYSTEM_PROJECTM_EVAL=OFF -DENABLE_SYSTEM_GLM=OFF "-DCMAKE_INSTALL_PREFIX=$install"
if ($LASTEXITCODE -ne 0) { throw 'projectM configuration failed' }
& $cmake --build "$source\build-release" --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'projectM build failed' }
& $cmake --install "$source\build-release" --config Release
if ($LASTEXITCODE -ne 0) { throw 'projectM install failed' }
if ($env:GITHUB_ENV) { "PMROOT=$install" | Out-File -LiteralPath $env:GITHUB_ENV -Encoding utf8 -Append }
Write-Output "Built pinned projectM in $install"
