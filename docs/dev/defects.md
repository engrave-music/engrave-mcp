# Defects and Known Issues Ledger

This document is the durable, tracked record of accepted defects, carried technical debts, and known risks for the `engrave` engine fork and the MCP host.

Evidence levels used in this ledger:
- **SOURCE-REVIEWED**: Verified through direct inspection of tracked source code and revision history.
- **EXECUTED**: Verified by compiling, executing binaries, test suites, or diagnostic tools and observing outputs/exit codes.
- **REPORTED**: Documented in milestone reports or prior investigation records (`notes/` paths are ignored scratch evidence and are cited, not relied on).

## Defect and Issue Inventory

| ID | Title | Status | Evidence Level | Impact |
| --- | --- | --- | --- | --- |
| DEFECT-ENGRAVE-001 | LeakSanitizer findings at `engrave-14` | Carried / Open | EXECUTED, SOURCE-REVIEWED | Long-lived host is not proven leak-clean across repeated document lifecycles |
| DEFECT-ENGRAVE-002 | Historical A4 `engrave-4` "ASan clean" claim correction | Documented / Corrected | SOURCE-REVIEWED, EXECUTED | Corrects baseline provenance (was address-only check `detect_leaks=0`, not leak-clean) |
| DEFECT-ENGRAVE-003 | Subnormal reverb-tail float divergence (C4) | Carried / Not adopted | SOURCE-REVIEWED (prior executed evidence) | Linux reverb tail performance penalty without FTZ/DAZ; bit-exactness preserved |
| DEFECT-ENGRAVE-004 | Snapshot/batch rollback may move layout | Accepted contract | SOURCE-REVIEWED, REPORTED | Atomic batch rollback via snapshot reload does not promise bit-exact layout |
| DEFECT-ENGRAVE-005 | Host asset staging must move to installer | Resolved 2026-11-01 (`b53ff3b`) | EXECUTED | Replaced retired QRC generator with engine installer; verified 345 files (28,380,646 B) |
| DEFECT-ENGRAVE-006 | Phase C formal status | ACCEPTED 2026-11-01 (Astra), no must-fix | EXECUTED, SOURCE-REVIEWED | Phase C closed at `engrave-14`; the open leak (-001) and host packaging (-005) items carry into A5 |
| DEFECT-ENGRAVE-007 | Parked API work | IMPLEMENTED at `engrave-15`; host adoption partial | EXECUTED, SOURCE-REVIEWED | The v1 host now retains runtime fonts/SoundFonts through the owned engine; public positions and excerpt export remain for A5 |
| DEFECT-ENGRAVE-008 | Encoder finalization cannot report codec-internal failure | Carried / Open | SOURCE-REVIEWED | A codec failure during final flush can leave a truncated output reported as success |
| DEFECT-ENGRAVE-009 | Audio storage errors lose public IO classification | Carried / Open | SOURCE-REVIEWED | Disk-full or open/close failure surfaces as an engine exception instead of an actionable `Io` result |
| DEFECT-ENGRAVE-010 | Routine engine gate does not execute the new boundary tests | RESOLVED 2026-11-01 (`1fb47a7`) | SOURCE-REVIEWED | Gate now runs diagnostics, IO-boundary, installer and manifest checks; both hosts green at `bfe86bd` |
| DEFECT-ENGRAVE-011 | Host adapter layer predates engine C5-C8 and does not compile | Fixed on Windows; Linux verification pending | EXECUTED, SOURCE-REVIEWED | The v1 host builds and its runtime assets, workers and MCP smoke pass against `engrave-15` on Windows |

## Details

### 1. LeakSanitizer findings at `engrave-14` (DEFECT-ENGRAVE-001)

