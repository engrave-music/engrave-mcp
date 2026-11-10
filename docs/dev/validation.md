# Validation

Acceptance is execution-based: a claim is accepted when the named lane or gate is built and run on native Windows and on the physical Linux host (`ssh <linux-host>`). Source review alone is labeled as such.

## Prerequisites

Build the binaries required by the selected lanes:

```powershell
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
xmake build -P . live_export_test staging_test
```

Lanes require both host test targets and opt-in engine test targets. Configure with `--engrave_tests=y` and build:

```powershell
# Configure with engine tests enabled (-P . targets the local checkout)
xmake f -P . --engrave_tests=y

# Build engine test suite (the engine_tests phony target pulls in semantic_dump, score_edit_test,
# score_audio_test, score_positions_test, excerpts_test, archive_test, assets_test, diagnostics_test,
# io_boundary_test, engraving_playback_tests, etc.) plus the import_export_probe runtime probe
xmake build -P . -y -j 12 engine_tests import_export_probe

# Build required host test targets
xmake build -P . -y -j 12 live_export_test staging_test registry_check host_api_test
# On Linux:
# xmake build -P . -y -j 12 publication_test
```

### Nested checkouts and xmake `-P .`

When running `xmake` commands in nested checkouts, submodules (such as `vendor/engrave`), or separate worktree checkouts under `.worktrees/`, always pass `-P .`. This ensures `xmake` operates on the local checkout rather than searching upward and selecting an enclosing parent repository.

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
  --build-dir build/xmake-windows `
  --engine-source vendor/engrave `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output notes/lanes/latest
