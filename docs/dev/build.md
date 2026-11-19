# Build and test

The host and engine are built with **Bazel** (pinned in `.bazelversion`) and a hermetic Clang 23 / `libc++` (**xclang**) C++23 toolchain on both Windows and Linux. **Pixi** provides `bazelisk`, Python/lint tools, and the pinned `cairo` and `zlib` native libraries. The engine build lives in the submodule (`vendor/engrave/MODULE.bazel`, `vendor/engrave/BUILD.bazel`) and is consumed by the host's `MODULE.bazel` via `local_path_override`.

## Prerequisites

Windows:

- Git and [Pixi](https://pixi.sh/).
- Optional: Windows SDK (`mt.exe`) when running the `windows_manifest` acceptance lane. No Visual Studio installation is required to compile, link, or package `engrave-mcp`.

Linux (validated on Ubuntu 24.04.5 x86_64 via `ssh user@<linux-host>`):

- Git and Pixi (`bazel` / `bazelisk` via Pixi or `~/engrave/tools/bazel/bin`).
- Cairo and zlib come from the Pixi environment (`@pixi_deps//:cairo` and `@pixi_deps//:zlib` in `vendor/engrave/bazel/deps.bzl`).
- WSL is not supported.

JSON: the host uses **glaze 9.1** (`@glaze` in `MODULE.bazel`) for its JSON codecs and wire writer.

## Initial checkout and setup

Clone with `--recursive`, or initialize the submodule before running `pixi install` (which installs `vendor/engrave` in editable mode):

```powershell
git submodule update --init
pixi install --locked
pixi run python scripts/bootstrap.py --soundfont
pixi run python scripts/prepare.py
```

`bootstrap.py` verifies the submodule status and downloads the base SoundFont (`FluidR3Mono_GM.sf3`). `prepare.py` verifies a clean, pinned `vendor/engrave` checkout (refusing a modified engine) and stages its hash-pinned asset manifest into `.runtime/assets`. `scripts/check_build_inputs.py` verifies `.bazelversion` and `pixi.lock` native package parity (`cairo`, `zlib`, `libzlib`) between the host and `vendor/engrave`.

## Development builds

Use `scripts/bbuild.py` (or `pixi run build`) for development builds across `release`, `debug`, `releasedbg`, and `asan` profiles. Outputs are staged to `build/<profile>`, external archives are cached in `build/.bazel_repo_cache`, and Bazel action outputs (including relocatable Clang binary precompiled headers for `engraving`) are shared across builds in `build/.bazel_disk_cache`. `--build-directory` overrides the staged output path, and `--no-pch` disables precompiled headers.

```powershell
pixi run --locked python scripts/bbuild.py --profile release -j 12 build engrave_mcp engrave_mcp_test host_api_test registry_dump registry_check
pixi run --locked python scripts/bbuild.py --profile debug -j 12 build engrave_mcp
pixi run --locked python scripts/bbuild.py --profile release compdb
pixi run --locked python scripts/bbuild.py --profile asan -j 12 build engrave_mcp
```

With no explicit target arguments, `bbuild.py build` builds `all_binaries` (all host binaries plus `@engrave//:engine_tests`). `registry_check` builds `registry_dump` and `registry_test` and runs `tests/integration/test_tool_schema.py` against `engrave_mcp/tools.json`.

## Clean builds at the pinned engine

Use `scripts/build-native.ps1` (Windows) and `scripts/build-native.sh` (Linux) only for a clean build at the pinned engine: both run `git submodule update --init` (which resets `vendor/engrave`), `scripts/prepare.py` (which refuses a modified engine), and `scripts/check_build_inputs.py --check`, then build into `build/<profile>` (`build/release` by default):

```powershell
# Windows (-Profile debug selects another profile)
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 12
```

```bash
# Linux (positional args: [jobs] [build_dir] [engine_source] [profile])
bash scripts/build-native.sh 8
```

### Running the development binary

On Windows, development builds load `cairo.dll` and `zlib.dll` from the Pixi environment, which must be on `PATH` (on Linux, `@pixi_deps` embeds the Pixi `lib/` RPATH directly):

```powershell
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"
build/release/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

## Test binaries

`tests/run_lanes.py` builds the targets selected lanes need before running them (unless `--no-build` is passed). When invoking standalone test scripts directly, build any needed targets with `scripts/bbuild.py`:

```powershell
pixi run --locked python scripts/bbuild.py --profile release -j 12 build live_export_test staging_test semantic_dump
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
- `build/`, `bazel-*`, `.pixi/`, `.runtime/`, `artifacts/`, and `notes/` are gitignored.

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

The root `clice.toml` maps profile databases (`build/release`, `build/debug`, `build/releasedbg`, `build/asan`) and excludes dependencies and generated trees. Generate a profile database with `scripts/bbuild.py --profile <profile> compdb` (via native `bazel aquery` with inline `xclang` toolchain flags). Keep one root clice project for host and engine.

Host formatting and linting follow the root `.clang-format` and `.clang-tidy`; engine checks via `--engine` follow `vendor/engrave/.clang-format` and `vendor/engrave/.clang-tidy`.

Per step, lint the touched files (`pixi run lint -- <paths>`, or `pixi run lint -- --engine <paths>` for engine files; regenerate `compdb` if translation units changed). Full-tree engine naming lint (`pixi run lint -- --engine`) runs at milestones.

Both `scripts/lint.py` and `vendor/engrave/tools/lint.py` automatically select the newest `compile_commands.json` across `build/<profile>/` and the repository root; explicit clice uses the profile mappings in `clice.toml`.

Engine lint runs in parallel across compiled translation units and fails on naming diagnostics or empty selections. The host pre-commit hook reformats staged files on disk and rejects the commit if anything changed so you can review and stage the result; the engine pre-commit hook checks staged engine C++ blobs without modifying the working tree.
