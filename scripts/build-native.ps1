param(
    [int]$Jobs = 12,
    [string]$BuildDirectory,
    [ValidateSet('release', 'debug', 'releasedbg', 'asan')][string]$Profile = 'release',
    [string[]]$Targets = @('engrave_mcp', 'engrave_mcp_test', 'registry_dump', 'registry_check', 'host_api_test'),
    [string]$EngineSource = ''
)
if (-not $PSBoundParameters.ContainsKey('BuildDirectory')) {
    $BuildDirectory = "build/$Profile"
}
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    if (-not $EngineSource) { $EngineSource = Join-Path $projectRoot 'vendor/engrave' }
    # The pixi environment installs vendor/engrave as an editable package, so the
    # submodule must exist before the first pixi command.
    & git submodule update --init --depth 1 -- vendor/engrave
    if ($LASTEXITCODE) { throw 'Engine submodule initialization failed' }
    & pixi run --locked python scripts/bootstrap.py
    if ($LASTEXITCODE) { throw 'Engine bootstrap failed' }
    & pixi run --locked python scripts/prepare.py
    if ($LASTEXITCODE) { throw 'Native source preparation failed' }
    & pixi run --locked python scripts/check_build_inputs.py --check --engine-source $EngineSource
    if ($LASTEXITCODE) { throw 'Build input validation failed' }
    & .pixi/envs/default/python.exe scripts/bbuild.py --profile $Profile --build-directory $BuildDirectory -j $Jobs build @Targets
    if ($LASTEXITCODE) { throw 'Native build failed' }
} finally { Pop-Location }
