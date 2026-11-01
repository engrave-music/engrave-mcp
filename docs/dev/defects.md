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
| DEFECT-ENGRAVE-008 | Encoder finalization cannot report codec-internal failure | RESOLVED at `engrave-16` (2026-11-01) | SOURCE-REVIEWED, EXECUTED | `end()` now returns a status, the writer preserves the first encode/finalize error, and an injected finalization failure is followed by a byte-identical render |
| DEFECT-ENGRAVE-009 | Audio storage errors lose public IO classification | RESOLVED at `engrave-16` (2026-11-01) | SOURCE-REVIEWED, EXECUTED | Audio storage failures return public `ErrorCode::Io`; a forced output-open failure is tested |
| DEFECT-ENGRAVE-010 | Routine engine gate does not execute the new boundary tests | RESOLVED 2026-11-01 (`1fb47a7`) | SOURCE-REVIEWED | Gate now runs diagnostics, IO-boundary, installer and manifest checks; both hosts green at `bfe86bd` |
| DEFECT-ENGRAVE-011 | Host adapter layer predates engine C5-C8 and does not compile | RESOLVED 2026-11-01 (Windows + linux-host) | EXECUTED | The v1 host builds and its runtime assets, workers and MCP smoke pass against `engrave-15` on both hosts; public-document cutover remains A5 S2+ work |
| DEFECT-ENGRAVE-012 | Snapshot-based render is not bit-identical to direct render | Accepted limitation | EXECUTED | Export jobs render a captured MSCZ snapshot, so render output can differ from a direct in-process render |
| DEFECT-ENGRAVE-013 | Pitch edits can leave a mismatched tie without a diagnostic | Carried / Open | SOURCE-REVIEWED, EXECUTED | An exported mismatched tie may not survive MusicXML reimport |
| DEFECT-ENGRAVE-014 | Snapshot rendering changes an unedited score's version header | Resolved by contract | SOURCE-REVIEWED, EXECUTED | The owner keeps the source version; a written and reopened snapshot and its render report the writer version |
| DEFECT-ENGRAVE-015 | Snapshot render drift exceeds the S3 example | Accepted limitation, exact outputs pinned | SOURCE-REVIEWED, EXECUTED | Fourteen measured outputs differ beyond the original small-drift limits |
| DEFECT-ENGRAVE-016 | Writer numeric precision in save/reopen | Carried / Open | SOURCE-REVIEWED, EXECUTED | Serialized dimensions can lose precision and alter subsequent output |
| DEFECT-ENGRAVE-017 | Output schemas inherited request bounds | Resolved | SOURCE-REVIEWED, EXECUTED | Imported text and edited track counts are no longer rejected by successful-response schemas |
| DEFECT-ENGRAVE-018 | Snapshot capture has MIDI and workspace-estimate gaps | Partially resolved; MIDI and workspace gaps open | SOURCE-REVIEWED, EXECUTED | Supported captures and position JSON are bounded; MIDI fallback and uncounted engine allocations are not a hard process-memory bound |
| DEFECT-ENGRAVE-019 | Audio temporary output can exceed its byte budget | Resolved by host adoption of `engrave-17` | SOURCE-REVIEWED, EXECUTED | Engine limits the cache; the cache and staged copy each receive half the job budget |
| DEFECT-ENGRAVE-020 | Snapshot pins fell through to legacy allowances | Resolved | SOURCE-REVIEWED, EXECUTED | A snapshot pin takes precedence over an older direct-engine allowance |
| DEFECT-ENGRAVE-021 | Runtime font cache breaks Linux audio monitoring | Resolved | SOURCE-REVIEWED, EXECUTED | The audio monitor counts only regular engine audio cache files |
| DEFECT-ENGRAVE-022 | No-repeat audio projected using repeat-expanded duration | Resolved for the reproduced rejection | SOURCE-REVIEWED, EXECUTED | The worker uses the matching end time when exposed and otherwise does not reject on the expanded estimate |
| DEFECT-ENGRAVE-024 | 4.70 cannot represent part EIDs | Accepted format limitation | EXECUTED, SOURCE-REVIEWED | Shared-part downgrades keep numeric part IDs but cannot write part EIDs; every representable element EID is preserved |
| DEFECT-ENGRAVE-025 | 4.70 export refuses scores with generated accidentals | Open / bounded-contract gap | EXECUTED | Editing a tied pair to C-sharp5 produced four accidentals and the export was refused as B470_UNSUPPORTED_FEATURE; accidental conversion is not implemented |
| DEFECT-ENGRAVE-023 | Stave-sharing labels are lost on 5.00 open/export | RESOLVED at `engrave-21` (2026-11-02) | EXECUTED | Labels now survive 5.00 ingest, export and reopen; the 5.00 output of label-bearing scores changes by design and no golden covers one |