- **Status**: Carried / Open (accepted defect for Phase C closeout; does not block cleanup gate but precludes leak-clean claim).
- **Evidence Level**: EXECUTED, SOURCE-REVIEWED.
- **Executed Evidence**:
  - Address-only ASan pass at delivery commit `21069dc360` (identical tree `724202126571e3945c276b119dc9986397fcfd10` to tagged merge `engrave-14` / `ab9e33c39b`) exits 0 for 24/24 commands with `detect_leaks=0`.
  - The same 24 commands with leak detection enabled (`detect_leaks=1`) give 18/24 nonzero exits. Functional test assertions pass before the process exits nonzero from LeakSanitizer.
  - Symbolization of 71 direct-leak stack groups from an unstripped binary relinked with identical `.text` bytes resolves eight allocation functions in seven files:
    1. `Part` construction and `Instrument` assignment (`src/engraving/dom/part.cpp:64`)
    2. read500 Part properties (`read500/tread.cpp:3600-3602`)
    3. Instrument construction and map-pointer replacement (`instrument.cpp:1044-1048`)
    4. Instrument templates
    5. Repeat-list collection
    6. MusicXML credits
    7. Note parentheses
  - Independent Terra review execution on the exact source tree as `engrave-14`: targeted test `Create.FromTemplatePath` run with `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1` and `LSAN_OPTIONS=report_objects=0`:
    - `repeat=1`: reports 4,375,936 bytes in 51,297 allocations;
    - `repeat=5`: reports 4,497,888 bytes in 53,049 allocations (delta 121,952 bytes and 1,752 allocations for four additional create/destroy cycles);
    - Functional tests pass, exit code 1 from LSan (logs: `notes/cleanup/c8/review-leaks/one.log`, `five.log`).
- **Source-Reviewed Evidence**: All seven allocation-site source files are byte-unchanged between `engrave-13` and `engrave-14`. However, because no leak-enabled baseline binary was built prior to C8, this establishes "unchanged allocation sites", not a demonstrated pre-existing leak rate.
- **Blind Spots**: The 24-command test set includes a Python fixture generator; the isolated embedder TU and external system libraries (e.g. Cairo) were not all instrumented.
- **Impact**: The long-lived A5 host (one engine per process, owner-thread calls, repeated open/create/close of documents) is not proven leak-clean. Do not attempt a quick "fix" by adding unconditional deletes: `InstrumentList` holds raw pointers and `Part::clone()` copies that list.
- **Next Step**: Focused ownership investigation plus a repeated open/create/close leak regression suite before treating the long-lived A5 host as leak-clean or release-ready.
- **Reviewer agreement**: the Astra phase review (`notes/cleanup/c8/REVIEW-ASTRA.md`) concurs that the repeated-create delta is operationally relevant, that `EngineContext` owning services does not resolve raw DOM ownership, and that recycling export worker processes cannot protect the resident document store.

### 2. Historical correction: the A4 `engrave-4` "ASan clean" claim (DEFECT-ENGRAVE-002)

- **Status**: Documented / Corrected.
- **Evidence Level**: SOURCE-REVIEWED, EXECUTED.
- **Description**: The historical ledger row for `engrave-4` in `docs/dev/fork.md` claimed "ASan clean". That run cited an address-only check configured with `detect_leaks=0`. It was not run with leak detection enabled and does not constitute a leak-clean baseline. It must not be quoted or relied upon as proof of leak freedom.
- **Impact**: Corrects baseline provenance to prevent false inferences that Phase C introduced new leaks.
- **Next Step**: Documented in `docs/dev/fork.md` ledger and recorded here.

### 3. Subnormal reverb-tail float divergence (C4, carried) (DEFECT-ENGRAVE-003)

