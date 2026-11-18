# Validation

Validation is tiered and execution-based:

- **Per step**: build, run the tests the change can affect, and run `tests/engine_gate.py --quick` when engine code changed.
- **At milestones and engine bumps**: run the full engine gate (`tests/engine_gate.py` without `--quick`), the default host lanes (`tests/run_lanes.py`), and validate on the Linux lab host (`ssh user@<linux-host>`).
- State which claims are **executed** and which are **source-reviewed**; never conflate them.

Run outputs (`--output` for lanes and the gate) go under `notes/runs/<name>` (gitignored), never under `.runtime/`, which holds runtime assets only.

## Building for tests

`tests/run_lanes.py` builds the targets the selected lanes need before running them (`--no-build` skips the build; `--profile` selects `build/<profile>`, defaulting to `release`). All `scripts/xbuild.py` profiles configure with `--engrave_tests=y` and share `build/.packages` and `build/.build_cache`; see [build.md](build.md) for profile builds and standalone test targets.

### Nested checkouts and xmake `-P .`

`scripts/xbuild.py` passes `-P` itself. When running raw `xmake` commands in nested checkouts, submodules (such as `vendor/engrave`), or worktrees under `.worktrees/`, pass `-P .` so `xmake` operates on the local checkout rather than an enclosing parent repository.

## Test Layout

The repository organizes tests into distinct directories with strict role separation:

- `tests/integration/`: Holds standalone integration test scripts. These are plain executable scripts and are never imported as Python modules. Integration test directories must not contain `__init__.py`.
- `tests/engrave_mcp_testing/`: The single shared helper package for the host codebase. It is installed in editable mode into `.pixi/envs/default` during `pixi install`.
- `tests/benchmarks/`: Holds performance, latency, memory RSS, and writer precision benchmarks.
- `tests/data/`: Holds host-specific test data, manifests, and golden references.

### Layout lint guard

The boundary between executable test scripts and shared helper modules is enforced by the layout lint guard (`audit_test_layout` in `tests/engrave_mcp_testing/layout.py`), which runs during `pixi run lint`. It verifies that:

1. No `__init__.py` exists inside any `tests/integration` directory in either repository.
2. No Python script in `tests/`, `scripts/`, `tools/`, `engrave/tests/`, or any `src/**/tests/` imports any module starting with `test_`. Shared helpers must be imported from the testing packages (`engrave_mcp_testing` in host, `engrave_testing` in the engine fork).

## Running a Single Test Directly

Individual integration tests can be executed directly using the environment Python interpreter without setting `PYTHONPATH`:

```powershell
.pixi\envs\default\python.exe tests/integration/test_mcp_smoke.py
```

On Linux:

```bash
.pixi/envs/default/bin/python tests/integration/test_mcp_smoke.py
```

Because `pixi install` installs `engrave-host-testing` (`tests/engrave_mcp_testing`) and `engrave-testing` (`vendor/engrave/tests/engrave_testing`) in editable mode into `.pixi/envs/default`, all test helpers are importable directly by the environment interpreter without requiring `PYTHONPATH`.

### Windows manifest verification

The default `windows_manifest` lane runs `tests/integration/test_windows_manifest.py --build-dir <build-dir>`, which extracts the embedded manifest of `engrave-mcp.exe`, `engrave-mcp-test.exe` and `engrave_output_test.exe` with the Windows SDK `mt.exe` and requires `SegmentHeap` and UTF-8 under `application/windowsSettings`, plus `asInvoker`. Windows ignores these settings anywhere else, so a misplaced element fails the check. Other executables can be passed as arguments; off Windows the lane skips.

## Running Lanes

Run the unified test runner:

```powershell
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"
.pixi\envs\default\python.exe tests/run_lanes.py `
  --engine-source vendor/engrave `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output notes/runs/lanes -j 4
```

`--profile` selects `build/<profile>` (default: `release`); `--build-dir` overrides it, and `--no-build` skips building the targets the selected lanes need. Pass `--list-lanes` (no runtime/asset paths required) to print the catalogue, parallel vs. exclusive scheduling, and dependencies.

Parallel lanes run in a thread pool (`-j` / `--jobs`, default half the CPU count bounded to 2–8; `-j 1` runs sequentially). Each lane gets its own `<output>/<lane>/cwd` and `tmp`, and writes `<output>/<lane>.log`; `semantic_check` waits for `golden` or requires a prior golden capture in `<output>/golden/actual`. After parallel work drains, exclusive lanes (default: `live_export`, `live_audio`, `archives`, `async_session`, `engine_audio`; opt-in: `document_stress`, `live_dense_cancel`, `live_queued_cancel`, `transport_wakeup`, `xmake_incremental`) run one at a time so concurrent runs do not skew latency measurements or share working state; run without unrelated builds or benchmarks when measuring timing.