## Details

### Host include boundary and poison-test coverage (A5 S6)

Every translation unit and header under `native/`, including tests, must use
only `engrave/...` public engine headers; there is no test-only internal-header
exception. `host_api_test` exercises corrupt-score rejection and recovery after
a throwing public document callback. The public engine API offers no way to
force a snapshot-restore failure, so this does not prove the host's recovery
probe against an actually poisoned document. The engine's own tests use its
internal fault-injection seam for that state. A deterministic end-to-end host
test would require a public, test-only way to request a restore failure; no
such engine contract or change is introduced here.

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

- **Status**: RESOLVED at `engrave-16` (2026-11-01).
- **Evidence Level**: SOURCE-REVIEWED (per-library error branches), EXECUTED (injected failure plus byte-identical follow-up render, five codec hashes).
- **Resolution**: encoder `end()` returns `muse::Ret` instead of a byte count, the soundtrack writer preserves the first encode/finalize error, and render requests accept an encoder factory so the tests can inject a finalization failure. No encoder algorithm or success-path byte changed. Note retained limitation: the injection proves the host/engine plumbing, not each FLAC/Opus/MP3/AAC library's own native failure branch.
- **Description**: Final flush failures are discarded on the way out of the audio export path. `muse/framework/audio/engine/internal/export/soundtrackwriter.cpp:107` calls `end()` from deferred cleanup and ignores its result; `abstractaudioencoder.h:46` exposes a byte count rather than a status; `flacencoder.cpp:169` ignores `finish()`, `oggencoder.cpp:123` ignores drain status, `mp3encoder.cpp:123` does not propagate negative flush results, and `aacencoder.cpp:236` breaks on non-EOF errors without exposing them. `engine_audio.cpp:733` does check file close and sticky device errors, so filesystem failures are caught, but a codec error discarded inside the encoder cannot be recovered there.
- **Impact**: A codec failure during finalization can leave a nonempty, truncated output reported as successful. No such failure was reproduced, and a zero byte count is not evidence of failure (several encoders return zero on success). These encoder files are unchanged since `engrave-6`, so this is retained debt, not a C4/C7 regression.
- **Next Step**: Give finalization an explicit status-bearing result that preserves the first encode/finalize error, and add injected finalization-failure plus subsequent-render tests.

### 9. Audio storage errors lose public IO classification (DEFECT-ENGRAVE-009)

- **Status**: RESOLVED at `engrave-16` (2026-11-01).
- **Evidence Level**: SOURCE-REVIEWED, EXECUTED (forced output-open failure returns `Io`; throwing-sink test).
- **Resolution**: an internal IO failure category now maps audio output open/write/close and encoder-stage storage failures to public `ErrorCode::Io`; `ByteSink` exception propagation is unchanged, and synthesis/codec semantics are untouched.
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