- **Status**: Carried / Not adopted.
- **Evidence Level**: SOURCE-REVIEWED prior executed evidence.
- **Description**: In C4, a scoped FTZ/DAZ (Flush-To-Zero / Denormals-Are-Zero) experiment was investigated to eliminate CPU slowdowns caused by subnormal floating-point numbers in decaying reverb tails. The experiment reduced sparse Linux mixing from ~9 s to ~0.4 s, but altered float32 WAV and MP3 output on the investigated fixture (first difference at 22.459 s, maximum sample delta ≈ 3.2e-30; golden descriptor changes `nonzeroSamples` and `centroidEnvelope` in 169/268 windows). FLAC, Ogg/Opus, and AAC stayed byte-identical.
- **Notes & Limitations**: Not adopted: any floating-point policy change requires its own two-platform measurement and an explicit hash-pinned golden decision. The original investigation table (`notes/cleanup/c4/`) mislabels Opus as "Vorbis" (the engine and public API use Opus). Furthermore, attributing performance differences to AMD vs. Intel architectures was an inference, not a measured cross-CPU result.
- **Impact**: Audio mixing on Linux without FTZ/DAZ may suffer performance penalties during quiet reverb tails, but bit-exactness is preserved.
- **Next Step**: Any future floating-point optimization must provide two-platform measurements and golden allow-list updates approved by project governance.

### 4. Snapshot/batch rollback may move layout (DEFECT-ENGRAVE-004)

- **Status**: Accepted contract / Tracked known behavior.
- **Evidence Level**: SOURCE-REVIEWED, REPORTED (user decision 2026-10-31).
- **Description**: In the transactional editing model, when an atomic batch that executes an operation under the `Snapshot` policy fails, the document is rolled back by reloading from the prior snapshot. Reloading a score snapshot may shift layout; bit-exact rollback is not promised.
- **Impact**: Inherited behavior from upstream architecture; tracked as an accepted design trade-off, not a cleanup regression.
- **Next Step**: Maintain documentation of this contract in public API specifications and host docs.

### 5. Host asset staging must move to the installer (DEFECT-ENGRAVE-005)

- **Status**: Resolved 2026-11-01 (commit `b53ff3b`).
- **Evidence Level**: EXECUTED.
- **Description**: Historical host asset staging relied on `scripts/generate_resources.py` parsing engine Qt `.qrc` files to produce `resources.json`. Those lists were retired in engine C7, causing `python -B scripts/generate_resources.py --check` to fail at `engrave-14`.
- **Resolution**:
  - Replaced `scripts/prepare.py` asset staging with integration on top of `vendor/musescore/tools/install_assets.py` and authoritative engine manifest `vendor/musescore/engrave/assets.json` (version 1, 345 files, 28,380,646 bytes total).
  - Deleted retired `scripts/generate_resources.py` and `resources.json`; dropped legacy `.runtime/assets-manifest.json` generation.
  - Implemented safe staging with temporary sibling directory and atomic swap-in to handle directory replacement safely on Windows without partial state.
  - Verified clean staging (38.969 s, exit 0, prepared 345 files) and subsequent idempotent run (1.658 s, exit 0, verified 345 files).
  - Verified installed `.runtime/assets` with `python -B vendor/musescore/tools/install_assets.py --verify .runtime/assets` (0.197 s, exit 0).
  - Confirmed 345 files, 28,380,646 total bytes, and byte-identical match between `.runtime/assets/assets.json` and `vendor/musescore/engrave/assets.json`.
  - Verified repair flow: file corruption detected and repaired back to green status (exit 0).
  - Verified asset-consuming smoke check through `pixi run -e native python scripts/test_assets.py --binary build/xmake-windows/assets_test.exe --assets .runtime/assets --font .pixi/envs/native/fonts/Inconsolata-Regular.ttf --output artifacts/test-assets --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3` (exit 0).
- **Related packaging limitation (SOURCE-REVIEWED, `tools/install_assets.py:57`)**: the installer reads `git cat-file HEAD:<source>`, not source-tree bytes, so it needs Git and a checkout with committed blobs; it is not a source-tarball installer. Keep that build-time requirement explicit, or add a hash-checked source-tree fallback when source archives become a distribution target. A5 should install/verify into a fresh versioned directory, stage its own external SoundFonts and native library dependencies, and use the manifest identity in packaging provenance: `Engine::create` checks only the instruments sentinel, so successful startup is not package verification.
- **Observed host build status note (EXECUTED)**: incremental host build `pwsh -NoProfile -File notes/claude-scripts/build.ps1 musescore_mcp` still fails compilation against `engrave-14` due to unmigrated API call sites in `native/worker_protocol.cpp` and `native/music_core.cpp` (pre-Phase C signatures for `initialize_engine_audio`, `load_native_score`, etc.), carried into A5 host cutover.

