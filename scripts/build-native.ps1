param(
    [int]$Jobs = 12,
    [string]$BuildDirectory = 'build/xmake-windows',
    [string[]]$Targets = @('engrave_mcp', 'engrave_mcp_test', 'registry_dump', 'registry_check', 'host_api_test', 'export_job_test')
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio C++ x64 toolchain is required' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    # bootstrap.py initializes an absent vendor/musescore submodule and prepare.py
    # refuses to run until it is initialized, so a fresh checkout needs this order.
    & pixi run --locked -e native python scripts/bootstrap.py
    if ($LASTEXITCODE) { throw 'Engine bootstrap failed' }
    & pixi run --locked -e native python scripts/prepare.py
    if ($LASTEXITCODE) { throw 'Native source preparation failed' }
    & pixi run --locked -e native python scripts/prepare_xmake.py --check
    if ($LASTEXITCODE) { throw 'xmake input validation failed' }
    & xmake f -y -m release -o $BuildDirectory
    if ($LASTEXITCODE) { throw 'Native configuration failed' }
    & xmake build -y -j $Jobs @Targets
    if ($LASTEXITCODE) { throw 'Native build failed' }
} finally { Pop-Location }