- **Status**: RESOLVED 2026-11-01 at `engrave-15` on Windows and linux-host; the public-document cutover remains A5 S2+ work.
- **Evidence Level**: EXECUTED (both hosts: build, core, worker and MCP checks), SOURCE-REVIEWED (adapter and remaining A5 work).
- **Description**: Before the port, `native/music_core.cpp` and `native/worker_protocol.cpp` still called the engine surface that existed up to `engrave-6`: the `music::runtime_assets` module functions (`set_runtime_cache_root`, `add_runtime_font`, `freeze_runtime_soundfont`, `shutdown_runtime_assets`), the singleton audio initializer (`initialize_engine_audio(soundfont, &error)`, `engine_audio_initialized()`), and two-argument `load_native_score(bytes, format)` / four-argument `export_native_score(project, format, options, soundfont)`. C5 moved those responsibilities into `EngineContext` ownership, so the current internal signatures take a leading `const EngineContext&` (`engrave/src/runtime/noqt_runtime.h`) and the runtime-asset module functions no longer exist. The earlier host build at `engrave-14` failed in `worker_protocol.cpp:331` (`initialize_engine_audio` overload mismatch) and in `music_core.cpp:412, 989, 1018, 1025, 1029, 1041, 1069, 1102, 1144, 1145, 1273, 1361, 1376` (wrong arity plus missing identifiers); the engine targets themselves built and archived cleanly.
- **Resolution**: Host commit `58627bd` creates a public `engrave::Engine` with the resource and cache options, retains a small public anchor `Document`, and obtains the owned context with `engrave::DocumentAccess::context`. This is the same internal bridge used by the fork's `engrave/tests/describe_test.cpp`; legacy projects, the anchor, and the engine are destroyed in that order on the owner thread. Internal loaders/exporters and worker audio use that context. Runtime font registration uses the public `Engine::add_font`; the context also retains the immutable copy and exposes the metadata required by the existing tool. Because the host's projects are still internal rather than public Documents, the adapter performs the public document implementation's text/layout invalidation on every open master and excerpt. The parent retains the selected SoundFont through `context.assets()` without starting audio; the worker calls public `Engine::set_soundfonts` and renders with its own context. This distinction matters because `set_soundfonts` at this pin initializes audio eagerly, while the existing host reserves audio initialization and rendering for workers. No removed module-global asset functions were restored and the engine submodule was not changed.
- **Executed Windows evidence**: `scripts/prepare.py` verified the pinned submodule and 345 staged assets. `pwsh -NoProfile -File notes/claude-scripts/build.ps1 musescore_mcp` exited 0, archived the engine and host libraries and linked `musescore-mcp.exe` (`build ok`, 59.391 s); xmake warned that the `muse_mpe` root `*.cpp` glob matched nothing. `music_core_test.exe` and `musescore-mcp-test.exe` built successfully. The core test passed existing creation/edit/revision, snapshot, mix and excerpt assertions plus new font advance invalidation, master/excerpt relayout, retained SoundFont selection and owner-process audio isolation assertions. The original core test at `f22e8aa` checked the resource path and initially empty font list, but did not contain the claimed font-advance or SoundFont-selection assertions; the advance assertion previously belonged to the engine `assets_test`.
- **Executed MCP evidence**: `scripts/mcp_smoke.py` passed through the rebuilt host with staged resources and `FluidR3Mono_GM.sf3`: 20 create/close cycles, editing, score/image/metadata exports and audible stereo 44.1 kHz WAV. `scripts/test_async_session.py --test-hooks` passed through `musescore-mcp-test.exe`, including snapshot isolation, audio mix parity, output reservations, cancellation, shutdown and worker cleanup. `scripts/test_runtime_assets_mcp.py` passed through `musescore-mcp.exe`: immutable runtime font/SoundFont copies, worker relayout, different selected audio, captured job font and SoundFont selections, rollback on an invalid replacement and retained-file cleanup. Host tests use the Pixi native DLL directory on `PATH`; see ignored `notes/a5/host-port/` for output.
- **Lane caveat**: The unmodified `scripts/run_lanes.py --only music_core` stopped at superproject line-ending preflight because the pre-existing, otherwise unmodified `scripts/engine_gate.py` has CRLF in this checkout. Running the lane with only that superproject preflight bypassed (the engine preflight stayed enabled) passed both the core binary and the runtime-asset MCP test. This is not a fully green unmodified lane-runner claim.
- **Executed linux-host evidence (parent)**: the host sources of `be0fac9` plus the engine tree of `engrave-15` build on linux-host (`xmake f -m release --cairo_prefix=<pixi native>` then `xmake build musescore_mcp music_core_test musescore_mcp_test`, 183.6 s, exit 0). `music_core_test` (runtime font advance, master/excerpt relayout, SoundFont selection, typed core), `scripts/test_async_session.py --test-hooks`, `scripts/test_runtime_assets_mcp.py` and `scripts/mcp_smoke.py` (20 cycles, audible 44.1 kHz WAV) all exit 0. The host `xmake.lua` needed its own `--cairo_prefix` handling for the engine renderer on a host without system cairo; that option now mirrors the engine's (`<prefix>/include/cairo`, `<prefix>/lib`, rpath off Windows) and is documented in `docs/dev/build.md`. Two tracked files were sitting in the worktree with CRLF despite the committed LF/`-text` bytes (`scripts/engine_gate.py`, `licenses/FluidR3Mono_License.md`); they were restored to the committed bytes, so the lane-runner line-ending preflight condition described above no longer reproduces, although the lane runner was not re-executed end to end.
- **Remaining A5 S2+ work**: Replace the privileged legacy project adapter with owner-thread public `Engine`/`Document` storage and codecs. The `score_export`/`score_export_start` position formats need a documented v2 mapping to `Document::positions` (including repeat-expanded timing and geometry); score-file exports need `ExportOptions::score`/`ScoreSel` for excerpt selection. The v2 runtime interface must explicitly retain live font/SoundFont changes, metadata and worker snapshot semantics while removing the internal asset bridge. Integrate these choices in the registry, schemas, job protocol and async session; preserve captured assets, revisions, cancellation and atomic publication. Resolve `DEFECT-ENGRAVE-009` before finalizing public audio IO error mapping, and carry codec-finalization and leak defects separately. Linux build/test and the A5 review remain the parent's work; this Windows-only host port did not run the engine golden gate.