Use `--only lane1,lane2` to run a subset (including opt-in lanes). The runner verifies line endings, native include boundaries, and required binaries before execution.

- **Opt-in lanes** (`--only`): excluded from default runs because they require extra inputs (`native_470_oracle`), test rare stress/cancellation limits (`output_schema_boundaries`, `document_stress`, `live_dense_cancel`, `live_queued_cancel`, `native_470_conversion_cancel`, `native_470_serialization_cancel`, `transport_wakeup`), or mutate source files and rebuild (`xmake_incremental`, which must run in an isolated `.worktrees/` clone).
- **Focused default lanes**: `native_includes` checks public header boundaries, `native_inputs` checks malformed input handling, `windows_manifest` checks Windows Segment Heap / UTF-8 / `asInvoker` manifests, `live_export` runs publication/cancellation/UTF-8 path checks alongside `live_export_test`, and `archives` tests archive export against `archive_test` and `engrave-mcp`. Run `--list-lanes` for the full list.

### Windows Output Roots and Path Limits

On Windows, standard `MAX_PATH` limitations (260 characters) can cause filesystem IO failures during staging and atomic publication when creating nested directories (such as `.engrave-stage-*` and `report-delivery/fault-*`). Always specify a short output root for `--output` (such as `notes/runs/w` inside the checkout).

### Running the Native 4.70 Oracle Lane

The oracle test runs through `tests/run_lanes.py --only native_470_oracle` or directly via `tests/integration/test_native_470_oracle.py`, taking `--native-470-oracle-archive`, `--native-470-oracle-executable`, `--native-470-oracle-assets`, and one mode (`--native-470-baseline`, `--native-470-writer-results <dir>`, or `--native-470-producer` with `--host-binary` and `--host-sha256`):

```bash
.pixi/envs/default/bin/python tests/run_lanes.py \
  --engine-source vendor/engrave \
  --assets .runtime/assets \
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 \
  --output notes/runs/oracle \
  --only native_470_oracle \
  --native-470-oracle-archive /path/to/musescore-4.7.6-oracle.tar.gz \
  --native-470-oracle-executable /path/to/semantic_dump \
  --native-470-oracle-assets /path/to/oracle_assets \
  --native-470-baseline
```

When invoking `tests/integration/test_native_470_oracle.py` directly, pass `--oracle-archive`, `--oracle-executable`, `--oracle-assets`, `--output`, and one mode (`--baseline` / `--writer-results` with `--current-binary build/release/semantic_dump` and `--current-assets .runtime/assets`, or `--producer` with `--host-binary`, `--host-resources`, and `--host-sha256`).

### Audio Paired-Capture Requirement

The `engine_audio` lane verifies audio synthesis determinism, MIDI event generation, dynamic range, and cross-platform audio parity across the audio corpus fixtures against reciprocal captures from the other platform (Windows vs Linux).

The cross-platform check requires matching captures from both platforms with verified host and fork git commit provenance. Produce each platform's capture with `tests/integration/test_engine_audio.py --capture-cross-platform-only` and mirror both `<root>/windows` and `<root>/linux` into the lane output's `audio-cross-platform/` directory (or pass `--cross-platform-captures` directly). If the counterpart capture is missing or incomplete, `engine_audio` reports status `pending` (exit code 2); milestone acceptance requires passing both local checks and reciprocal cross-platform captures with matching commit provenance.

## Engine gate

The engine owns its standalone public boundary gate in `vendor/engrave/tests/gate.py`. The host wrapper at `tests/engine_gate.py` defaults to `--pixi-manifest pixi.toml` and shares the host's `build/.build_cache` (`--ccachedir` overrides it):

```powershell
# Windows (PowerShell 7); pass --mutex-wrapper scripts/xmake_mutex.ps1 if sharing xmake's Windows lock
.pixi\envs\default\python.exe tests/engine_gate.py --engine-source vendor/engrave `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output notes/runs/gate --quick
```

```bash
# Linux
.pixi/envs/default/bin/python tests/engine_gate.py --engine-source vendor/engrave \
  --assets .runtime/assets \
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 \
  --output notes/runs/gate --quick