### 6. Phase C formal status (DEFECT-ENGRAVE-006)

- **Status**: ACCEPTED 2026-11-01 by the GPT-6 Astra phase review, with no must-fix items.
- **Evidence Level**: EXECUTED, SOURCE-REVIEWED.
- **Description**: Phase C (C0–C8) implementation is complete and gate-green at `ab9e33c39b` on both Windows and physical Linux `linux-host` (Terra re-ran both full gates at the exact tag: Windows 213 identical / 38 hash-allowed / 0 different / 0 missing; Linux 186 / 65 / 0 / 0; 124 commands, zero failures). Terra's closeout review initially rejected formal acceptance pending a governance-documentation correction, a tracked defect record and the Astra phase review (`notes/cleanup/c8/REVIEW-TERRA.md`); the first two were completed in `33d9ef7`/`a2d72ac`, and the Astra review then returned ACCEPT for formal closeout with no must-fix items (`notes/cleanup/c8/REVIEW-ASTRA.md`).
- **Impact**: Phase C is formally closed at `engrave-14`. That accepts the cleanup boundary only: it is not a statement of long-lived host release readiness or universal codec/importer correctness. The leak (-001) and host packaging (-005) defects stay open, and -008/-009/-010 were added by the Astra review.
- **Next Step**: Carry -001, -005, -007, -008, -009 and -010 into A5 as explicit work items; the Astra review requires -009 to be resolved before A5's public error mapping is finalized and -010 before the next engine pin.

### 7. Parked API work (DEFECT-ENGRAVE-007)

- **Status**: IMPLEMENTED at `engrave-15`; host adoption partial.
- **Evidence Level**: SOURCE-REVIEWED and EXECUTED (engine gate on both hosts; host adapter on Windows).
- **Description**: Runtime font/SoundFont changes, exact measure/segment positions, and excerpt score-file export were required additions on the cleaned engine boundary. `engrave-15` (`28e6c8ffb6`, merge of `a5/api-gaps-14` at `bfe86bd090`, tree-identical) provides `Engine::add_font`/`Engine::set_soundfonts` with font-metric and layout invalidation, `Document::positions` (1200-DPI page-local geometry, repeat-expanded event times, revision-preserving), and excerpt export through `ExportOptions::score` for MSCX, MSCZ, MusicXML, MXL and MIDI with link remapping and style/metadata handling.
- **Impact**: The engine API no longer blocks excerpt export, runtime font configuration or exact geometry. The v1 host port in `DEFECT-ENGRAVE-011` preserves runtime font and SoundFont mutation; it still uses internal score export/positions and requires the public document API cutover.
- **Next Step**: Adopt `Document::positions` and `ExportOptions::score` in the A5 v2 query/export contract, and migrate runtime assets, worker snapshots and jobs to the public API without dropping their current behavior.

### 8. Encoder finalization cannot report codec-internal failure (DEFECT-ENGRAVE-008)

- **Status**: Carried / Open.
- **Evidence Level**: SOURCE-REVIEWED.
- **Description**: Final flush failures are discarded on the way out of the audio export path. `muse/framework/audio/engine/internal/export/soundtrackwriter.cpp:107` calls `end()` from deferred cleanup and ignores its result; `abstractaudioencoder.h:46` exposes a byte count rather than a status; `flacencoder.cpp:169` ignores `finish()`, `oggencoder.cpp:123` ignores drain status, `mp3encoder.cpp:123` does not propagate negative flush results, and `aacencoder.cpp:236` breaks on non-EOF errors without exposing them. `engine_audio.cpp:733` does check file close and sticky device errors, so filesystem failures are caught, but a codec error discarded inside the encoder cannot be recovered there.
- **Impact**: A codec failure during finalization can leave a nonempty, truncated output reported as successful. No such failure was reproduced, and a zero byte count is not evidence of failure (several encoders return zero on success). These encoder files are unchanged since `engrave-6`, so this is retained debt, not a C4/C7 regression.
- **Next Step**: Give finalization an explicit status-bearing result that preserves the first encode/finalize error, and add injected finalization-failure plus subsequent-render tests.