### 12. Snapshot-based render is not bit-identical to direct render (DEFECT-ENGRAVE-012)

- **Status**: Accepted limitation of the A5 export-job design.
- **Evidence Level**: EXECUTED (A5 S3 tests on Windows).
- **Description**: Export jobs render a captured MSCZ snapshot in a separate worker, so the result comes from a save/reopen round trip rather than the live document. In the S3 parity tests, MSCZ save/reopen rounded the A4 page height by 0.001 point and changed 36 of 2,005,644 PNG pixels for each tested master and excerpt. The tests therefore compare PDF/SVG geometry within 0.02 point and PNG output within a stated pixel tolerance.
- **Impact**: v2 render output must not be advertised or golden-compared as bit-identical to a direct in-process render, and tolerance-based comparisons must be explicit wherever snapshot rendering is validated. No universal parity claim follows from the tested scores.
- **Next Step**: Keep the tolerance documented alongside the export-job tests; if a consumer needs bit-identical output, make a direct-render design decision rather than silently widening tolerances.
- **S5 evidence at `engrave-16`**: The limited S3 tolerance is not a bound on every score. The Windows census independently reopened the public-engine MSCZ and reproduced the host's SVG, PNG and PDF descriptors for 25 source fixtures. Thirty-two specific outputs were within the documented bounds; fourteen exceeded them. All are pinned individually, and the fourteen are accepted as the documented writer-precision round-trip limitation under `DEFECT-ENGRAVE-015`. They do not establish equal rendering. See [the v2 output policy and census](a5-outputs.md).

### 13. Pitch edits can leave a mismatched tie without a diagnostic (DEFECT-ENGRAVE-013)

- **Status**: Carried / Open at `engrave-15`.
- **Evidence Level**: SOURCE-REVIEWED, EXECUTED (Windows, v2 wire).
- **Description**: The public engine contract in `engrave/include/engrave/ops/notes.h` explicitly changes a tied note independently while propagating to its linked score copies. `engrave/src/api/ops/notes.cpp` calls the engine's pitch operation on that note; `HostApi::edit` decodes the batch and passes it to `Document::edit`. This is an engine boundary limitation, not a host conversion of a valid two-end edit. Neither the individual edit nor the export reports that the still-connected endpoints now have different pitches.
- **Executed evidence**: `scripts/test_linked_tie.py` opens `fixtures/golden/linked_parts.mscx`, whose first tied notes are B4 (`I_I` and `L_L`). Changing only `I_I` to D5 changes its linked copies and returns no diagnostic; MusicXML contains a D5 start and B4 stop. On MXL reimport, the connection is lost and the start becomes a `let-ring`. In a fresh document, setting both ends to D5 in one batch preserves the tie after the same MXL round trip. This observes the specified independent-note edit and the consequence of exporting an invalid tie; it does not establish behavior for every tie form or format.
- **Impact**: Clients editing a tie must coordinate both endpoints or risk an undiagnosed loss on interchange. The retired v1 pitch-edit projection rejected partial tied-note edits; v2 does not retain that protection.
- **Next Step**: Decide an engine-level contract for detecting and diagnosing or preventing mismatched ties, including multi-operation batches and linked excerpts, and add public-boundary tests. Until then, retain the focused v2 reproduction and do not claim partial tied-note edits round-trip safely.