```

`--quick` skips the exclusive timing checks (`output-timing` and `dense-cancellation`) for per-step verification; omit `--quick` at milestones and engine bumps. Use a fresh `--output` directory under `notes/runs/<name>` for each run; existing capture directories are rejected. On Windows, pass `--mutex-wrapper scripts/xmake_mutex.ps1` when an engine build shares the global `Global\engrave-xmake-build` build lock.

The gate builds the fork's `engine_tests` target and executes the API, import, playback, and adapter suites, along with `diagnostics_test`, `io_boundary_test`, installer tests, and asset manifest verification. It compares outputs against pinned goldens using `golden.py` comparators and the platform hash allow-list (`vendor/engrave/tests/data/golden/allow.json`).

## CI

`.github/workflows/ci.yml` (pushes to `main`, pull requests, manual dispatch) is a build-and-unit-test lane, not acceptance. A `checks` job (Ubuntu, Pixi) verifies the submodule pin and xmake inputs, runs `pixi run format-check`, the layout audit and the Python checks that need no binary or asset (`test_test_layout`, `test_native_includes`, `test_engine_gate_wrapper`, `test_cleanup_regressions`, `test_oracle_harness`, `test_rendered_page`, `tests/test_engine_gate.py`, `tests/test_xbuild.py`). A `build-test` job on Windows (MSVC, `windows-latest`) and Linux (the pinned Pixi GCC) builds `engrave_mcp`, `engrave_mcp_test`, `registry_dump`, `registry_check` (which runs the tool-schema test), `host_api_test`, `staging_test` and, on Linux, `publication_test`, then runs `staging_test`, `publication_test`, `host_api_test` and the integration scripts that need only the server binary and the staged engine assets (`version_info`, `open_data`, `native_inputs`, `positions`, `imports_midi`, `imports_guitarpro`, `repeats`, `transport`, plus `windows_manifest` on Windows). `host_api_test` runs with the in-repository `vendor/engrave/tests/data/engine_audio/sine.sf2` as both SoundFonts, so CI downloads none. The fork has its own workflow (`vendor/engrave/.github/workflows/ci.yml`) for the engine unit tests.

Not run in CI, and still manual here: the engine gate and golden comparisons (`tests/engine_gate.py`), the audio corpus and `engine_audio`, every lane that takes `--soundfont` (`live_export`, `live_audio`, `async_session`, `export_options`, `mcp_smoke`, ...), the opt-in lanes, the benchmarks, the Windows-versus-Linux paired captures, and engine naming lint. A green CI run does not replace the milestone validation policy above.

## Known timing and platform limitations

- **Dense-audio cancellation measurements**: The engine test measures request-to-first-cancellation-observation (p95 ≤ 5 ms, max ≤ 10 ms) separately from observation-to-return, including synchronous cleanup (p95 ≤ 50 ms, max ≤ 100 ms). The host integration test retains its request-to-return limits.
- **Windows path length limitations**: On Windows, long output paths approaching or exceeding standard path limits require short temporary roots to avoid filesystem IO failures during staging and atomic publication.

## Benchmarks

- `tests/benchmarks/bench_export_latency.py` — warm/cold render and score export latency, phase breakdown, optional high-priority mode (`ENGRAVE_MCP_BENCH_HIGH_PRIORITY=1`)
- `tests/benchmarks/bench_live_audio.py` — warm/cold audio synthesis latency and RSS
- `tests/benchmarks/bench_document_lifetime.py` — open, cold-to-PDF, and long-run drift
- `tests/benchmarks/bench_export_rss.py` — RSS memory sampling across export cycles
- `tests/benchmarks/bench_audio_distribution.py` — repeated audio synthesis measurement distribution
- `tests/benchmarks/bench_native_writer_precision.py` — writer precision round-trip measurements
- `tests/integration/test_live_dense_cancel.py` — cancellation on a 49,152-note score
- `tests/integration/test_xmake_incremental.py` — incremental build behavior

Benchmark claims must retain raw samples, phase timings, hardware specification, and environment conditions.

## What is not covered

- No leak-clean guarantee: LeakSanitizer findings remain open (see `docs/dev/defects.md`).
- No hard process-memory bound: export quotas enforce logical file output limits, not process RSS limits.
- No hard audio latency p95 guarantee under host contention.
- No macOS or non-x86_64 platform validation.
- Missing opt-in binaries fail closed: running the full suite requires compiling the optional engine test targets.
