# Build and test

The host is built with **xmake**; **Pixi** provides the pinned toolchain and native libraries. The engine build lives in the submodule (`vendor/engrave/xmake/engrave.lua`) and is included by the host's `xmake.lua`.

## Prerequisites

Windows:

- Git and [Pixi](https://pixi.sh/).
- Visual Studio 2022 with the x64 C++ toolchain (MSVC 14.51+).
- xmake 3.1.1+ (available on `PATH`).

Linux (validated on Linux x86_64, GCC 15.3, C++23):

- Git and Pixi.
- Cairo and zlib come from the Pixi environment: the engine build requires them as `pixi::cairo` and `pixi::zlib` (an xmake package manager in `vendor/engrave/xmake/modules`), so no prefix option is needed.
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

`bootstrap.py` verifies the submodule status and downloads the base SoundFont (`FluidR3Mono_GM.sf3`). `prepare.py` verifies the submodule pin and stages the engine's hash-pinned asset manifest (348 files, 28,405,734 bytes) into `.runtime/assets`.

## Development builds

Use `scripts/xbuild.py` for independent profiles. Outputs default to `build/release`, `build/debug`, `build/releasedbg`, and `build/asan`; configuration lives under `.xmake/profiles/<platform>-<arch>-<profile>/`. All profiles share `build/.packages`. Options before the action belong to the wrapper; subsequent arguments go to xmake. `--build-directory` overrides the profile output.

```powershell
pixi run --locked python scripts/xbuild.py --profile release configure --engrave_tests=y
pixi run --locked python scripts/xbuild.py --profile release build -j 12 engrave_mcp engrave_mcp_test host_api_test registry_dump registry_check
pixi run --locked python scripts/xbuild.py --profile debug build -j 12 engrave_mcp
pixi run --locked python scripts/xbuild.py --profile releasedbg compdb
pixi run --locked python scripts/xbuild.py --profile asan build -j 12 engrave_mcp
```

The wrapper configures automatically on first use; `configure` changes options and `--reconfigure` forces configuration. Host targets use `set_default(false)`, so name them explicitly. Native build scripts retain `build/xmake-windows` and `build/xmake-linux` for release; other profiles default to `build/<profile>`. Windows accepts `-Profile debug`; Linux accepts the profile as its fourth positional argument.

Host rules live in `xmake/rules/*.lua`. The engine entry point `vendor/engrave/xmake/engrave.lua` loads split `environment.lua`, `packages.lua`, `generated.lua`, `tests.lua`, `rules/`, and `targets/` modules.

## Windows native build

```powershell
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 12
```

The script enters the Visual Studio developer shell, verifies submodule status, validates xmake inputs, configures `build/xmake-windows`, and builds default host targets: `engrave_mcp`, `engrave_mcp_test`, `registry_dump`, `registry_check`, and `host_api_test`.

To build additional host test targets:

```powershell
pixi run --locked python scripts/xbuild.py --build-directory build/xmake-windows build -j 12 live_export_test staging_test
# On Linux:
# pixi run --locked python scripts/xbuild.py --build-directory build/xmake-linux build -j 8 publication_test
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
bash scripts/build-native.sh 8 build/xmake-linux
```

An optional third argument selects another engine checkout. The output binary is `build/xmake-linux/engrave-mcp`; it finds the Pixi cairo through its run path.

## Test binaries

Specific lanes and tests use standalone binaries not built by default:

```powershell
pixi run --locked python scripts/xbuild.py --build-directory build/xmake-windows build -j 12 live_export_test staging_test
```

- `live_export_test` — direct engine live export oracle
- `staging_test` — staging folder permissions and DACL tests
- `publication_test` — Linux fsync ordering and failure injection tests

The golden and semantic check suites also require the engine's `semantic_dump` tool:

```powershell
pixi run --locked python scripts/xbuild.py configure --engrave_tests=y
pixi run --locked python scripts/xbuild.py build -j 12 semantic_dump
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

The acceptance runner and lane catalog are detailed in [validation.md](validation.md). Use `-j 4` for parallel functional lanes. The runner discovers `build/release` first, then the platform native-script directory; explicit `--build-dir` wins. Timing and mutation lanes run exclusively after functional lanes drain.

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

`scripts/format.py --backend {auto,clice,clang-format} -j N` selects the C++ formatter. Auto uses clice with a usable binary, root `clice.toml`, and compilation database, otherwise clang-format. `scripts/lint.py --backend {auto,clice,clang-tidy} -j N` selects the C++ linter: auto keeps host checks scoped through clang-tidy; explicit clice checks the entire database, including the engine. Python uses Ruff, Markdown/JSON/YAML use Prettier, and TOML uses Taplo regardless of the C++ backend.

The root `clice.toml` maps profile databases (`build/release`, `build/debug`, `build/releasedbg`, `build/asan`) and excludes dependencies and generated trees. Generate a profile database with `scripts/xbuild.py --profile <profile> compdb`. Keep one root clice project for host and engine.

Formatting and linting rules follow `vendor/engrave/.clang-tidy` and `.clang-format`.

Full engine naming lint is required for all engine changes. Because the engine lint tool only selects translation units present in the compilation database, regenerate the profile database after enabling engine tests (`--engrave_tests=y`), otherwise test and adapter translation units are omitted from linting:

```powershell
pixi run --locked python scripts/xbuild.py configure --engrave_tests=y
pixi run --locked python scripts/xbuild.py compdb
pixi run lint -- --engine
```

Both `scripts/lint.py` and `vendor/engrave/tools/lint.py` automatically select the newest `compile_commands.json` across `build/<profile>/` and the repository root; explicit clice uses the profile mappings in `clice.toml`.

Engine lint runs in parallel across compiled translation units and fails on naming diagnostics or empty selections. The installed pre-commit hook verifies staged changes prior to commit; full naming lint is verified at acceptance.
