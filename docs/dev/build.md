# Build and test

The host is built with **xmake**; **Pixi** provides the pinned toolchain and native libraries. The engine build lives in the submodule (`vendor/engrave/xmake/engrave.lua`) and is included by the host's `xmake.lua`.

## Prerequisites

Windows:

- Git and [Pixi](https://pixi.sh/).
- Visual Studio 2022 with the x64 C++ toolchain (MSVC 14.51+).
- xmake 3.1.1+ (available on `PATH`).

Linux (validated on Linux x86_64, GCC 15.3, C++23):

- Git and Pixi.
- Cairo in the Pixi environment (passed with `--cairo_prefix`).
- WSL is not supported.

> If xmake's bundled aria2 fails to download a package with a TLS handshake error, run `xmake g --downloader=curl` once.

JSON: the host uses **glaze 9.1** for its DOM and wire writer. The engine build declares the pinned package and the `engrave-pinned` xmake repository; every host target that compiles JSON code lists `add_packages("glaze")` explicitly instead of relying on the `engrave` target's public propagation.

## Initial checkout and setup

Clone with `--recursive`, or initialize the submodule before running `pixi install` (which depends on `vendor/engrave` in editable mode):

```powershell
git submodule update --init
pixi install --locked
pixi run python scripts/bootstrap.py --soundfont
pixi run python scripts/prepare.py
```

`bootstrap.py` verifies the submodule status and downloads the base SoundFont (`FluidR3Mono_GM.sf3`). `prepare.py` verifies the submodule pin and stages the engine's hash-pinned asset manifest (348 files, 28,404,538 bytes) into `.runtime/assets`.

## Development builds

Once configured (via the build scripts or `xmake f`), build targets directly.

Because host targets (`engrave_mcp`, `engrave_mcp_test`, `host_api_test`, `registry_dump`, `registry_check`, etc.) are declared with `set_default(false)` in `engrave_mcp/xmake.lua`, running `pixi run build` (which invokes `xmake build -y`) without arguments builds only default engine targets and will not compile the host server or its tools. Specify host targets explicitly or use the native build script:

```powershell
# Build host binaries and tools directly
pixi run build engrave_mcp engrave_mcp_test host_api_test registry_dump registry_check

# Build specific engine test targets
pixi run build engrave_mnx_import_test
```

## Windows native build

```powershell
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
```

The script enters the Visual Studio developer shell, verifies submodule status, validates xmake inputs, configures `build/xmake-windows`, and builds default host targets: `engrave_mcp`, `engrave_mcp_test`, `registry_dump`, `registry_check`, and `host_api_test`.

To build additional host test targets:

```powershell
xmake build live_export_test staging_test
# On Linux:
# xmake build publication_test
```

### Running the development binary

Development builds link against Pixi DLLs, which must be on `PATH`:

```powershell
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"
build/xmake-windows/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

## Linux build

```bash
bash scripts/build-native.sh 8 build/xmake-linux "$PWD/.pixi/envs/default"
```

The third argument specifies the Cairo prefix (`<prefix>/include/cairo`, `<prefix>/lib`). The output binary is `build/xmake-linux/engrave-mcp`.

## Test binaries

Specific lanes and tests use standalone binaries not built by default:

```powershell
xmake build live_export_test staging_test
```

- `live_export_test` — direct engine live export oracle
- `staging_test` — staging folder permissions and DACL tests
- `publication_test` — Linux fsync ordering and failure injection tests

The golden and semantic check suites also require the engine's `semantic_dump` tool:

```powershell
xmake f --engrave_tests=y
xmake build semantic_dump
```

## Packaging

Windows:

```powershell
pixi run python scripts/package_windows.py --no-qt `
  --binary build/xmake-windows/engrave-mcp.exe --output artifacts/windows-next
```

Linux:

```bash
pixi run --locked python scripts/package_linux.py \
  --binary build/xmake-linux/engrave-mcp --output artifacts/linux-next
```

The packagers assemble runtime DLL/ELF dependencies, asset folders, SoundFonts, and licenses into a self-contained folder. A packaged binary automatically resolves its adjacent `assets/` directory without needing `--resources`.

## Tests and lanes

Run the smoke and transport tests against the compiled binary (pointing `ENGRAVE_MCP_RESOURCES` at `.runtime/assets`):

```powershell
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
pixi run python tests/integration/test_mcp_smoke.py
pixi run python tests/integration/test_transport.py build/xmake-windows/engrave-mcp.exe
```

The acceptance runner and lane catalog are detailed in [validation.md](validation.md). Pass `--build-dir build/xmake-windows` (or the corresponding Linux build directory) when executing lanes.

## Layout rules

- Host sources in `engrave_mcp/` include only public engine headers from `engrave/...`, validated by `scripts/check_native_includes.py`.
- `engrave_mcp/tools.json` is generated by `registry_dump` and validated by `registry_check`; do not edit manually.
- `build/`, `.pixi/`, `.runtime/`, `artifacts/`, and `notes/` are gitignored.

## Code formatting and linting

```powershell
pixi run format -- .
pixi run format-check
pixi run lint
pixi run format -- --engine vendor/engrave
pixi run format-check -- --engine
pixi run lint -- --engine
pixi run setup-hooks
```

Formatting and linting rules follow `vendor/engrave/.clang-tidy` and `.clang-format`.

Full engine naming lint is required for all engine changes. Because the engine lint tool only selects translation units present in `compile_commands.json`, the compilation database must be regenerated after enabling engine tests (`--engrave_tests=y`), otherwise test and adapter translation units are omitted from linting:

```powershell
xmake f --engrave_tests=y
xmake project -k compile_commands
pixi run lint -- --engine
```

Engine lint runs in parallel across compiled translation units and fails on naming diagnostics or empty selections. The installed pre-commit hook verifies staged changes prior to commit; full naming lint is verified at acceptance.
