# Validation

Validation is tiered and execution-based:

- **Per step**: Build, run the tests the change can affect, and run
  `tests/engine_gate.py --quick` when engine code changed.
- **At milestones and engine bumps**: Run the full engine gate
  (`tests/engine_gate.py` without `--quick`), the default host lanes
  (`tests/run_lanes.py`), and validate on both native Windows and native Linux.
- **Evidence hygiene**: State which claims are **executed** and which are
  **source-reviewed**; never conflate them.

Run outputs (`--output` for lanes and the gate) go under `notes/runs/<name>`
(gitignored), never under `.runtime/`, which holds runtime assets only.

## Building for Tests

`tests/run_lanes.py` builds the targets selected lanes need before running them
(`--no-build` skips the build; `--profile` selects `build/<profile>`, defaulting
to `release`). All `scripts/bbuild.py` profiles share `build/.bazel_repo_cache`
and `build/.bazel_disk_cache`; see [build.md](build.md) for profile builds and
standalone test targets.

## Test Layout

- `tests/integration/`: Standalone executable integration scripts. Never imported
  as Python modules; integration directories must not contain `__init__.py`.
- `tests/engrave_mcp_testing/`: Single shared helper package for the host,
  installed in editable mode into `.pixi/envs/default` during `pixi install`.
- `tests/benchmarks/`: Performance, latency, RSS, and writer precision
  benchmarks.
- `tests/data/`: Host-specific test data, manifests, and golden references.

### Layout Lint Guard

`audit_test_layout` (`tests/engrave_mcp_testing/layout.py`) runs during
`pixi run lint` and enforces:

1. No `__init__.py` inside any `tests/integration` directory in either
   repository.
2. No Python script in `tests/`, `scripts/`, `tools/`, `engrave/tests/`, or
   `src/**/tests/` imports any module starting with `test_`. Shared helpers must
   live in `engrave_mcp_testing` (host) or `engrave_testing` (engine fork).

## Running a Single Test Directly

Because `pixi install` installs `engrave-host-testing` and `engrave-testing` in
editable mode into `.pixi/envs/default`, run scripts directly with the Pixi
Python interpreter without setting `PYTHONPATH`:

```powershell
# Windows
.pixi\envs\default\python.exe tests/integration/test_mcp_smoke.py
```

```bash
# Linux
.pixi/envs/default/bin/python tests/integration/test_mcp_smoke.py
```

### Windows Manifest Verification

The default `windows_manifest` lane runs
`tests/integration/test_windows_manifest.py --build-dir <build-dir>`, using the
Windows SDK `mt.exe` to extract embedded manifests from `engrave-mcp.exe`,
`engrave-mcp-test.exe`, and `engrave_output_test.exe`. It requires `SegmentHeap`
and UTF-8 under `application/windowsSettings` plus `asInvoker` (Windows ignores
misplaced elements, so the check fails if placement is wrong). Off Windows, the
lane skips.

## Running Lanes

```powershell
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"
.pixi\envs\default\python.exe tests/run_lanes.py `
  --engine-source vendor/engrave `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output notes/runs/lanes -j 4
```

Key options and scheduling behavior:

- **Target & lane selection**: `--profile` selects `build/<profile>` (default:
  `release`), `--build-dir` overrides it, `--no-build` skips building, `--only
lane1,lane2` runs a subset (including opt-in lanes), and `--list-lanes` prints
  the catalogue, scheduling mode, and dependencies without requiring runtime
  paths. The runner verifies line endings, native include boundaries, and
  required binaries before starting.
- **Parallel vs. exclusive lanes**: Parallel lanes run in a thread pool
  (`-j` / `--jobs`, default half CPU count clamped to 2–8; `-j 1` runs
  sequentially), each with its own `<output>/<lane>/cwd`, `tmp`, and
  `<output>/<lane>.log`. `semantic_check` waits for `golden` (or requires a
  prior capture in `<output>/golden/actual`). After parallel lanes drain,
  exclusive lanes run one at a time (default: `live_export`, `live_audio`,
  `archives`, `async_session`, `engine_audio`; opt-in: `document_stress`,
  `live_dense_cancel`, `live_queued_cancel`, `transport_wakeup`,
  `bazel_incremental`) so concurrent work does not skew latency measurements or
  share working state. Avoid unrelated builds or benchmarks when measuring
  timing.