### 14. Snapshot rendering changes an unedited score's version header (DEFECT-ENGRAVE-014)

- **Status**: Resolved by contract at `engrave-16`.
- **Evidence Level**: SOURCE-REVIEWED, EXECUTED (Windows, v2 wire and public engine probe).
- **Mechanism**: `capture_export` in `native/api_output.cpp` writes an MSCZ snapshot and the worker opens it. The engine's `Writer::writeScore` writes the running application's `programVersion` and revision into the archive; `export_native_score` restores the owner's in-memory version fields afterwards, but cannot undo the stamp in the serialized bytes. On rendering, `HeaderFooterLayout` expands `$v` to the running version for a dirty score and to `mscoreVersion()` for a clean score. The reopened worker document therefore has the snapshot's 5.0.0 provenance, while the untouched owner still has the source's 4.5.0. The v1 worker protocol carried source version fields separately; the current public engine API exposes `Outline::mscore_version` but no operation to restore source version, revision, file version and dirty state on the reloaded worker document.
- **Executed evidence**: On Windows, `scripts/test_version_info.py` opens `fixtures/version/header.mscx` (file format 4.70, program version 4.5.0). The owner outline remains 4.5.0 after synchronous and background MSCZ exports, rendering and excerpt generation. Both written archives contain `<programVersion>5.0.0</programVersion>`; both reopen with outline version 5.0.0, and their renders show 5.0.0. The unedited and edited owner renders also show 5.0.0 because rendering uses a written snapshot. Earlier direct public-engine probes showed 4.5.0 for the unedited source and 5.0.0 for the reopened archive.
- **Contract**: Exports write the current format version. The owner retains its in-memory source version; a written/reopened snapshot and every render through that snapshot report the written version. The render's version is not evidence that the original source file was saved by that version. This content/provenance distinction is separate from the geometry limitation in `DEFECT-ENGRAVE-012`.

### 15. Snapshot render drift exceeds the accepted bounds (DEFECT-ENGRAVE-015)

- **Status**: Accepted writer-precision round-trip limitation at `engrave-16` (Windows); fourteen exact outputs are individually pinned.
- **Evidence Level**: EXECUTED (Windows, public engine and v2 host), SOURCE-REVIEWED (snapshot/export path).
- **Description**: MSCZ save/reopen can change more than the small geometry and antialiasing differences measured in S3. Fourteen original S5 golden render differences exceed the stated bounds. The complete per-output evidence is in [the census](a5-output-census.json). For `drums_import.musicxml`, the public direct render is 1225 × 1585 and the public render of its reopened MSCZ is 1224 × 1584, exactly matching the host. The common area differs at 22,588 pixels (limit 100); one text y coordinate moves from 158.5907135 to 159.4676361 points. `rendered_page` also reports ink-share ratio 1.2716 and grid L1 0.2764 against limits 1.10 and 0.25. These failures are not covered by the 36-pixel example in -012.
- **Page-size mechanism**: The MusicXML scaling and page dimensions imply approximately 612.000000386 × 792.000000499 points before saving; the serialized style contains exactly 8.5 × 11 inches. Both PDF/SVG page boxes round to 612 × 792 points, but the PNG renderer applies `ceil` at 144 dpi, producing the observed one-pixel difference. This explains the dimensions, not the separate ink/layout difference.
- **Impact and acceptance**: A completed snapshot render can differ in pixel content or geometry from a direct render of the live document. These fourteen exceed the 0.02-point / 0.005%-pixel example and are accepted only for the exact fixture, old baseline, direct-engine and reopened-engine/host hashes and measurements in the census. The golden comparator rejects any other changed normalized output, pixel, glyph or geometry and additionally requires the exact encoded PNG bytes for all eight PNG exceptions. The drums rendered-page exception also pins the PNG file bytes and exact diagnostics. This is a round-trip precision limitation, not a claim that rendering is equal.
- **Follow-up**: Investigate writer numeric precision independently under `DEFECT-ENGRAVE-016`. The size-rounding mechanism is demonstrated; the evidence does not separately isolate every ink or layout movement. No engine change is part of this acceptance.

