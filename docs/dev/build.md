# Build and test

The host is built with **xmake**; **Pixi** provides the pinned toolchain and native libraries. The engine build lives in the submodule (`vendor/engrave/xmake/engrave.lua`) and is included by the host's `xmake.lua`.

## Prerequisites

Windows:

- Git and [Pixi](https://pixi.sh/).
- Visual Studio 2026 (or Build Tools) with the x64 C++ toolchain (MSVC 14.51+).
- xmake 3.1.1+ (available on `PATH`).

Linux (validated on Ubuntu 24.04.5 x86_64 via `ssh user@<linux-host>`, conda GCC 15.3; Pixi pins Clang/clang-tools 23.1.4, C++23):

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

`bootstrap.py` verifies the submodule status and downloads the base SoundFont (`FluidR3Mono_GM.sf3`). `prepare.py` verifies a clean, pinned `vendor/engrave` checkout (refusing a modified engine) and stages its hash-pinned asset manifest into `.runtime/assets`.

## Development builds

Use `scripts/xbuild.py` for development builds across `release`, `debug`, `releasedbg`, and `asan` profiles. Outputs go to `build/<profile>`, configuration to `.xmake/profiles/<platform>-<arch>-<profile>/`, packages to `build/.packages`, and the compile cache is shared across all build directories in `build/.build_cache`. Every profile configures with `--engrave_tests=y` by default; test targets build only when named. Options before the action belong to the wrapper; subsequent arguments go to xmake. `--build-directory` overrides the profile output.

```powershell
pixi run --locked python scripts/xbuild.py --profile release build -j 12 engrave_mcp engrave_mcp_test host_api_test registry_dump registry_check
pixi run --locked python scripts/xbuild.py --profile debug build -j 12 engrave_mcp
pixi run --locked python scripts/xbuild.py --profile releasedbg compdb
pixi run --locked python scripts/xbuild.py --profile asan build -j 12 engrave_mcp
```

`xbuild.py` configures automatically on first use and passes `-P` itself (`-P .` matters only when running raw `xmake` in nested checkouts); `configure` changes options and `--reconfigure` forces configuration. Host and test targets use `set_default(false)`, so name them explicitly.

### C++ modules: postponed

The build uses headers only. A C++23 module facade that wrapped the public headers was tried (fork `95199efc2b`) and removed: xmake turns the compile cache off for module targets, the facade made incremental builds slower, and wrapping headers leaves the engine as it was. Adopting modules would need the engine restructured for them; that is postponed while header builds stay fast.

Host rules live in `xmake/rules/*.lua`. The engine entry point `vendor/engrave/xmake/engrave.lua` loads split `environment.lua`, `packages.lua`, `generated.lua`, `tests.lua`, `rules/`, and `targets/` modules.

## Clean builds at the pinned engine

Use `scripts/build-native.ps1` (Windows) and `scripts/build-native.sh` (Linux) only for a clean build at the pinned engine: both run `git submodule update --init` (which resets `vendor/engrave`) and `scripts/prepare.py` (which refuses a modified engine), then configure and build into `build/<profile>` (`build/release` by default):

```powershell
# Windows (-Profile debug selects another profile)
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 12
```

```bash
# Linux (positional args: [jobs] [build_dir] [engine_source] [profile])
bash scripts/build-native.sh 8
```

The Windows script enters the Visual Studio developer shell and builds `engrave_mcp`, `engrave_mcp_test`, `registry_dump`, `registry_check`, and `host_api_test`. On Linux, `build-native.sh` selects the Pixi GCC toolchain and writes the runtime run path into the binaries.

### Running the development binary

On Windows, development builds link against Pixi DLLs, which must be on `PATH`:

```powershell
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"
build/release/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

## Test binaries

`tests/run_lanes.py` builds the targets selected lanes need before running them (unless `--no-build` is passed). When invoking standalone test scripts directly, build any needed targets with `scripts/xbuild.py`:

```powershell
pixi run --locked python scripts/xbuild.py --profile release build -j 12 live_export_test staging_test semantic_dump
# On Linux, add publication_test; for the full engine test closure, build engine_tests.
```

- `live_export_test` — direct engine live export oracle
- `staging_test` — staging folder permissions and DACL tests
- `publication_test` — Linux fsync ordering and failure injection tests
- `semantic_dump` — engine dump tool used by golden and semantic checks

## Packaging

Windows:

```powershell
pixi run python scripts/package_windows.py --no-qt `
  --binary build/release/engrave-mcp.exe --output artifacts/windows-next
```

Linux:

```bash
pixi run --locked python scripts/package_linux.py \
  --binary build/release/engrave-mcp --output artifacts/linux-next
```

The packagers assemble runtime DLL/ELF dependencies, asset folders, SoundFonts, and licenses into a self-contained folder. A packaged binary automatically resolves its adjacent `assets/` directory without needing `--resources`.

## Tests and lanes

Run the smoke and transport tests against the compiled binary (pointing `ENGRAVE_MCP_RESOURCES` at `.runtime/assets`):

```powershell
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
pixi run python tests/integration/test_mcp_smoke.py
pixi run python tests/integration/test_transport.py build/release/engrave-mcp.exe
```

The acceptance runner and lane catalog are detailed in [validation.md](validation.md). `tests/run_lanes.py` builds the targets the selected lanes need, uses `build/<profile>` (`--profile release` by default; explicit `--build-dir` overrides), and writes outputs under `notes/runs/<name>`.

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

Host formatting and linting follow the root `.clang-format` and `.clang-tidy`; engine checks via `--engine` follow `vendor/engrave/.clang-format` and `vendor/engrave/.clang-tidy`.

Per step, lint the touched files (`pixi run lint -- <paths>`, or `pixi run lint -- --engine <paths>` for engine files; regenerate `compdb` if translation units changed). Full-tree engine naming lint (`pixi run lint -- --engine`) runs at milestones (see [defects.md](defects.md) for pre-existing baseline findings).

Both `scripts/lint.py` and `vendor/engrave/tools/lint.py` automatically select the newest `compile_commands.json` across `build/<profile>/` and the repository root; explicit clice uses the profile mappings in `clice.toml`.

Engine lint runs in parallel across compiled translation units and fails on naming diagnostics or empty selections. The host pre-commit hook reformats staged files on disk and rejects the commit if anything changed so you can review and stage the result; the engine pre-commit hook checks staged engine C++ blobs without modifying the working tree.
