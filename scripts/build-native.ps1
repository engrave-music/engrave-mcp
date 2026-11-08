param(
    [int]$Jobs = 12,
    [string]$BuildDirectory = 'build/xmake-windows',
    [string[]]$Targets = @('engrave_mcp', 'engrave_mcp_test', 'registry_dump', 'registry_check', 'host_api_test'),
    [string]$EngineSource = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    # Explicitly select the submodule so cached options cannot select an old worktree.
    if (-not $EngineSource) { $EngineSource = Join-Path $projectRoot 'vendor/engrave' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio C++ x64 toolchain is required' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    # bootstrap.py initializes an absent vendor/engrave submodule and prepare.py
    # refuses to run until it is initialized, so a fresh checkout needs this order.
    & pixi run --locked python scripts/bootstrap.py
    if ($LASTEXITCODE) { throw 'Engine bootstrap failed' }
    & pixi run --locked python scripts/prepare.py
    if ($LASTEXITCODE) { throw 'Native source preparation failed' }
    & pixi run --locked python scripts/prepare_xmake.py --check --engine-source $EngineSource
    if ($LASTEXITCODE) { throw 'xmake input validation failed' }
    & xmake f -P . -y -m release -o $BuildDirectory "--engine_source=$EngineSource"
    if ($LASTEXITCODE) { throw 'Native configuration failed' }
    & xmake build -P . -y -j $Jobs @Targets
    if ($LASTEXITCODE) { throw 'Native build failed' }
} finally { Pop-Location }