### 16. Writer numeric precision in save/reopen (DEFECT-ENGRAVE-016)

- **Status**: Carried / Open; independent of A5 acceptance.
- **Evidence Level**: SOURCE-REVIEWED, EXECUTED (Windows, `engrave-16`).
- **Description**: Snapshot serialization rounds render-relevant dimensions, including the approximately 0.001 pt A4 page-height change in the S3 tests and the near-integer page size in the drums S5 fixture. The latter crosses the PNG renderer's `ceil` boundary after reopening. Save/reopen also reproduces the individually measured pixel and geometry drift in [the census](a5-output-census.json); the page-size rounding alone does not prove the cause of every content movement.
- **Next Step**: Independently investigate and improve writer numeric precision with round-trip tests that preserve dimension values and compare direct and reopened renders. Preserve explicit format/version compatibility, async snapshot isolation and the current pinned baselines when evaluating the change.

### 17. Output schemas inherited request bounds (DEFECT-ENGRAVE-017)

- **Status**: Resolved by `2b63260`.
- **Evidence**: **EXECUTED** on Windows: `test_output_schema_boundaries.py` validates actual successful responses for an imported 5,000-character title, 5,000-character part name and 66,000-character lyric, plus an edited score with 261 audio tracks. It separately checks synthetic oversized result arrays against the schema. The generated registry schema passed `--write` and `--check`; input schemas were compared with `a8b030e` and did not change. **SOURCE-REVIEWED**: the derivation and remaining bounds are in [the output bound table](a5-output-schema-bounds.md).

### 18. Residual capture and workspace limits (DEFECT-ENGRAVE-018)

- **Status**: Supported score and position payloads now use the `engrave-17` capture budget and cooperative cancellation; MIDI and total-memory gaps remain open.
- **Executed evidence**: On Windows, the export-job lane rejects 32-byte score, render and audio captures, records and successfully publishes the unbounded MIDI fallback when it fits, rejects it when retained bytes exceed the reservation, and checks position JSON at one byte below and exactly its shared allowance on a 512-measure fixture. The async lane cancels during the engine callback and checks the capture deadline; neither publishes an artifact. The lane also checks that a running no-repeat job retains and releases its worker position allowance.
- **Source-reviewed boundary**: The job's remaining reservation limits MSCX, MSCZ, MusicXML, MXL and position payloads; streamed position JSON shares that reservation with the live payload. Render and audio use a bounded MSCZ, and worker projection positions are bounded. Budgeted MIDI returns `Unsupported` and is retried unbudgeted; the snapshot and internal job status record that fact, and retained MIDI is checked against the reservation after capture. MIDI graph construction is neither bounded nor interruptible. Layout, repeat-list creation and deflate cannot always be interrupted. The engine's score-writer allowance and estimate exclude layout, repeat lists, archive metadata, encoder internals and temporary strings; positions additionally use the documented index bound, and container capacity and allocator overhead are not counted. The reservation is not a process-memory bound. See [the registry limits](a5-registry.md).
- **Next step**: Bound/cancel MIDI playback-graph construction and establish measured or enforceable process-workspace limits, including the exclusions above. Keep the engine's working-buffer estimate distinct from an allocation guarantee.

### 19. Audio temporary output can exceed its byte budget (DEFECT-ENGRAVE-019)