```

Use `--only lane1,lane2` to execute a subset. The runner verifies line endings, native include boundaries, and required binaries before execution, writing per-lane logs and a consolidated summary.

### Windows Output Roots and Path Limits

On Windows, standard `MAX_PATH` limitations (260 characters) can cause filesystem IO failures during staging and atomic publication when creating nested directories (such as `.engrave-stage-*` and `report-delivery/fault-*`). Always specify a short output root for `--output` (such as `notes/wr` or a short path near the drive root).

## Lane Inventory

The test runner registers 46 acceptance lanes in total: 37 execute by default and 9 are opt-in.

| Area                        | Lanes                                                                                                                                                 |
| --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| Protocol and session        | `host_api`, `transport`, `async_session`, `tool_schema`, `transport_wakeup` (opt-in)                                                                  |
| Live export and publication | `live_export`, `export_options`, `positions`, `output_schema_boundaries` (opt-in), `live_dense_cancel` (opt-in), `live_queued_cancel` (opt-in)        |
| Audio                       | `live_audio`, `audio_budget`, `score_audio`, `audio_facade`, `engine_audio`                                                                           |
| Notation and edits          | `score_edit`, `native_edit`, `linked_tie`, `xml_text`, `repeats`, `native_470_conversion_cancel` (opt-in), `native_470_serialization_cancel` (opt-in) |
| Documents and formats       | `score_positions`, `excerpts`, `open_data`, `imports_midi`, `imports_gp`, `archives`, `roundtrip`, `native_470_oracle` (opt-in)                       |
| Golden and semantics        | `golden`, `semantic_check`, `rendered_page`, `oracle_harness`                                                                                         |
| Assets and engine           | `runtime_assets`, `assets`, `import_export_probe`, `upstream_gtest`, `version_info`                                                                   |
| Smoke and hygiene           | `mcp_smoke`, `native_includes`, `native_inputs`, `windows_manifest`, `document_stress` (opt-in), `xmake_incremental` (opt-in)                         |

### Direct Fork Script Invocations

The host test runner invokes fork test scripts directly from `vendor/engrave/tests/integration/` for engine-owned lanes (`test_assets.py` for `assets`, `test_excerpts.py` for `excerpts`). No duplicate host wrapper scripts exist in host `tests/integration/`.

### Live Export Child Scripts

The `live_export` lane runs five child test scripts inside its execution:

1. `tests/integration/test_live_publication_cancel.py`: verifies atomic publication and publication cancellation.
2. `tests/integration/test_live_publication_limits.py`: verifies publication limits and quotas.
3. `tests/integration/test_destination_errors.py`: verifies destination path error mapping.
4. `tests/integration/test_live_path_utf8.py`: verifies non-ASCII UTF-8 export paths and staging.
5. `tests/integration/test_live_report_delivery.py`: verifies structured error delivery and document callbacks.

In addition, `live_export` verifies direct partial-writer comparisons against `live_export_test` and multi-format MCP export outputs.

### Opt-in Lane Requirements

The 9 opt-in lanes are excluded from default runs and require specific parameters or dedicated execution environments:

1. `output_schema_boundaries`: Validates exported score schema boundaries. Requires `--binary engrave-mcp`, `--resources <assets>`, `--soundfont <soundfont>`, and `--output <dir>`.
2. `document_stress`: Runs bounded repeated document open, edit, and export lifecycles. Requires the `ENGRAVE_MCP_STRESS_OUTPUT` environment variable set to the output directory.
3. `live_dense_cancel`: Verifies large score cancellation recovery. Requires `--binary engrave-mcp`, `--resources <assets>`, `--soundfont <soundfont>`, `--output <dir>`, `--score <dense_musicxml>` (generated via `cancel_score.generate_cancel_score`), and `--delay-ms 10`.
4. `live_queued_cancel`: Verifies cancellation of queued export requests before engine processing. Requires `--binary engrave-mcp`, `--resources <assets>`, `--soundfont <soundfont>`, and `--output <dir>`.
5. `native_470_conversion_cancel`: Verifies interruptible MuseScore 4.70 conversion. Requires `--binary engrave-mcp-test`, `--resources <assets>`, `--output <dir>`, and `--host-sha256 <sha256>` of the test binary.
6. `native_470_serialization_cancel`: Verifies low-level MuseScore 4.70 serialization cancellation. Requires `--binary engrave-mcp-test`, `--resources <assets>`, and `--output <dir>`.
7. `transport_wakeup`: Verifies transport process teardown, thread termination, and idle CPU limits. Requires `--binary engrave-mcp`, `--resources <assets>`, and `--output <dir>`.
8. `xmake_incremental`: Verifies tool registry schema regeneration, large UTF-8 descriptions, and incremental no-op build behavior. Requires `--binary engrave-mcp`, `--assets <assets>`, and `--output <dir>`. Because this test mutates `engrave_mcp/schema_defs.cpp` and triggers incremental builds, it must execute only in an isolated clone (under `.worktrees/`).
9. `native_470_oracle`: Compares 4.70 writer output against the pinned Linux 4.7.6 oracle. Requires `--only native_470_oracle`, the oracle artifacts (`--native-470-oracle-archive`, `--native-470-oracle-executable`, `--native-470-oracle-assets`), and exactly one mode choice: `--native-470-baseline`, `--native-470-writer-results <dir>`, or `--native-470-producer` (which requires `--host-binary <bin>` and `--host-sha256 <sha256>`).

### Running the Native 4.70 Oracle Lane

The oracle test executes through the unified test runner (`tests/run_lanes.py`) or by running the integration test script (`tests/integration/test_native_470_oracle.py`) directly.

#### Runner Invocation

The test runner passes common lane arguments (`--build-dir`, `--engine-source`, `--assets`, `--soundfont`, `--output`) and requires `--only native_470_oracle`, the oracle flags, and a mode selection:

```bash
.pixi/envs/default/bin/python tests/run_lanes.py \
  --build-dir build/xmake-linux \
  --engine-source vendor/engrave \
  --assets .runtime/assets \
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 \
  --output notes/lanes/oracle \
  --only native_470_oracle \
  --native-470-oracle-archive /path/to/musescore-4.7.6-oracle.tar.gz \
  --native-470-oracle-executable /path/to/semantic_dump \
  --native-470-oracle-assets /path/to/oracle_assets \
  --native-470-baseline
```

For `--native-470-producer` mode, supply `--host-binary <path>` and `--host-sha256 <sha256>` (and optionally `--native-470-live-edits`). For `--native-470-writer-results <dir>`, supply the directory of actual writer outputs.

#### Direct Script Invocation

The standalone script requires `--oracle-archive`, `--oracle-executable`, `--oracle-assets`, `--output`, and exactly one mode (`--baseline`, `--writer-results <dir>`, or `--producer`):

```bash
.pixi/envs/default/bin/python tests/integration/test_native_470_oracle.py \
  --oracle-archive /path/to/musescore-4.7.6-oracle.tar.gz \
  --oracle-executable /path/to/oracle_bin/semantic_dump \
  --oracle-assets /path/to/oracle_assets \
  --output notes/oracle/run \
  --baseline \
  --current-binary build/xmake-linux/semantic_dump \
  --current-assets .runtime/assets
