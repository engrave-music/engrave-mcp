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
.pixi\envs\native\python.exe tests/run_lanes.py `
  --build-dir build/xmake-windows `
  --engine-source vendor/engrave `
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

| Area                        | Lanes                                                                                               |
| --------------------------- | --------------------------------------------------------------------------------------------------- |
| Protocol and session        | `host_api`, `transport`, `async_session`, `tool_schema`                                             |
| Live export and publication | `live_export`, `export_options`, `positions`                                                        |
| Audio                       | `live_audio`, `audio_budget`, `score_audio`, `audio_facade`, `engine_audio`                         |
| Notation and edits          | `score_edit`, `native_edit`, `linked_tie`, `xml_text`, `repeats`                                    |
| Documents and formats       | `score_positions`, `excerpts`, `open_data`, `imports_midi`, `imports_gp`, `archives`, `roundtrip`   |
| Golden and semantics        | `golden`, `v2_golden_overlap`, `semantic_check`, `compat_corpus`, `rendered_page`, `oracle_harness` |
| Assets and engine           | `runtime_assets`, `assets`, `noqt_probe`, `upstream_gtest`, `version_info`                          |
| Smoke                       | `mcp_smoke`                                                                                         |
| Opt-in                      | `phase_b_oracle` (4.70 writer, pinned oracle)                                                       |

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
python tests/engine_gate.py --engine-source FORK --build-dir FORK/build/c0 `
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
installer's unit tests and verification of the supplied asset directory. The
`mnx_schema` step validates the MNX files that `engrave_mnx_import_test`
exports against the vendored W3C MNX schema with a draft 2020-12 validator
(`tests/support/check_mnx_schema.py`; mnxdom's own validator implements draft 7). It
compares outputs against the pinned goldens with `golden.py`'s comparators and
the per-platform hash allow-list. At `engrave-31` the gate was green on Windows
(213 identical / 38 allowed / 0 different / 0 missing) and on linux-host
(186 / 65 / 0 / 0).

At `engrave-32` (`9d13ae4938`), the complete gate passed with the same counts
on both platforms. The first Windows attempt failed the existing dense-audio
synthesis cancellation p95 assertion at 30.1587 ms against a 30 ms limit; a
subsequent complete run passed at 26.7596 ms. The result is an observed rerun,
not evidence that the timing threshold is stable.

At `engrave-33` (`bf44cdeb88`), the complete gate passed at the exact commit
with the same counts on Windows and linux-host. The MusicXML velocity regression
and 12 before/after playback cases also passed on both; the saved WAV and MIDI
files for those cases were byte-identical.

At `engrave-34` (`8eb0c25a52`), 17 focused MusicXML dynamics tests, the host
native-edit test and the host golden comparison passed on both platforms. The
golden counts were 307 identical/83 allowed on Windows and 265/125 on linux-host,
with zero unexpected outputs using `tests/data/golden/allow.json` SHA-256
`a19f8f905e182ccd9e3f25a3eaad726300366ba2993e858ba01586ec70e8979d`.
The full Linux engine gate passed at the exact commit (186 identical/65 allowed/
0 different/0 missing). Twelve before/after playback probes on each platform
confirmed the intentional grouped-dynamics audio and MIDI changes.

The full Windows engine gate **failed** at the exact commit: dense-audio
synthesis cancellation measured 140.0552 ms p95 and 176.2603 ms maximum,
above its 30 ms and 50 ms limits. A separate exact-commit diagnostic run excluded
only `AudioInternal.DenseScoreCancellationLatencyAndRecovery` and passed the
remaining checks and golden comparison (213 identical/38 allowed/0 different/
0 missing). It is not a full gate pass. Earlier normal-priority runs at related
candidate commits also missed this timing assertion. The cause and stability of
the timing variation are not established.

At `engrave-35` (`8e522af6b7`), both exact-commit full engine gates passed:
Windows 213 identical/38 allowed/0 different/0 missing, and physical linux-host
186/65/0/0. Seven focused chord-line read tests and nine edit tests also passed
on each platform, including the existing three-point legacy path, 5.00 offsets,
MSCX/MSCZ reopen after edits, linked parts and undo/rollback. Host golden
comparisons passed with 307 identical/83 allowed on Windows and 265/125 on linux-host,
using the unchanged allow-list hash above. An executed host probe confirmed 5.00
offset export and the existing fail-closed 4.70 refusal for chord lines.

The Windows dense-audio cancellation test passed in this run: synthesis p95
26.1483 ms and maximum 27.7273 ms, against 30 ms and 50 ms limits. This does
not resolve or explain the prior `engrave-34` failures. The optional warm-graph
timing test was skipped by its default test configuration.

## Accepted evidence (M4, 2026-11-03)

Host code through `cd0f504`, engine `engrave-31` (`629027f1`), acceptance record
in `d07e624`.

| Measurement                                                | Windows                                                              | Linux                 |
| ---------------------------------------------------------- | -------------------------------------------------------------------- | --------------------- |
| Render worst-row median / p95                              | 8.348 / 9.534 ms (fixed replay 7.986 / 10.343)                       | 1.652 / 1.789 ms      |
| Dense cancellation p95 / max                               | 8.872 / 27.053 ms                                                    | 11.270 / 11.312 ms    |
| Golden (identical / allowed / different / missing / extra) | 307 / 83 / 0 / 0 / 0                                                 | 265 / 125 / 0 / 0 / 0 |
| WAV/FLAC corpus pairs                                      | 44/44 byte-identical                                                 | 44/44                 |
| Warm WAV p95                                               | 274.877 ms clean full run; 270.094 ms corrected high-priority repeat | —                     |

Read the audio row as an observation. Full 1,000-call replays produced both
passing and failing runs at the same binary. `engine_call` dominated the failing
samples, but phase timing cannot separate engine work, I/O and scheduling, so do
not attribute the variance to the host alone. M4 accepted audio latency as an
unresolved variance limit, backed by functional parity, byte-identical audio and
cancellation. Do not present a single passing run as a stable p95.

The earlier A5/M1–M3 phase evidence, measurement variance studies and per-lane
JSON dumps are not carried in this tree; they are in Git history at `704295d`
under `docs/dev/`.

## Benchmarks

- `tests/benchmarks/bench_export_latency.py` — warm/cold render and score-export latency,
  phase breakdown, optional high-priority mode
  (`ENGRAVE_MCP_BENCH_HIGH_PRIORITY=1`).
- `tests/benchmarks/bench_live_audio.py` — warm/cold audio latency and RSS.
- `tests/benchmarks/bench_document_lifetime.py` — open, cold-to-PDF and long-run drift.
- `tests/benchmarks/bench_export_rss.py` — RSS sampling around exports.
- `tests/integration/test_live_dense_cancel.py` — cancellation on a 49,152-note score.
- `tests/integration/test_xmake_incremental.py` — incremental-build behaviour.

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