- **Status**: Resolved by host adoption of the `engrave-17` limits for encoded audio file extent and cumulative writes.
- **Executed evidence**: On Windows, a no-repeat WAV whose duration cannot be projected hit `audio intermediate byte limit exceeded` one byte below its required file extent, left no cache or staged artifact, and succeeded at the required extent. Another WAV rendered under two realistic limits had the same SHA-256. The live default-budget 1,000-repeat fixture still succeeds with repeats disabled; the existing audio budget and live-font controls pass.
- **Source-reviewed boundary**: `max_bytes=floor(output_bytes/2)` limits the engine cache as it grows, and `max_write_bytes` adds 1 MiB for header rewrites. The engine returns `ResourceLimit`, removes the intermediate and does not call the sink on overrun. The engine retains the cache during sink delivery, so the stage is independently limited to the same half; their peak combined logical file size is at most `2*floor(output_bytes/2)`. This halves the maximum encoded audio size under the configured job budget. The sampled monitor is defence in depth; its polling interval is not an overshoot guarantee. Other cache files, filesystem overhead, and general process memory are outside this logical audio-file quota. Cleanup after worker termination remains best effort.
- **Projection limitation**: With repeats disabled the public API cannot always provide the renderer's end time; if the unexpanded end is not an identifiable expanded measure boundary, early projection is omitted and the engine quota and sink remain the guards. See -022 and [the registry limits](a5-registry.md).
- **Conservative FLAC projection (accepted limitation)**: the FLAC preflight estimate is deliberately conservative. **EXECUTED** in review: under a private 10,000,000-byte worker budget, the FLAC preflight rejected a file that the enforced extent quota produced at 4,404,855 bytes when projection was skipped. No rejection under the default 1 GiB job budget (512 MiB audio half-budget) was reproduced. The projection therefore protects the quota at the cost of rejecting some lossless files that would have fit; clients that hit it can raise the job output budget.

### 20. Snapshot pins fell through to legacy allowances (DEFECT-ENGRAVE-020)

- **Status**: Resolved by `8a5794e`.
- **Evidence**: **EXECUTED** on Windows: the oracle harness exercises both `pedal.mscx` SVG and PDF entries using their real recorded migration and A3 allowance hashes. With marker bytes mapped to those hashes, each older allowance passes without a v2 snapshot pin and fails with the pin; the pinned host hash passes and a missing output fails. **SOURCE-REVIEWED**: the full comparator and v2 overlap check apply snapshot precedence before equality or older allowances. Existing audio tolerances and other allowances are unchanged.

### 21. Runtime font cache breaks Linux audio monitoring (DEFECT-ENGRAVE-021)

- **Status**: Resolved by `cfa0671`. The A5 re-review (`notes/a5/s8/REVIEW-TERRA-2.md`, section 3.A) reproduced an `Is a directory` worker failure after a runtime font was registered.
- **Resolution**: The monitor sums only regular, non-symlink direct children of the worker cache whose names start with `engrave-audio-`, the pinned engine's encoded-audio filename prefix. It does not recurse, count runtime font directories, unrelated cache files, symlinks, sockets or other non-regular entries. The worker separately limits the staged output; the monitor does not sum that copy with the cache file.
- **Evidence**: **EXECUTED** on Windows and physical Linux `linux-host` at `3ae0d29`: live host WAV exports without a font and after `runtime_info` registers Petaluma Script both succeed under the default budget; the export-job lane also ignores unrelated cache entries, stops a growing named audio cache file, and rejects a real oversized audio job. On `linux-host`, the same new live-font test against the `626d0bb` host failed with `Is a directory` after its no-font control succeeded. **SOURCE-REVIEWED**: the pinned engine writes its encoded temporary audio as an `engrave-audio-<pid>-<index>.<format>` direct child of its cache directory. The polling interval is not itself a hard quota; the writer limit described in -019 now enforces it.

### 22. No-repeat audio projected using repeat-expanded duration (DEFECT-ENGRAVE-022)