### 9. Audio storage errors lose public IO classification (DEFECT-ENGRAVE-009)

- **Status**: Carried / Open.
- **Evidence Level**: SOURCE-REVIEWED.
- **Description**: `engrave/src/audio/engine_audio.cpp:718` and `:739` classify output open/write/close failure as `EngineFailure`, and `engrave/src/api/audio.cpp:288` throws for that category, while the same public method returns `ErrorCode::Io` for unusable cache directories and readback failures. A disk-full or unwritable-output failure can therefore surface as an engine exception instead of an actionable IO result. Sticky stream errors are not silently treated as success; they are misclassified. The public mapping is unchanged since C0 apart from explicit context passing.
- **Impact**: A5 cannot classify storage failures reliably, which matters for host error mapping and for reporting partial outputs.
- **Next Step**: Add an internal IO failure category and map it to `Io`, keeping exception propagation from the caller's `ByteSink`; land this before A5's public error mapping is finalized and cover it with a public-boundary failure test.

### 10. Routine engine gate does not execute the new boundary tests (DEFECT-ENGRAVE-010)

- **Status**: Carried / Open.
- **Evidence Level**: SOURCE-REVIEWED.
- **Description**: `xmake/tests.lua` lists `diagnostics_test` and `io_boundary_test` as dependencies of the `c0_gate` target, but `scripts/engine_gate.py` (from `:74` on) never launches them, nor the installer tests or manifest verification. A green routine gate therefore cannot detect regressions covered only by those assertions. Phase C closeout itself is unaffected: those tests were executed separately and reported for this tag.
- **Impact**: The gate's regression coverage is narrower than its target graph implies, so future engine pins could pass while those boundaries regress.
- **Next Step**: Add the missing suites to a durable gate entrypoint before the next engine pin, and verify the manifest against the asset directory actually supplied to the build; keep the artifact comparator and its allowances unchanged.
- **Resolution (2026-11-01)**: `scripts/engine_gate.py` now executes `diagnostics_test` and `io_boundary_test`, runs the engine installer's unit tests (`tools/test_install_assets.py`), and verifies the supplied asset directory against `engrave/assets.json` (`tools/install_assets.py --verify`) as separate `asset-manifest` and `asset-installer` steps; `docs/dev/c0-gate.md` records the wider set. Commit `1fb47a7` (superproject). EXECUTED: the steps passed on Windows (gate exit 0) and on linux-host (gate exit 0) when validating `a5/api-gaps-14` = tree of `engrave-15`. The artifact comparator and allowances were not touched.

### 11. Host adapter layer predates engine C5-C8 and does not compile (DEFECT-ENGRAVE-011)

