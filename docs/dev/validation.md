# Validation

Acceptance is execution-based: a claim is accepted when the named lane or gate was built and run on native Windows and on the physical Linux host (`ssh <linux-host>`). Source review alone is labeled as such.

## Prerequisites

Build the binaries required by the selected lanes:

```powershell
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
xmake build live_export_test staging_test
```

Lanes require both host test targets and opt-in engine test targets. Configure with `--engrave_tests=y` and build:

```powershell
# Configure with engine tests enabled
xmake f --engrave_tests=y

# Build engine test suite (the engine_tests phony target pulls in semantic_dump, score_edit_test,
# score_audio_test, score_positions_test, excerpts_test, archive_test, assets_test, diagnostics_test,
# io_boundary_test, engraving_playback_tests, etc.) plus the import_export_probe runtime probe
# (verifies EngineContext import, layout, MusicXML export, and MIDI export)
xmake build engine_tests import_export_probe

# Build required host test targets
xmake build live_export_test staging_test
# On Linux:
# xmake build publication_test
```

## Running lanes

Run the unified test runner:

```powershell
$env:PATH = "$PWD\.pixi\envs\native\Library\bin;$env:PATH"
.pixi\envs\native\python.exe tests/run_lanes.py `
  --build-dir build/xmake-windows `
  --engine-source vendor/engrave `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output notes/lanes/latest
```

Use `--only lane1,lane2` to execute a subset. The runner verifies line endings, native include boundaries, and required binaries before execution, writing per-lane logs and a consolidated summary.

## Lanes

There are 35 lanes; 34 run by default. `native_470_oracle` is opt-in and requires the pinned 4.7.6 oracle archive, executable, and assets.

| Area                        | Lanes                                                                                             |
| --------------------------- | ------------------------------------------------------------------------------------------------- |
| Protocol and session        | `host_api`, `transport`, `async_session`, `tool_schema`                                           |
| Live export and publication | `live_export`, `export_options`, `positions`                                                      |
| Audio                       | `live_audio`, `audio_budget`, `score_audio`, `audio_facade`, `engine_audio`                       |
| Notation and edits          | `score_edit`, `native_edit`, `linked_tie`, `xml_text`, `repeats`                                  |
| Documents and formats       | `score_positions`, `excerpts`, `open_data`, `imports_midi`, `imports_gp`, `archives`, `roundtrip` |
| Golden and semantics        | `golden`, `semantic_check`, `rendered_page`, `oracle_harness`                                     |
| Assets and engine           | `runtime_assets`, `assets`, `import_export_probe`, `upstream_gtest`, `version_info`               |
| Smoke                       | `mcp_smoke`                                                                                       |
| Opt-in                      | `native_470_oracle` (4.70 writer, pinned Linux oracle)                                            |

### Focused tests

The `live_export` lane includes focused publication tests (`test_live_publication_cancel.py`, `test_live_publication_limits.py`, `test_live_path_utf8.py`, `test_live_report_delivery.py`).

Additional focused tests located in `tests/integration/` can be run directly when touching specific areas:

- `test_output_schema_boundaries.py` — exported score schema conformance
- `test_native_470_conversion_cancel.py` — interruptible 4.70 conversion
- `test_native_470_serialization_cancel.py` — low-level 4.70 serialization cancellation
- `test_live_queued_cancel.py` — queued export cancellation
- `test_live_dense_cancel.py` — large score cancellation recovery
- `test_transport_wakeup.py` — transport teardown and idle CPU checks
- `test_native_export_versions.py` — native version options and warnings
- `test_destination_errors.py` — destination path error mapping
- `test_document_stress.py` — bounded repeated document lifecycles
- `test_native_includes.py` — public C++ header boundary verification
- `test_native_inputs.py` — malformed and corrupt input handling
- `test_xmake_incremental.py` — incremental build behavior

## Engine gate

The engine owns its standalone public boundary gate in `vendor/engrave/tests/gate.py`. The host provides a thin wrapper at `tests/engine_gate.py`:

Windows (PowerShell 7):

```powershell
.pixi\envs\native\python.exe tests/engine_gate.py --engine-source vendor/engrave `
  --build-dir vendor/engrave/build/gate `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output .runtime/gate-run
```

Linux:

```bash
python tests/engine_gate.py --engine-source vendor/engrave \
  --build-dir vendor/engrave/build/gate \
  --assets .runtime/assets \
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 \
  --output .runtime/gate-run \
  --cairo-prefix .pixi/envs/native
```

On Windows, use native Python (`.pixi\envs\native\python.exe`) in PowerShell 7 (`--cairo-prefix` is optional on Windows). Pass `--mutex-wrapper scripts/xmake_mutex.ps1` when the engine build shares xmake's build lock. On Linux, set `--cairo-prefix` to the Pixi environment root, with its `bin` on `PATH` and `lib` on `LD_LIBRARY_PATH`. Use a fresh `--output` directory for each run; existing capture directories are rejected.

The gate builds the fork's `engine_tests` target and executes the API, import, playback, and adapter suites, along with `diagnostics_test`, `io_boundary_test`, installer tests, and asset manifest verification. It compares outputs against pinned goldens using `golden.py` comparators and the platform hash allow-list (`vendor/engrave/tests/data/golden/allow.json`).

## Known timing and platform limitations

- **Windows dense-audio cancellation variance**: The dense-audio cancellation assertion can fail intermittently on Windows. The engine test (`AudioInternal.DenseScoreCancellationLatencyAndRecovery`) uses a 30 ms p95 threshold; the host integration test (`tests/integration/test_live_dense_cancel.py:188`) asserts a 50 ms p95 and 100 ms maximum. The cause of the timing variation is not established. An intermittent failure must not be reported as a clean pass.
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
