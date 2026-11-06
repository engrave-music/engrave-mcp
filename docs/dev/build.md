# Build and test

The host is built with **xmake**; **Pixi** provides the pinned toolchain and
native libraries. The engine's build lives in the fork
(`vendor/engrave/xmake/engrave.lua`) and is included by the host's
`xmake.lua`.

## Prerequisites

Windows:

- Git and [Pixi](https://pixi.sh/).
- Visual Studio 2022 with the x64 C++ toolchain (MSVC 14.51+).
- xmake 3.1.1+ (the build scripts use the one available on `PATH`).

Linux (validated on Linux x86_64, GCC 15.3, C++23):

- Git and Pixi.
- Cairo in the Pixi environment (passed with `--cairo_prefix`).
- WSL is not used for this project.

> If xmake's bundled aria2 fails to download a package with a TLS handshake
> error, run `xmake g --downloader=curl` once.

## First checkout

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont
pixi run -e native python scripts/prepare.py
```

`bootstrap.py` initializes the `vendor/engrave` submodule and downloads the
base SoundFont. `prepare.py` verifies the submodule pin and installs the engine's
hash-pinned asset manifest (345 files, 28,380,646 bytes) into `.runtime/assets`;
it refuses to run before the submodule exists.

## Development builds

Once a build directory is configured (by the wrapper below or `xmake f`), iterate
with:

```powershell
pixi run build                       # default targets
pixi run build engrave_mnx_import_test  # specific targets
```

This runs `xmake build -y` directly, with no pin or clean-tree checks, so an
uncommitted engine change builds as is. xmake finds MSVC itself; no developer
shell is needed. The wrappers below are for first, packaging and acceptance
builds: they refuse to build unless `vendor/engrave` is clean and at the
recorded pin.

## Windows build

```powershell
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
```

The wrapper enters the Visual Studio developer shell, runs bootstrap/prepare and
the xmake input check, configures `build/xmake-windows`, and builds these
targets by default:

`engrave_mcp`, `engrave_mcp_test`, `registry_dump`, `registry_check`,
`host_api_test`.

Build other targets explicitly, for example:

```powershell
xmake build live_export_test staging_test
```

(`-Targets` takes a PowerShell array; passing a comma-separated string to
`pwsh -File` does not split it.)

`--engine_source=DIR` overrides the engine checkout. `releasedbg` is available
for symbolized profiling.

### Running the dev build

The build links Pixi's native libraries, so they must be on `PATH`:

```powershell
$env:PATH = "$PWD\.pixi\envs\native\Library\bin;$env:PATH"
build/xmake-windows/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

Without the Pixi DLL directory the process exits during `initialize`; this is a
loader failure, not a server failure. Packaged builds bundle the DLLs.

## Linux build

```bash
bash scripts/build-native.sh 8 build/xmake-linux "$PWD/.pixi/envs/native"
```

The third argument is the Cairo prefix (`<prefix>/include/cairo`, `<prefix>/lib`).
The output is `build/xmake-linux/engrave-mcp`.

## Test binaries

Some lanes need targets that the default build does not produce:

```powershell
xmake build live_export_test staging_test
```

- `live_export_test` — direct public-API oracle for live export.
- `staging_test` — private staging and Windows ACL contract.
- `publication_test` — Linux-only fsync ordering/failure injection.

The golden and compatibility lanes also need the engine's `semantic_dump`
probe, which is opt-in:

```powershell
xmake f --engrave_tests=y
xmake build semantic_dump
```

`--engrave_tests=y` pulls GoogleTest and the engine's own test targets into the
build graph; it is not part of the production build.

## Packaging

Windows:

```powershell
pixi run -e native python scripts/package_windows.py --no-qt `
  --binary build/xmake-windows/engrave-mcp.exe --output artifacts/windows-next
```

Linux:

```bash
pixi run --locked -e native python scripts/package_linux.py \
  --binary build/xmake-linux/engrave-mcp --output artifacts/linux-next
```

Both packagers collect the runtime DLL/ELF closure, assets and SoundFonts, and
refuse Qt libraries. A packaged server finds its sibling `assets/` directory
automatically, so `--resources` is unnecessary. File counts and sizes are
whatever the generated `manifest.json` records; do not hardcode them.

## Tests and lanes

Run the smoke test first:

```powershell
pixi run -e native python tests/integration/test_mcp_smoke.py
pixi run -e native python tests/integration/test_transport.py build/xmake-windows/engrave-mcp.exe
```

The acceptance runner and its lane list are documented in
[validation.md](validation.md). It needs a build directory that contains the
binaries the selected lanes use; pass `--build-dir build/xmake-windows` for the
Windows build, not the repository's `build/` directory.

## Layout notes

- `engrave_mcp/` host sources may include only public `engrave/...` headers;
  `native_include_check` enforces this before host targets build.
- `engrave_mcp/tools.json` is generated by `registry_dump` and checked by
  `registry_check`; regenerate it instead of editing it by hand.
- `build/`, `.pixi/`, `.xmake/`, `.runtime/`, `.upstream/`, `artifacts/` and
  `notes/` are ignored.

## Style checks

Engine naming follows `.clang-tidy`: CamelCase types, snake_case functions,
variables and members, and UPPER_CASE enum constants. Members may retain a
trailing `_`. Use `struct` for custom types. clang-format enforces own-header,
standard, external and project include groups using the engine configuration.

```powershell
pixi run format -- .
pixi run format-check
pixi run lint
pixi run format -- --engine vendor/engrave
pixi run format-check -- --engine
pixi run lint -- --engine
pixi run setup-hooks
```

Engine commands accept file or directory arguments relative to the current
working directory and exclude thirdparty directories. `format-check -- --engine`
checks all owned engine C++ files. `lint -- --engine` checks compiled owned TUs
from the root `compile_commands.json` in parallel and fails on naming diagnostics
or empty selections. Generate the database with `xmake project -k compile_commands`
after configuring `--engrave_tests=y`. The full engine lint is an acceptance step
for engine changes. The installed engine pre-commit hook format-checks staged
blobs; naming is checked at acceptance.

For a build using `--engine_source=DIR`, pass `--engine-root DIR` to engine
lint, format and setup-hooks. Supply DIR as a selection path as well when it is
outside the current directory. Hook setup validates an initialized engine
checkout before changing hooks. Rerun setup-hooks after updating hook code.
Engine lint intentionally rejects `--fix`, since parallel TUs share headers.
For a scoped format check, use `pixi run format -- --check --engine PATH`;
the format-check task includes `.` and checks the full selected engine.