- **Status**: The false rejection reproduced in the A5 re-review (`notes/a5/s8/REVIEW-TERRA-2.md`, section 3.B) is resolved by `3ae0d29`.
- **Resolution**: Repeat-enabled exports retain the expanded outline estimate. For repeat-disabled exports, the worker uses the engine's expanded-timeline position at the unexpanded end tick when the public measure positions identify that boundary; otherwise it skips early projection instead of using the wrong duration. The enforced engine quota and sink limit still apply. This fallback is not an exact preflight for every score.
- **Evidence**: **EXECUTED** on Windows and physical Linux `linux-host` at `3ae0d29`: the 1,000-repeat fixture now produces a 2,469,646-byte WAV under the default limit without repeats and is rejected with repeats enabled. At four repeats, both modes render and their WAV projections are 180,450 bytes above the encoded sizes, within the documented rounding/header margin; a limit one byte below each projection is rejected. A second fixture whose unexpanded end falls inside an expanded measure succeeds via the fallback at its exact encoded-size limit. On `linux-host`, the 1,000-repeat no-repeat test against the `626d0bb` host failed with the original projected-audio `ResourceLimit`. **SOURCE-REVIEWED**: the pinned renderer derives the unexpanded end tick from `repeatList(false)` but converts it using the expanded tempo timeline. The public positions API supplies the time only at exported measure starts and can omit a one-to-one measure mapping.
### 23. Stave-sharing labels are lost on 5.00 open/export (DEFECT-ENGRAVE-023)

- **Status**: Open; blocks the phase-B B470_STAVE_SHARING_LABEL_DROPPED warning.
- **Evidence**: **EXECUTED** in B1 on Windows: the current 5.00 host opens `tests/phase_b/stave_labels.source.mscx` (a score whose staves share a label) and re-exports 5.00 with both labels silently absent. The preserved 4.7.6 oracle rejects the genuine 5.00 source with `[2007]`, and the target fixture it can read has no labels.
- **Impact**: This is data loss on an ordinary 5.00 open/export round trip, and it makes the phase-B warning contract unimplementable for this feature: a writer cannot warn about an object that is already gone from the DOM. Downgrading such a score cannot report what was lost, which is exactly the case the warning channel exists for.
- **Next step**: Retain the stave-sharing labels through ingestion and the 5.00 writer (preferred), or reject the document on open with a diagnostic when the engine cannot represent them. Either route needs a reopen test and a phase-B fixture update; do not paper over it with a writer-side warning.
- **Resolution (`engrave-21`, commit `542d3261ad`)**: the labels are now retained through 5.00 ingestion and export, and reopen tests confirm both labels survive. This intentionally changes the 5.00 output of a label-bearing score; no pinned golden covers such a score, and the ordinary 5.00 controls remain byte-identical apart from the recorded program revision. The phase-B `B470_STAVE_SHARING_LABEL_DROPPED` warning is therefore now emitted for the 4.70 export of a score that actually has a label, which is the case the warning existed for. EXECUTED on Windows and linux-host.

### 24. 4.70 cannot represent part EIDs (DEFECT-ENGRAVE-024)

- **Status**: Accepted format limitation, scoped in the phase-B contract.
- **Evidence**: **EXECUTED** in the B5/B6 fixtures and the ngrave-23 regression: a shared-part downgrade keeps all four numeric Part ids 1–4, four ordinary parts reopen, and all four source part EIDs are absent. The 4.70 Part reader/writer has numeric ids and no interoperable part-EID field, so no private extension was invented.
- **Impact**: "IDs retained" in the earlier B1 wording overstated the contract for parts. Every representable element EID (score, staff, measure, clef, time signature, chord, note, spanner, …) is preserved; part EIDs are the documented exception. A consumer that keys parts by EID must fall back to the numeric part id in 4.70 files.
- **Next step**: Keep docs/dev/phase-b-features.md as the authority for which IDs survive; revisit only if a future 4.70 writer gains an interoperable part-identity field.

### 25. 4.70 export refuses scores with generated accidentals (DEFECT-ENGRAVE-025)

- **Status**: Open; recorded as a bounded-contract gap, not a silent failure.
- **Evidence**: **EXECUTED** in the phase-B live-edit evidence: a paired edit to C-sharp5 generated four accidentals, and the 4.70 export refused with B470_UNSUPPORTED_FEATURE (no artifact). Windows saw the same refusal in its initial capture; the accepted live scenario deliberately uses B4 to C5. The refused export carries its warning, so a client is not handed a downgraded file silently.
- **Impact**: Scores that need generated accidentals in 4.70 are refused rather than degraded. Any agent workflow that produces chromatic alterations before a 4.70 export hits this.
- **Next step**: Implement accidental conversion (or an explicit degraded form with a frozen warning identity) and add a fixture; until then the phase-B contract must keep listing generated accidentals as outside the accepted conversion set.