- **Focused default lanes**: `native_includes` checks public header boundaries,
  `native_inputs` checks malformed input handling, `windows_manifest` checks
  Segment Heap / UTF-8 / `asInvoker` manifests, `live_export` runs
  publication/cancellation/UTF-8 path checks alongside `live_export_test`, and
  `archives` tests archive export against `archive_test` and `engrave-mcp`.
- **Opt-in lanes** (`--only`): Excluded from default runs because they require
  extra inputs (`native_470_oracle`), test stress/cancellation limits
  (`output_schema_boundaries`, `document_stress`, `live_dense_cancel`,
  `live_queued_cancel`, `native_470_conversion_cancel`,
  `native_470_serialization_cancel`, `transport_wakeup`), or mutate source files
  and rebuild (`bazel_incremental`, which must run in an isolated `.worktrees/`
  clone).

### Windows Output Roots and Path Limits

On Windows, standard `MAX_PATH` limits (260 characters) can cause I/O errors
during staging and atomic publication when nested directories (such as
`.engrave-stage-*` and `report-delivery/fault-*`) are created. Always use a
short `--output` root such as `notes/runs/w`.

### Native 4.70 Oracle Lane

Run via `tests/run_lanes.py --only native_470_oracle` or directly via
`tests/integration/test_native_470_oracle.py`, supplying
`--native-470-oracle-archive`, `--native-470-oracle-executable`,
`--native-470-oracle-assets`, and one mode (`--native-470-baseline`,
`--native-470-writer-results <dir>`, or `--native-470-producer` with
`--host-binary` and `--host-sha256`):

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

When invoking `tests/integration/test_native_470_oracle.py` directly, pass
`--oracle-archive`, `--oracle-executable`, `--oracle-assets`, `--output`, and
one mode (`--baseline` / `--writer-results` with
`--current-binary build/release/semantic_dump` and
`--current-assets .runtime/assets`, or `--producer` with `--host-binary`,
`--host-resources`, and `--host-sha256`).

Producer mode uses the pinned 4.7.6 `semantic_dump` to reopen MSCX and MSCZ
exports of the compatibility manifest, all 26 golden fixtures, and a blank
grand-staff Piano from `score_create`. For the corpus, it compares the full
semantic dump against the corresponding 5.00 export and requires zero load
errors and zero connector recovery diagnostics. The five documented fermata
fixtures permit only the pinned reader's exact one-tick-early tempo restoration.
The Piano check also verifies the legacy brace and bass clef in the exported XML.

Add `--native-470-live-edits` to the lane command (or `--live-edits` to the
direct script) to exercise paired edits, checkpoint rollback, and one-end tie
refusals. The linked B4-to-C5 and B4-to-C♯5 edits check pitch, spelling, and
ties in the oracle dump. The chromatic probe checks the sharp accidental in
the same MSCX and MSCZ artifacts because the pinned dump omits accidentals.

### Audio Paired-Capture Requirement

The `engine_audio` lane verifies audio synthesis determinism, MIDI event
generation, dynamic range, and cross-platform audio parity against reciprocal
captures from the other platform (Windows vs. Linux) with matching host and fork
git commit provenance:

- Produce each platform's capture with
  `tests/integration/test_engine_audio.py --capture-cross-platform-only`.
- Place both `<root>/windows` and `<root>/linux` under the lane output's
  `audio-cross-platform/` directory (or pass `--cross-platform-captures`).
- If the counterpart capture is missing or incomplete, `engine_audio` exits with
  status `pending` (code 2). Milestone acceptance requires both local checks and
  matching reciprocal captures.

## Engine Gate

The engine owns its standalone public boundary gate in
`vendor/engrave/tests/gate.py`. The host wrapper at `tests/engine_gate.py`
builds `engine_tests` into `build/<profile>` via `scripts/bbuild.py` by default,
defaults to `--pixi-manifest pixi.toml`, and shares `build/.bazel_repo_cache`
and `build/.bazel_disk_cache`:

