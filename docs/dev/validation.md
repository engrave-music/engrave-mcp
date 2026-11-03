# Validation

Acceptance is execution-based: a claim is only accepted when the named lane or
gate was built and run at the recorded pin, on native Windows and on the
physical Linux host (`ssh <linux-host>`). Source review alone is labelled as such.

## Prerequisites

Build the binaries the selected lanes need:

```powershell
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
xmake build live_export_test staging_test
```

The `golden`, `semantic_check` and `compat_corpus` lanes need the engine's
`semantic_dump` probe, which is opt-in:

```powershell
xmake f --engrave_tests=y
xmake build semantic_dump
```

Engine test binaries used by some lanes (`score_edit_test`, `score_audio_test`,
`excerpts_test`, `assets_test`, `archive_test`, `noqt_probe`,
`engine_audio_probe`, `engraving_playback_tests`, `mpe_tests`) come from the same
`--engrave_tests=y` configuration.

## Running lanes

```powershell
$env:PATH = "$PWD\.pixi\envs\native\Library\bin;$env:PATH"
.pixi\envs\native\python.exe scripts/run_lanes.py `
  --build-dir build/xmake-windows `
  --engine-source vendor/musescore `
  --assets .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3 `
  --output notes/lanes/latest
```

Use `--only lane1,lane2` for a subset. The runner checks line endings, the
native include boundary and the expected binaries before starting, and writes a
per-lane log plus a summary.

## Lanes

There are 37 lanes; 36 run by default. `phase_b_oracle` is opt-in and needs the
pinned 4.7.6 oracle archive, executable and assets.

| Area | Lanes |
|---|---|
| Protocol and session | `host_api`, `transport`, `async_session`, `tool_schema` |
| Live export and publication | `live_export`, `export_options`, `positions` |
| Audio | `live_audio`, `audio_budget`, `audio_facade`, `engine_audio` |
| Notation and edits | `score_edit`, `native_edit`, `linked_tie`, `xml_text`, `repeats` |
| Documents and formats | `score_positions`, `excerpts`, `open_data`, `imports_midi`, `imports_gp`, `archives`, `roundtrip` |
| Golden and semantics | `golden`, `v2_golden_overlap`, `semantic_check`, `compat_corpus`, `rendered_page` |
| Assets and engine | `runtime_assets`, `assets`, `noqt_probe`, `upstream_gtest`, `version_info` |
| Smoke | `mcp_smoke` |
| Opt-in | `phase_b_oracle` (4.70 writer, pinned oracle) |

The `live_export` lane also runs the focused publication checks
(`test_live_publication_cancel.py`, `test_live_publication_limits.py`,
`test_live_path_utf8.py`, `test_live_report_delivery.py`). The
`test_output_schema_boundaries.py`, `test_phase_b_cancellation.py`,
`test_write470_interruption.py`, `test_live_queued_cancel.py`,
`test_live_dense_cancel.py`, `test_transport_wakeup.py` and
`test_native_version.py` checks are documented focused tests, not default lanes;
run them explicitly when touching those areas.

## Engine gate

The engine's public-boundary gate is separate from the host lanes:

```powershell
python scripts/engine_gate.py --engine-source FORK --build-dir FORK/build/c0 `
  --assets ASSETS --soundfont SF3 --output OUTPUT --cairo-prefix PREFIX
```

On Windows, use the Pixi native Python and PowerShell 7, and add
`--mutex-wrapper <wrapper.ps1>` when the engine build needs the project's xmake
lock. On Linux the Cairo prefix is the Pixi environment root; put its `bin` on
`PATH` and `lib` on `LD_LIBRARY_PATH`. Use a fresh `--output` directory for each
run; the runner rejects existing captures. `--skip-build` is for diagnostics
only.

The gate builds the fork's `c0_gate` target and runs the API, import, playback
and adapter suites, plus `diagnostics_test`, `io_boundary_test`, the engine
installer's unit tests and verification of the supplied asset directory. It
compares outputs against the pinned goldens with `golden.py`'s comparators and
the per-platform hash allow-list. At `engrave-31` the gate was green on Windows
(213 identical / 38 allowed / 0 different / 0 missing) and on linux-host
(186 / 65 / 0 / 0).

## Accepted evidence (M4, 2026-11-03)

Host code through `cd0f504`, engine `engrave-31` (`629027f1`), acceptance record
in `d07e624`.

| Measurement | Windows | Linux |
|---|---|---|
| Render worst-row median / p95 | 8.348 / 9.534 ms (fixed replay 7.986 / 10.343) | 1.652 / 1.789 ms |
| Dense cancellation p95 / max | 8.872 / 27.053 ms | 11.270 / 11.312 ms |
| Golden (identical / allowed / different / missing / extra) | 307 / 83 / 0 / 0 / 0 | 265 / 125 / 0 / 0 / 0 |
| WAV/FLAC corpus pairs | 44/44 byte-identical | 44/44 |
| Warm WAV p95 | 270.094 ms in the accepted run | — |

Read the audio row as an observation. Full 1,000-call replays produced both
passing and failing runs at the same binary, dominated by host contention; M4
accepted audio latency as an unresolved variance limit, backed by functional
parity, byte-identical audio and cancellation. Do not present a single passing
run as a stable p95.

The earlier A5/M1–M3 phase evidence, measurement variance studies and per-lane
JSON dumps are not carried in this tree; they are in Git history at `704295d`
under `docs/dev/`.

## Benchmarks

- `scripts/bench_export_latency.py` — warm/cold render and score-export latency,
  phase breakdown, optional high-priority mode
  (`ENGRAVE_MCP_BENCH_HIGH_PRIORITY=1`).
- `scripts/bench_live_audio.py` — warm/cold audio latency and RSS.
- `scripts/bench_open_cold_drift.py` — open, cold-to-PDF and long-run drift.
- `scripts/bench_export_rss.py` — RSS sampling around exports.
- `scripts/test_live_dense_cancel.py` — cancellation on a 49,152-note score.
- `scripts/test_xmake_incremental.py` — incremental-build behaviour.

Keep raw samples, phase timings and host conditions with any reported number.
A timing claim without its host, build, priority and sample count is not
evidence.

## What is not covered

- No leak-clean claim: LSan findings are open (defect 001).
- No hard process-memory bound; the export budget is a logical file budget.
- No stable audio p95 guarantee.
- No macOS or non-x86_64 validation.
- No claim that the full lane set passes on a host that lacks the opt-in engine
  test binaries; the runner fails closed on missing binaries.
