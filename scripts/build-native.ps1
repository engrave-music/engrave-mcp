param(
    [int]$Jobs = 12,
    [string]$BuildDirectory = 'build/xmake-windows',
    [ValidateSet('release', 'debug', 'releasedbg', 'asan')][string]$Profile = 'release',
    [string[]]$Targets = @('engrave_mcp', 'engrave_mcp_test', 'registry_dump', 'registry_check', 'host_api_test'),
    [string]$EngineSource = ''
)
if ($Profile -ne 'release' -and -not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = "build/$Profile"
}
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
    # The pixi environment installs vendor/engrave as an editable package, so the
    # submodule must exist before the first pixi command.
    & git submodule update --init --depth 1 -- vendor/engrave
    if ($LASTEXITCODE) { throw 'Engine submodule initialization failed' }
    & pixi run --locked python scripts/bootstrap.py
    if ($LASTEXITCODE) { throw 'Engine bootstrap failed' }
    & pixi run --locked python scripts/prepare.py
    if ($LASTEXITCODE) { throw 'Native source preparation failed' }
    & pixi run --locked python scripts/prepare_xmake.py --check --engine-source $EngineSource
    if ($LASTEXITCODE) { throw 'xmake input validation failed' }
    & .pixi/envs/default/python.exe scripts/xbuild.py --profile $Profile --build-directory $BuildDirectory configure "--engine_source=$EngineSource"
    if ($LASTEXITCODE) { throw 'Native configuration failed' }
    & .pixi/envs/default/python.exe scripts/xbuild.py --profile $Profile --build-directory $BuildDirectory build -j $Jobs @Targets
    if ($LASTEXITCODE) { throw 'Native build failed' }
} finally { Pop-Location }