```powershell
# Windows (PowerShell 7)
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

- `--quick` skips the `engine-audio` corpus check for faster per-step
  verification; omit `--quick` at milestones and engine bumps.
- With an engine checkout that includes the SoundFont-free lane, `--no-audio`
  runs GTest, asset and MNX checks, and compares 226 outputs across 25 fixtures.
  Add `--goldens-only` to run just the asset manifest and golden checks. Before
  the submodule pin advances, pass the updated checkout with `--engine-source`.
- Always pass a fresh `--output` directory under `notes/runs/<name>`; existing
  capture directories are rejected.
- The gate executes `engrave_core_tests` (API, handles, describe, read, ops,
  import, MNX, diagnostics, and I/O boundary suites across GTest shards),
  `engrave_output_test`, upstream playback/MPE suites, installer tests, asset
  manifest verification, and golden comparisons (`golden.py` +
  `vendor/engrave/tests/data/golden/allow.json`).

## CI

Both CI workflows run on pushes to `main`, pull requests, and manual dispatch.

- **`checks`** (Ubuntu, Pixi): Verifies the submodule pin and Bazel inputs
  (`scripts/check_build_inputs.py`), runs `pixi run format-check`, the layout
  audit, and binary-free Python checks (`test_test_layout`,
  `test_native_includes`, `test_engine_gate_wrapper`,
  `test_cleanup_regressions`, `test_oracle_harness`, `test_rendered_page`,
  `tests/test_engine_gate.py`, `tests/test_bbuild.py`).
- **`build-test`** (Windows and Linux, Bazel + `xclang`): Builds `engrave_mcp`,
  `engrave_mcp_test`, `registry_dump`, `registry_check`, `host_api_test`,
  `staging_test`, and (on Linux) `publication_test`, then runs `staging_test`,
  `publication_test`, `host_api_test` (using the in-repo
  `vendor/engrave/tests/data/engine_audio/sine.sf2` so CI downloads no
  SoundFonts), and asset-only integration scripts (`cli`, `version_info`, `open_data`,
  `native_inputs`, `positions`, `imports_midi`, `imports_guitarpro`, `repeats`,
  `transport`, plus `windows_manifest` on Windows). The positions lane compares
  a PNG exported through MCP to the pinned engine's platform golden.
- The host **`sanitize`** job on Linux builds the executable, staging, publication,
  host API, and engine core tests with `--profile sanitize`. Runs the four test
  binaries with ASan, LSan, and UBSan, using four shards for the engine core
  suite and the checked-in sine SoundFont for the host API.
- The **fork workflow** at `vendor/engrave/.github/workflows/ci.yml` runs release unit tests
  and the SoundFont-free golden comparison on Windows and Linux. Its Linux
  `sanitize` job runs four core shards, both upstream GTest suites, and the
  SoundFont-free golden probes with ASan, LSan, and UBSan. Both sanitizer jobs
  use disk cache keys distinct from their release jobs.
- **Manual milestone checks** include the full engine gate
  (`tests/engine_gate.py`), the audio corpus and `engine_audio`, the live export
  and audio lanes, opt-in lanes, benchmarks, Windows-vs-Linux paired captures,
  and engine naming lint.

## Timing Measurements and Benchmarks

- **Dense-audio cancellation**: The engine test measures
  request-to-first-cancellation-observation (p95 <= 5 ms, max <= 10 ms)
  separately from observation-to-return including synchronous cleanup (p95 <=
  50 ms, max <= 100 ms). The host integration test retains request-to-return
  limits.
- **Benchmark scripts**:
  - `tests/benchmarks/bench_export_latency.py`: warm/cold render and score
    export latency, phase breakdown, optional high-priority mode
    (`ENGRAVE_MCP_BENCH_HIGH_PRIORITY=1`)
  - `tests/benchmarks/bench_live_audio.py`: warm/cold audio synthesis latency
    and RSS
  - `tests/benchmarks/bench_document_lifetime.py`: open, cold-to-PDF, and
    long-run drift
  - `tests/benchmarks/bench_export_rss.py`: RSS memory sampling across export
    cycles
  - `tests/benchmarks/bench_audio_distribution.py`: repeated audio synthesis
    measurement distribution
  - `tests/benchmarks/bench_native_writer_precision.py`: writer precision
    round-trip measurements
  - `tests/integration/test_live_dense_cancel.py`: cancellation on a 49,152-note
    score
  - `tests/integration/test_bazel_incremental.py`: incremental build and schema
    regeneration behavior

Benchmark claims must retain raw samples, phase timings, hardware
specifications, and environment conditions.

## What Is Not Covered

- No hard process-memory bound: export quotas enforce logical file output
  limits, not process RSS limits.
- No hard audio latency p95 guarantee under host contention.
- No macOS or non-x86_64 platform validation.
- Missing opt-in binaries fail closed: running the full suite requires compiling
  the optional engine test targets.
