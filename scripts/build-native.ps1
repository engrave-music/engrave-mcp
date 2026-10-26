param([int]$Jobs = 12, [string]$BuildDirectory = 'build/native-windows')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio C++ x64 toolchain is required' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    & pixi run --locked -e native python scripts/prepare.py
    if ($LASTEXITCODE) { throw 'Native source preparation failed' }
    $prefix = (Join-Path $projectRoot '.pixi/envs/native/Library').Replace('\', '/')
    & pixi run --locked -e native cmake -S . -B $BuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$prefix"
    if ($LASTEXITCODE) { throw 'Native configuration failed' }
    & pixi run --locked -e native cmake --build $BuildDirectory --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Native build failed' }
} finally { Pop-Location }
