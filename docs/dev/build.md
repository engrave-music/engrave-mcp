# Build and Test

The host and engine are built with **Bazel** (pinned in `.bazelversion`) and a
hermetic Clang 23 / `libc++` (**xclang**) C++23 toolchain on both Windows and
Linux. **Pixi** provides `bazelisk`, Python/lint tools, and the pinned `cairo`,
`freetype`, and `zlib` native libraries. The engine build lives in `vendor/engrave`
(`MODULE.bazel`, `BUILD.bazel`) and is consumed by the host's `MODULE.bazel` via
`local_path_override`.

## Prerequisites

Windows:

- Git and [Pixi](https://pixi.sh/).
- Optional: Windows SDK (`mt.exe`) when running the `windows_manifest`
  acceptance lane. No Visual Studio installation is required to compile, link,
  or package `engrave-mcp`.

Linux (validated on Ubuntu 24.04.5 x86_64):

- Git and Pixi (`bazel` / `bazelisk` via Pixi).
- Cairo, FreeType, and zlib come from the Pixi environment (`@pixi_deps//:cairo`,
  `@pixi_deps//:freetype`, and `@pixi_deps//:zlib` in `vendor/engrave/bazel/deps.bzl`).
- WSL is not supported.

JSON: the host uses **glaze 9.1** (`@glaze` in `MODULE.bazel`) for its JSON
codecs and wire writer.

## Initial Checkout and Setup

Clone with `--recursive`, or initialize the submodule before running
`pixi install` (which installs `vendor/engrave` in editable mode):

```powershell
git submodule update --init
pixi install --locked
pixi run python scripts/bootstrap.py --soundfont
pixi run python scripts/prepare.py
```

- `scripts/bootstrap.py` verifies the submodule status and downloads the base
  SoundFont (`FluidR3Mono_GM.sf3`).
- `scripts/prepare.py` verifies a clean, pinned `vendor/engrave` checkout
  (refusing a modified engine) and stages its asset manifest from committed Git
  blobs into `.runtime/assets`.
- `scripts/check_build_inputs.py` verifies `.bazelversion` and `pixi.lock`
  native package parity (`cairo`, `freetype`, `libfreetype`, `libfreetype6`,
  `zlib`, `libzlib`) between the host and `vendor/engrave`.

## Development Builds

Use `scripts/bbuild.py` (or `pixi run build`) across `release`, `debug`,
`releasedbg`, and `sanitize` profiles. Outputs are staged to `build/<profile>`,
external archives are cached in `build/.bazel_repo_cache`, and Bazel action
outputs (including relocatable Clang binary precompiled headers) are shared
across builds in `build/.bazel_disk_cache`. `--build-directory` overrides the
staged output path, and `--no-pch` disables precompiled headers.

```powershell
pixi run --locked python scripts/bbuild.py --profile release -j 12 build engrave_mcp engrave_mcp_test host_api_test registry_dump registry_check
pixi run --locked python scripts/bbuild.py --profile debug -j 12 build engrave_mcp
pixi run --locked python scripts/bbuild.py --profile release compdb
pixi run --locked python scripts/bbuild.py --profile sanitize -j 12 build engrave_mcp
```

The `release` profile uses xclang's `-O2`, LLD section garbage collection, and
safe identical code folding on Windows and Linux. It keeps ordinary
development builds free of link-time optimization.

The `sanitize` profile enables AddressSanitizer (ASan), LeakSanitizer (LSan),
and fatal UndefinedBehaviorSanitizer (UBSan) in one build via xclang's
`--features=asan --features=ubsan`. Only the third-party LAME and libFLAC C
codec targets disable UBSan. They retain ASan instrumentation.

With no target arguments, `bbuild.py build` builds `all_binaries` (all host
binaries plus `@engrave//:engine_tests`). `registry_check` builds
`registry_dump` and `registry_test` and runs
`tests/integration/test_tool_schema.py` against `engrave_mcp/tools.json`.

### Persistent Bazel JVM for Agent Sessions (`bazel_server_mcp.py`)

On Windows, agent command runners typically execute each shell call inside a Job
Object with `KILL_ON_JOB_CLOSE`, which terminates any Bazel JVM spawned inside
that call. `scripts/bazel_server_mcp.py` (and
`vendor/engrave/tools/bazel_server_mcp.py` for standalone engine checkouts) is a
zero-dependency stdio MCP server exposing `bazel_server_start` and
`bazel_server_status` over a configurable project map (defaulting to `host` +
`engine`, extensible via `--project NAME=PATH` or `--config projects.json`).
Passing `--warm host` warms the Bazel JVM in the background immediately on IDE
startup and re-warms it automatically if Bazel ever exits after its idle timeout
(`--keepalive-secs`, default 60s). Because the IDE/agent host keeps the MCP
process alive outside per-command Job Objects, subsequent `scripts/bbuild.py`,
`tests/run_lanes.py`, `tests/engine_gate.py`, and `bazelisk` commands
automatically connect to the same warm Bazel JVM even if the agent never invokes
an MCP tool:

```json
{
  "mcpServers": {
    "bazel-server": {
      "command": "C:/path/to/engrave-mcp/.pixi/envs/default/python.exe",
      "args": [
        "C:/path/to/engrave-mcp/scripts/bazel_server_mcp.py",
        "--warm",
        "host"
      ]
    }
  }
}
```

### Precompiled Headers vs. C++23 Modules

Both the engine (`vendor/engrave/bazel/pch/engraving_pch.h`) and Ring 2 host
(`bazel/pch/host_pch.h`) use relocatable Clang binary PCH
(`-fpch-instantiate-templates`, `-Xclang -fno-pch-timestamp`,
`-Xclang -fmodule-file-home-is-cwd`) containing only stable STL and third-party
headers (`@glaze`, `@ngcpp_proxy`, `@unordered_dense` for the host), so edits to
project headers never invalidate the PCH.

A module-first C++23 prototype (`export module music.json` / `music.registry`
with `import std;` via `@xclang//bazel:std` and zero consumer `#include`s) was
benchmarked against raw headers and `host_pch` on the 5-TU Glaze-heavy
`tool_registry` + `registry_dump` + `registry_test` slice (`-c opt`, warm Bazel
server, `xclang 23.1.5.2`):

| Metric                                 | Raw headers        | C++23 modules (`import std;`) | Clang binary PCH (`host_pch`) |
| -------------------------------------- | ------------------ | ----------------------------- | ----------------------------- |
| Windows clean 5-TU                     | 16.61s (CP 13.80s) | 20.18s (CP 17.45s)            | **12.18s (CP 9.20s)**         |
| Windows 1-TU inc (`schema_defs.cpp`)   | 14.15s (CP 11.66s) | 10.56s (CP 8.09s)             | **10.51s (CP 7.69s)**         |
| Windows 1-TU inc (`registry_test.cpp`) | 7.63s (CP 5.22s)   | 5.45s (CP 3.03s)              | **4.51s (CP 1.73s)**          |
| Linux clean 5-TU                       | 6.58s (CP 6.35s)   | 8.69s (CP 8.41s)              | **4.85s (CP 4.63s)**          |
| Linux 1-TU inc (`schema_defs.cpp`)     | 6.33s (CP 6.12s)   | 4.86s (CP 4.61s)              | **4.69s (CP 4.47s)**          |
| Linux 1-TU inc (`registry_test.cpp`)   | 2.76s (CP 2.54s)   | 1.82s (CP 1.58s)              | **1.03s (CP 0.82s)**          |

Across the full Ring 2 host (`//:host_binaries`), `host_pch` cut clean rebuilds
from `33.74s` to `23.79s` on Windows (-29.5%, CP `29.77s` -> `18.57s`) and
`10.78s` to `7.84s` on Linux (-27.3%, CP `9.79s` -> `7.20s`), with one-time cold
PCH builds of `7.27s` (Windows) and `2.60s` (Linux). Beyond beating C++23
modules on every compile metric (no `CppModuleDepsScanning` or serialized `.pcm`
chains), binary PCH preserves full `clang-tidy` compatibility:
`scripts/bbuild.py compdb` strips `-include-pch`, whereas Clang `.pcm` files
embed the exact LLVM commit SHA and are rejected by external `clang-tidy`
binaries whose LLVM revision differs from `xclang`.

## Clean Builds at the Pinned Engine

Use `scripts/build-native.ps1` (Windows) and `scripts/build-native.sh` (Linux)
only for a clean build at the pinned engine: both run
`git submodule update --init` (resetting `vendor/engrave`), `scripts/prepare.py`
(refusing a modified engine), and `scripts/check_build_inputs.py --check`, then
build into `build/<profile>` (`build/release` by default):

```powershell
# Windows (-Profile debug selects another profile)
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 12
```

```bash
# Linux (positional args: [jobs] [build_dir] [engine_source] [profile])
bash scripts/build-native.sh 8
```

### Running the Development Binary

On Windows, development builds load `cairo.dll`, `freetype.dll`, and `zlib.dll`
from the Pixi environment, which must be on `PATH` (on Linux, `@pixi_deps`
embeds the Pixi `lib/` RPATH directly):

```powershell
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"
build/release/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

## Standalone Test Binaries

`tests/run_lanes.py` builds the targets selected lanes need automatically
(unless `--no-build` is passed). When invoking standalone test scripts directly,
build any needed targets with `scripts/bbuild.py`:

```powershell
pixi run --locked python scripts/bbuild.py --profile release -j 12 build live_export_test staging_test semantic_dump
# On Linux, add publication_test; for the full engine test closure, build engine_tests.
```

- `live_export_test`: direct engine live export oracle
- `staging_test`: staging folder permissions and DACL tests
- `publication_test`: Linux fsync ordering and failure injection tests
- `semantic_dump`: engine dump tool used by golden and semantic checks

## Packaging

Windows:

```powershell
pixi run python scripts/package_windows.py `
  --binary build/release/engrave-mcp.exe --output artifacts/windows-next
```

Linux:

```bash
pixi run --locked python scripts/package_linux.py \
  --binary build/release/engrave-mcp --output artifacts/linux-next
```

The packagers strip the copied packaged executable by default (pass `--no-strip`
to keep symbols), leave the staged `build/<profile>` binary untouched, and
record `unstripped_executable_sha256` in `manifest.json`. They also assemble
runtime DLL/ELF dependencies, asset folders, SoundFonts,
and licenses into a self-contained folder. A packaged binary automatically
resolves its adjacent `assets/` directory without needing `--resources`.

## Smoke Tests and Lanes

Run the smoke and transport tests against a built binary:

```powershell
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
pixi run python tests/integration/test_mcp_smoke.py
pixi run python tests/integration/test_transport.py build/release/engrave-mcp.exe
```

For the full acceptance runner, engine gate, and CI coverage, see
[validation.md](validation.md).

## Layout Rules

- Host sources in `engrave_mcp/` include only public engine headers under
  `engrave/...`, enforced by `scripts/check_native_includes.py`.
- `engrave_mcp/tools.json` is generated by `registry_dump` and validated by
  `registry_check`; do not edit it manually.
- `build/`, `bazel-*`, `.pixi/`, `.runtime/`, `artifacts/`, `notes/`,
  `.worktrees/`, `AGENTS.local.md`, and `docs/do_not_commit/` are gitignored.

## Code Formatting and Linting

```powershell
pixi run format -- .
pixi run format-check
pixi run lint
pixi run format -- --engine vendor/engrave
pixi run format-check -- --engine
pixi run lint -- --engine
pixi run setup-hooks
```

- **Backends**: `scripts/format.py --backend {auto,clice,clang-format} -j N`
  uses `clice` when a usable binary, root `clice.toml`, and compilation database
  are present, otherwise `clang-format`.
  `scripts/lint.py --backend {auto,clice,clang-tidy} -j N` keeps `auto` host
  checks scoped through `clang-tidy`; explicit `clice` checks the entire
  database including the engine. Python uses Ruff, Markdown/JSON/YAML use
  Prettier, and TOML uses Taplo regardless of the C++ backend.
- **Compilation databases**: Root `clice.toml` maps profile databases
  (`build/release`, `build/debug`, `build/releasedbg`, `build/sanitize`) and
  excludes vendored third-party code and `.pixi/` headers from lint and format.
  Generate a profile database with `scripts/bbuild.py --profile <profile> compdb`
  (via `bazel aquery` with inline `xclang` flags). Both `scripts/lint.py` and `vendor/engrave/tools/lint.py`
  auto-select the newest `compile_commands.json` across `build/<profile>/` and
  the repo root. Keep one root `clice` project for host and engine.
- **Config scope & cadence**: Host checks follow root `.clang-format` /
  `.clang-tidy`; `--engine` checks follow `vendor/engrave/.clang-format` /
  `vendor/engrave/.clang-tidy`. Lint touched files per step
  (`pixi run lint -- <paths>` or `pixi run lint -- --engine <paths>`,
  regenerating `compdb` if translation units changed); run full-tree engine
  naming lint (`pixi run lint -- --engine`) at milestones.
- **Pre-commit hooks**: Engine lint runs in parallel across compiled translation
  units and fails on naming diagnostics or empty selections. The host pre-commit
  hook reformats staged files on disk and rejects the commit if anything changed
  so you can review and stage the result; the engine hook checks staged engine
  C++ blobs without modifying the working tree.