- **Status**: Fixed on Windows at `engrave-15`; Linux verification pending with the parent A5 work.
- **Evidence Level**: EXECUTED (Windows build, core, worker and MCP tests), SOURCE-REVIEWED (adapter and remaining A5 work).
- **Description**: Before the port, `native/music_core.cpp` and `native/worker_protocol.cpp` still called the engine surface that existed up to `engrave-6`: the `music::runtime_assets` module functions (`set_runtime_cache_root`, `add_runtime_font`, `freeze_runtime_soundfont`, `shutdown_runtime_assets`), the singleton audio initializer (`initialize_engine_audio(soundfont, &error)`, `engine_audio_initialized()`), and two-argument `load_native_score(bytes, format)` / four-argument `export_native_score(project, format, options, soundfont)`. C5 moved those responsibilities into `EngineContext` ownership, so the current internal signatures take a leading `const EngineContext&` (`engrave/src/runtime/noqt_runtime.h`) and the runtime-asset module functions no longer exist. The earlier host build at `engrave-14` failed in `worker_protocol.cpp:331` (`initialize_engine_audio` overload mismatch) and in `music_core.cpp:412, 989, 1018, 1025, 1029, 1041, 1069, 1102, 1144, 1145, 1273, 1361, 1376` (wrong arity plus missing identifiers); the engine targets themselves built and archived cleanly.
- **Resolution**: Host commit `58627bd` creates a public `engrave::Engine` with the resource and cache options, retains a small public anchor `Document`, and obtains the owned context with `engrave::DocumentAccess::context`. This is the same internal bridge used by the fork's `engrave/tests/describe_test.cpp`; legacy projects, the anchor, and the engine are destroyed in that order on the owner thread. Internal loaders/exporters and worker audio use that context. Runtime font registration uses the public `Engine::add_font`; the context also retains the immutable copy and exposes the metadata required by the existing tool. Because the host's projects are still internal rather than public Documents, the adapter performs the public document implementation's text/layout invalidation on every open master and excerpt. The parent retains the selected SoundFont through `context.assets()` without starting audio; the worker calls public `Engine::set_soundfonts` and renders with its own context. This distinction matters because `set_soundfonts` at this pin initializes audio eagerly, while the existing host reserves audio initialization and rendering for workers. No removed module-global asset functions were restored and the engine submodule was not changed.
- **Executed Windows evidence**: `scripts/prepare.py` verified the pinned submodule and 345 staged assets. `pwsh -NoProfile -File notes/claude-scripts/build.ps1 musescore_mcp` exited 0, archived the engine and host libraries and linked `musescore-mcp.exe` (`build ok`, 59.391 s); xmake warned that the `muse_mpe` root `*.cpp` glob matched nothing. `music_core_test.exe` and `musescore-mcp-test.exe` built successfully. The core test passed existing creation/edit/revision, snapshot, mix and excerpt assertions plus new font advance invalidation, master/excerpt relayout, retained SoundFont selection and owner-process audio isolation assertions. The original core test at `f22e8aa` checked the resource path and initially empty font list, but did not contain the claimed font-advance or SoundFont-selection assertions; the advance assertion previously belonged to the engine `assets_test`.
- **Executed MCP evidence**: `scripts/mcp_smoke.py` passed through the rebuilt host with staged resources and `FluidR3Mono_GM.sf3`: 20 create/close cycles, editing, score/image/metadata exports and audible stereo 44.1 kHz WAV. `scripts/test_async_session.py --test-hooks` passed through `musescore-mcp-test.exe`, including snapshot isolation, audio mix parity, output reservations, cancellation, shutdown and worker cleanup. `scripts/test_runtime_assets_mcp.py` passed through `musescore-mcp.exe`: immutable runtime font/SoundFont copies, worker relayout, different selected audio, captured job font and SoundFont selections, rollback on an invalid replacement and retained-file cleanup. Host tests use the Pixi native DLL directory on `PATH`; see ignored `notes/a5/host-port/` for output.
- **Lane caveat**: The unmodified `scripts/run_lanes.py --only music_core` stopped at superproject line-ending preflight because the pre-existing, otherwise unmodified `scripts/engine_gate.py` has CRLF in this checkout. Running the lane with only that superproject preflight bypassed (the engine preflight stayed enabled) passed both the core binary and the runtime-asset MCP test. This is not a fully green unmodified lane-runner claim.
- **Remaining A5 S2+ work**: Replace the privileged legacy project adapter with owner-thread public `Engine`/`Document` storage and codecs. The `score_export`/`score_export_start` position formats need a documented v2 mapping to `Document::positions` (including repeat-expanded timing and geometry); score-file exports need `ExportOptions::score`/`ScoreSel` for excerpt selection. The v2 runtime interface must explicitly retain live font/SoundFont changes, metadata and worker snapshot semantics while removing the internal asset bridge. Integrate these choices in the registry, schemas, job protocol and async session; preserve captured assets, revisions, cancellation and atomic publication. Resolve `DEFECT-ENGRAVE-009` before finalizing public audio IO error mapping, and carry codec-finalization and leak defects separately. Linux build/test and the A5 review remain the parent's work; this Windows-only host port did not run the engine golden gate.