```

When using `--producer`, pass `--host-binary <path>`, `--host-resources <assets>`, and `--host-sha256 <sha256>` (with optional `--live-edits`) instead of `--current-binary` and `--current-assets`.

### Focused Default Lanes

Three focused lanes run by default:

- `native_includes`: Scans source files and checks that public C++ header inclusion boundaries across host and engine are maintained (requires no precompiled binaries).
- `native_inputs`: Validates server handling of malformed and corrupt inputs (requires `--output <dir>`).
- `windows_manifest`: Checks the embedded Windows application manifests (Segment Heap, UTF-8, asInvoker); skips off Windows.

Lane `archives` runs `tests/integration/test_archives.py` with `--binary build/.../archive_test`, `--server build/.../engrave-mcp`, `--resources <assets>`, and `--output <dir>`, requiring the `--server` binary dependency.

### Audio Paired-Capture Requirement

The `engine_audio` lane verifies audio synthesis determinism, MIDI event generation, dynamic range, and cross-platform audio parity across 26 fixtures against reciprocal captures from the other platform (Windows vs Linux).

The cross-platform check requires matching captures from both platforms with verified host and fork git commit provenance. If the counterpart capture is missing or incomplete, `engine_audio` reports status `pending` (exit code 2). A `pending` result does not constitute acceptance; full acceptance requires passing both local checks and reciprocal cross-platform captures with matching commit provenance.

## Engine gate

The engine owns its standalone public boundary gate in `vendor/engrave/tests/gate.py`. The host provides a thin wrapper at `tests/engine_gate.py`:

Windows (PowerShell 7):

```powershell
.pixi\envs\default\python.exe tests/engine_gate.py --engine-source vendor/engrave `
  --build-dir vendor/engrave/build/gate `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output .runtime/gate-run
```

Linux:

```bash
.pixi/envs/default/bin/python tests/engine_gate.py --engine-source vendor/engrave \
  --build-dir vendor/engrave/build/gate \
  --assets .runtime/assets \
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 \
  --output .runtime/gate-run
```

On Windows, use native Python (`.pixi\envs\default\python.exe`) in PowerShell 7. Pass `--mutex-wrapper scripts/xmake_mutex.ps1` when the engine build shares xmake's build lock. The wrapper passes `--pixi-manifest pixi.toml`, so the gate builds against this project's Pixi environment (the cairo and zlib in `pixi.lock`; on Linux also its conda GCC), puts the environment's `bin` on `PATH` and lets binaries find its libraries through their run path. Use a fresh `--output` directory for each run; existing capture directories are rejected.

The gate builds the fork's `engine_tests` target and executes the API, import, playback, and adapter suites, along with `diagnostics_test`, `io_boundary_test`, installer tests, and asset manifest verification. It compares outputs against pinned goldens using `golden.py` comparators and the platform hash allow-list (`vendor/engrave/tests/data/golden/allow.json`).

## CI

`.github/workflows/ci.yml` (pushes to `main`, pull requests, manual dispatch) is a build-and-unit-test lane, not acceptance. A `checks` job (Ubuntu, Pixi) verifies the submodule pin and xmake inputs, runs `pixi run format-check`, the layout audit and the Python checks that need no binary or asset (`test_test_layout`, `test_native_includes`, `test_engine_gate_wrapper`, `test_cleanup_regressions`, `test_oracle_harness`, `test_rendered_page`, `tests/test_engine_gate.py`). A `build-test` job on Windows (MSVC, `windows-latest`) and Linux (the pinned Pixi GCC) builds `engrave_mcp`, `engrave_mcp_test`, `registry_dump`, `registry_check` (which runs the tool-schema test), `host_api_test`, `staging_test` and, on Linux, `publication_test`, then runs `staging_test`, `publication_test`, `host_api_test` and the integration scripts that need only the server binary and the staged engine assets (`version_info`, `open_data`, `native_inputs`, `positions`, `imports_midi`, `imports_guitarpro`, `repeats`, `transport`, plus `windows_manifest` on Windows). `host_api_test` runs with the in-repository `vendor/engrave/tests/data/engine_audio/sine.sf2` as both SoundFonts, so CI downloads none. The fork has its own workflow (`vendor/engrave/.github/workflows/ci.yml`) for the engine unit tests.

Not run in CI, and still manual here: the engine gate and golden comparisons (`tests/engine_gate.py`), the audio corpus and `engine_audio`, every lane that takes `--soundfont` (`live_export`, `live_audio`, `async_session`, `export_options`, `mcp_smoke`, ...), the opt-in lanes, the benchmarks, the Windows-versus-Linux paired captures, and engine naming lint. A green CI run does not replace the native Windows plus `ssh <linux-host>` acceptance above.

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
