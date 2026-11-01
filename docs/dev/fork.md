# MuseScore fork

The engine is consumed from the fork https://github.com/Locietta/engrave as the
submodule `vendor/musescore` (shallow, pinned by SHA with an `engrave-<n>` tag per pin).
The current pin is `engrave-14` (`ab9e33c39b`).

The fork operates as a forward-only hard fork. Its ancestral upstream provenance
terminates at `upstream-final` (`cbffaa078d`, syncing upstream MuseScore `32f0a26e5b` and
framework `da3885cb`). Since `upstream-final`, development on `main` moves forward strictly
via normal reviewed feature merges and commits, with no scheduled upstream merges and no
force-pushes or rebases of published history. Reference branches `upstream/musescore`
and `upstream/muse-framework` are frozen. Upstream fixes arrive only by cherry-pick/adapt
with upstream SHA, framework origin if applicable, technical rationale, and focused test
evidence.

For governance details, see [Rules (Hard-Fork Governance)](#rules-hard-fork-governance).
For all milestone pins and commit history, see [Ledger: main](#ledger-main).

## Engine build

Since A4 the fork carries the engine's standalone Qt-free xmake build, and since C7 the pinned asset manifest, so the engine builds as its own library and the superproject only includes it:

- `xmake.lua` — the standalone project: `xmake f -m release --cairo_prefix=PREFIX && xmake build` builds the production engine targets (`engrave` and its module libraries) in a bare checkout.
- `xmake/engrave.lua` — the engine targets (`muse_global`, `muse_draw`, `muse_mpe`, `engraving`, `iex_musicxml`, `iex_mnx`, `iex_guitarpro`, `iex_midi`, `native_fluidsynth`, `muse_audio_engine`, and `engrave`), generated version metadata (`engrave_version.h`), and pinned package requirements. Targets compile via folder globs.
- `xmake/tests.lua` — opt-in test targets (`--engrave_tests=y`) and the `c0_gate` phony target that builds the public-API, import, playback and adapter regression suites. `scripts/engine_gate.py` builds and executes the gate and artifact comparisons; it does not yet launch `diagnostics_test`, `io_boundary_test`, the installer tests or manifest verification (see `docs/dev/defects.md`, DEFECT-ENGRAVE-010).
- `xmake/packages/` — the pinned third-party recipes (`freetype`, `harfbuzz`, `flac`, `lame`, `opus`, `opusenc`, `fdk-aac`, `pugixml`, `utfcpp`, `picojson`, `mnx-json-schema`, `mnxdom`, plus `zlib` for standalone builds).
- `engrave/assets.json` & `tools/install_assets.py` — the versioned SHA256/size asset manifest (345 pinned files including fonts, instruments, templates, MPE profiles, and licenses) and standalone asset installer replacing legacy `.qrc` lists.

## Rules (Hard-Fork Governance)

Following the final upstream sync (`upstream-final` at `cbffaa078d`, upstream `32f0a26e5b`, framework `da3885cb`), `engrave` operates as a self-contained hard fork:

- **Forward-only history**: `main` moves forward strictly via normal reviewed feature merges and commits. Force-pushes and rebases of published history are prohibited.
- **Pins and provenance**: Every engine state pinned by `vendor/musescore` in the superproject carries an `engrave-<n>` tag on `main` and a ledger row recording its commit, delta summary, and validation evidence on both Windows and Linux (`linux-host`). The final synced upstream (`32f0a26e5b`) and framework (`da3885cb`) SHAs remain ancestral provenance.
- **Frozen upstream reference branches**: `upstream/musescore` and `upstream/muse-framework` are frozen reference branches. There are no scheduled upstream merges, forced updates, subtree-equality checks, or "tool-made merge commits only" restrictions. Specific upstream commits may be fetched and cherry-picked/adapted on demand with their upstream SHA, framework origin (if applicable), rationale, and focused test evidence.
- **Historical sync & trim artifacts**: `tools/sync-upstream.py` and its synthetic recovery suite (`tools/test_sync_upstream.py`) are archived in Git history at `upstream-final`..`engrave-13` and removed from the active tree in C8. `TRIM` is retained as historical deletion provenance for A3 batches 1–8 and C1 `drop-3x`, not an active path-denial list for new code.
- **Asset & license provenance**: Runtime assets and third-party font/engine licenses are pinned by SHA256 and byte size in `engrave/assets.json` and installed/verified via `tools/install_assets.py`. GPL-3.0 notices and third-party license files are preserved.
- **Validation**: Every pin requires green `scripts/engine_gate.py` validation across supported input corpora (5.00 MSCZ/MSCX, MusicXML/MXL, MIDI, Guitar Pro/PowerTab, MNX) and render/audio goldens on both native Windows and physical Linux (`linux-host`).
- **Practicalities**:
  - Push over SSH (`git@github.com:Locietta/engrave.git`). Pushing large histories over HTTPS fails in this environment with HTTP 408.
  - Line endings on Windows: work clones require `core.eol=lf` and `core.autocrlf=false` because `.gitattributes` declares `* text=auto`.
  - The orchestrator's local work clone is `.upstream/engrave` (with per-task worktrees under `.upstream/engrave-wt/`).

## Ledger: main

Foundation commits and pinned releases on `main`. Pins start at A2 (`engrave-1`).

| Pin / Milestone | Commit | Upstream SHA | Framework SHA | Notes |
| --- | --- | --- | --- | --- |
| absorb (A1 baseline) | `7150f2b05e` | `0ea613f` | `ecdc6e7` | Not a pin. Absorbed muse_framework into `muse/` via subtree. Base for A1/A2. |
| main tip (A1 accepted) | `0adbf67ed7` | `0ea613f` | `ecdc6e7` | Not a pin. Hardened sync tool and 29-step test suite; accepted by Terra. |
| `engrave-1` | `3a0fff5731` | `0ea613f` | `ecdc6e7` | First pin. Merges `a2/port` (`2c6b77282f`, the 5.0 port at parity) into `main`; the tree differs from `2c6b77282f` only in `tools/`. Validated on Windows and linux-host (superproject `engrave/a2` `c090d81`); superproject pins it at `abc13ca`. |
| `engrave-2` | `e9136ee340` | `2abc2de` | `ecdc6e7` | A3 hard trim: 8 deletion commits `9857df5da2..251cf4662c` (9621 files removed, each built cold on linux-host with all lanes passing), sync tool fixes (framework `TRIM`, classified resolutions; merge `5e30174935`), then sync of upstream `0ea613f..2abc2de` (15 trimmed changes discarded, 0 additions dropped; `parts_tests.cpp` keeps the A2 Qt-free topic). Validated on Windows and linux-host (superproject `engrave/a3` `d950834`). Kept on purpose: `src/importexport/mnx/` (uncompiled, future build option), `share/templates/` (A4 create-from-template), the CMake files the manifest generator reads, and test fixtures used by the lanes. |
| `engrave-3` | `f00d6cbd02` | `2abc2de` | `ecdc6e7` | A4 S0. Merges `a4/s0-build` (`1455abe210`): the engine carries its own xmake build (`xmake.lua`, `xmake/engrave.lua`, `xmake/manifest.lua`, `xmake/packages/`, `xmake/tests.lua`) and the manifest generator (`tools/`). Tree equals `1455abe210`. Validated on Windows and linux-host (superproject `engrave/a4` `da49ca6`, report `notes/a4/s0/REPORT.md`): fork standalone and superproject cold builds, 29/29 lanes, goldens unchanged. |
| `engrave-4` | `d8c21df149` | `2abc2de` | `ecdc6e7` | A4 C++ API. Merges `a4/int` (`d382aad91c`): public headers `engrave/include/engrave/`, library and tests under `engrave/`; the xmake build uses folder globs, so the manifest and its generator are gone, and so are the CMake files it read (trim batch 7; batch 8 removes more uncompiled sources). Engine patches, all goldens identical: EID fixes F1-F3, the `Transaction::push` hook (F4), a PowerTab empty chord becomes a rest, a `layoutPageView` guard, a figured-bass `layoutLines` runaway, a tuplet `doUndoAddElement` marker, and a GPX back-reference that repeats its pattern. Validated on Windows and linux-host (superproject `engrave/a4-int` `cb31e46`, report `notes/a4/validation/FINAL.md`): cold builds, 29/29 lanes on both hosts, audio paired both ways, address-only ASan check (`detect_leaks=0`, not a leak-clean baseline; see `docs/dev/defects.md`), goldens 308/55/0. Accepted with one relaxed guarantee: a failed batch that ran a Snapshot-policy operation is reloaded and may move layout (user decision 2026-10-31; see `docs/dev/defects.md`). |
| `upstream-final` | `cbffaa078d` | `32f0a26e5b` | `da3885cb` | Not a pin. The last upstream sync (9 files changed after trim, no conflicts); the fork is a hard fork from here and takes upstream fixes by cherry-pick only. Checked on Windows (fork suites, goldens 308/55/0) and linux-host (build only), report `notes/sync32f0/REPORT.md`. |
| `engrave-5` | `23c743d339` | `32f0a26e5b` | `da3885cb` | Phase C step C1. Merges `a4/drop-3x` (`41565474d6`): the readers for scores before MuseScore 4.6 (read114, read206, read302, read400, read410) are deleted and listed in `TRIM`; such scores fail with `Unsupported`. Fixtures were upgraded once to the current format (`native-format-upgrade.json`). Report `notes/drop3x/REPORT.md`; the merge was built on Windows and passed the ops, handles, op-policy and api suites. |
| `engrave-6` | `a05c3bc3f5` | `32f0a26e5b` | `da3885cb` | Phase C step C0. Merges `c0/gate` (`bdbb77f87a`, tag `engrave-c0-gate`): test capture and build closure only, production engine sources equal engrave-5. The engine gate `scripts/engine_gate.py` (usage and coverage `docs/dev/c0-gate.md`) checks the engine through the public API; green on Windows and linux-host, and all 211 overlapping artifacts equal the v1 host's. The v1 host is frozen: its targets are opt-in, binaries and replay runner archived under ignored `artifacts/frozen-v1/`. Report `notes/cleanup/c0/REPORT.md`. |
| `engrave-7` | `01af7d3694` | `32f0a26e5b` | `da3885cb` | Phase C steps C2 and C4. C2 (`c2/build`, `dbeb65122e`): tests and probes are opt-in (`--engrave_tests=y`), version metadata in one TU, dead configuration arms removed. C4 (`c4/audio`, `015de81467`): the audio RPC/controller layers are replaced by one offline render session; synthesis, mixer, DSP and all five encoders unchanged. The merged tree passed the engine gate on Windows and linux-host with 0 golden differences. Reports `notes/cleanup/c2/REPORT.md`, `notes/cleanup/c4/REPORT.md`. |
| `engrave-8` | `ab9321e771` | `32f0a26e5b` | `da3885cb` | Phase C step C3 (`c3/inert`, `f8c434e664`; merged tree equals it): a small stderr diagnostic sink replaces kors logger/profiler, assertions get their own header, and disabled Qt/accessibility/plugin arms are deleted. Touching `log.h` rebuilds 340 TUs instead of 534. Gate green on Windows and linux-host, 0 golden differences. Report `notes/cleanup/c3/REPORT.md`. |
| `engrave-9` | `fe0b7c6a06` | `32f0a26e5b` | `da3885cb` | MNX import (`mnx/import`, `6ef70fe4fd`; merged tree equals it): pinned mnxdom and json-schema-validator packages, `Format::Mnx`, and 38 test inputs. Import only. Report `notes/mnx/REPORT.md`. |
| `engrave-10` | `d385dfc1a8` | `32f0a26e5b` | `da3885cb` | Reader fix (`fix/hidden-tuplet-read`, `3c9eacc498`): read460/read500 restore a leading gap rest inside a tuplet before `checkMeasure()`, which otherwise filled over the following notes (found via the MNX reference `tupletHiddenRest_ref.mscx`). Focused read test; gate green on both hosts with no golden changes. Report `notes/readfix/REPORT.md`. |
| `engrave-11` | `fd37009ebd` | `32f0a26e5b` | `da3885cb` | Phase C step C5 (`c5/context`, `d3bb8e38d1`; merged tree equals it): `EngineContext` owns filesystem, fonts, engraving services, MPE profiles and offline audio in fixed init/teardown order; `kors_modularity`, `kors_async`, `NoQtApplication` and all IoC/async headers are deleted (0 remaining includes, 654 -> 645 production TUs). Gate green on Windows and linux-host with 0 golden differences. Report `notes/cleanup/c5/REPORT.md`. |
| `engrave-12` | `d0f1db9b51` | `32f0a26e5b` | `da3885cb` | Phase C step C6 (`c6/interaction`, `27424c924a`; merged tree equals it): target-based note/duration edits without `Selection`/`InputState` choreography, startup dummy palette `MasterScore` and `PaletteScoreProvider` removed, and unreachable desktop drag/grip/view/key-event methods deleted (645 -> 640 production TUs, -6,249 source lines, `score.h` rebuild 374 -> 371 TUs). Gate green on Windows and linux-host with 0 golden differences. Report `notes/cleanup/c6/REPORT.md`. |
| `engrave-13` | `b685f34af2` | `32f0a26e5b` | `da3885cb` | Phase C step C7 (`c7/types-assets`, `2d31bee3f9`; merged tree equals it): `ByteView` (`std::span<const uint8_t>`) at Guitar Pro and audio archive input boundaries, checked native `StreamFile` IO propagating seek/write/finalization errors, standard UTF-8 path conversion at native runtime boundaries, CJK + non-BMP `U+1D11E` + emoji filename/lyrics roundtrip and render tests, and a single hash-pinned standalone asset manifest (`engrave/assets.json` + `tools/install_assets.py`, 345 files including 14 licenses) retiring the 15 `.qrc` lists. Gate green on Windows and linux-host with 0 golden differences. Report `notes/cleanup/c7/REPORT.md`. |
| `engrave-14` | `ab9e33c39b` | `32f0a26e5b` | `da3885cb` | Phase C step C8 (`c8/close`, `21069dc360`; merged tree equals it): closes Phase C cleanup by retiring `tools/sync-upstream.py` and `tools/test_sync_upstream.py` in Git history, deleting 60 unreferenced framework/draw/audio/concurrency/thumbnail files and `xmake/framework_config.h` (640 -> 625 production TUs, -15,790 net lines; cumulative C0 -> C8: 661 -> 625 TUs, -13,657 TU lines while adding MNX import; uncached `log.h` rebuild 334 TUs, `score.h` rebuild 342 TUs). Gate green on Windows and linux-host with 0 golden differences; ASan address checks 24/24 pass on linux-host (LSan findings documented in `docs/dev/defects.md`; formal Phase C acceptance pending the Astra phase review). Report `notes/cleanup/c8/REPORT.md`. Formal Phase C acceptance: GPT-6 Astra, ACCEPT with no must-fix item, 2026-11-01 (`notes/cleanup/c8/REVIEW-ASTRA.md`); open defects tracked in `docs/dev/defects.md`. |

| `engrave-15` | `28e6c8ffb6` | `32f0a26e5b` | `da3885cb` | A5 parked API (`a5/api-gaps-14`, `bfe86bd090`; this merge has its tree): `Engine::add_font`/`Engine::set_soundfonts` retain copies and invalidate font metrics/layout and the SoundFont selection for existing documents; `Document::positions` exposes exact 1200-DPI page-local measure/segment geometry and repeat-expanded event times without changing the revision; `ExportOptions::score` exports one excerpt as an independent score (MSCX/MSCZ/MusicXML/MXL/MIDI) with link remapping and style/metadata preservation. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0) at `bfe86bd`, where the gate now also executes `diagnostics_test`, `io_boundary_test`, the installer unit tests and manifest verification. Validation log `.upstream/engrave-wt/a5-api14/build/VERIFICATION.md`; evidence under ignored `notes/api14/`. |

| `engrave-16` | `5fd08f45cb` | `32f0a26e5b` | `da3885cb` | Engine follow-up batch (`fix/audio-io-and-assets`, `a504476c3b`; this merge has its tree): audio storage failures return public `ErrorCode::Io` through an internal IO category, encoder finalization carries a `muse::Ret` status and preserves the first encode/finalize error (injected-failure test followed by a byte-identical render), `set_soundfonts` validates eagerly but defers audio startup and live-handle refresh until rendering, and `Engine::runtime_fonts()`/`RuntimeFontInfo` expose retained runtime font metadata. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0) at `a504476` with success-path audio and render bytes unchanged. |

| `engrave-17` | `2386f06d5f` | `32f0a26e5b` | `da3885cb` | Host resource budgets (`feat/byte-budgets`, `b89d44aa0e`; this merge has its tree): `CaptureOptions` gives score-file and position capture a byte budget plus cancellation (`ErrorCode::Cancelled`, position sizing helpers), and `AudioOptions::max_bytes`/`max_write_bytes` are enforced by the audio file device before writing, deleting the intermediate file and publishing nothing on failure. MIDI capture returns `Unsupported` for bounded capture; the workspace estimate is not a hard process-memory bound. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0) with unchanged success-path codec hashes. |

| `engrave-18` | `7cbeaa52cf` | `32f0a26e5b` | `da3885cb` | API correctness from the A6 API review (`fix/api-correctness`, `4a7043b6f2`; this merge has its tree): segment addresses validate full-width tick terms and reject out-of-range or non-positive denominators with `InvalidArgument` (missing measure still `NotFound`, unmatched segment still `StaleHandle`); query tick comparisons are exact and overflow-free with denominator validation and widened measure-window arithmetic; public `Fraction` ordering/equality compares musical value while representation-sensitive property-default comparison stays component-wise; both audio methods return `Poisoned` before touching score or sink; excerpt/property/render/SoundFont/rollback doc comments corrected. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-19` | `45ea71c664` | `32f0a26e5b` | `da3885cb` | API documentation/policy fix (`fix/rollback-contract`, `73d827ac1d`; this merge has its tree): the `edit()` comment now states the real rollback contract — layout is kept exactly only when the batch ran in one transaction whose unwind was trusted (engine did not throw, no Snapshot-policy operation ran) and the unwound score matched the saved copy; otherwise the document reloads that copy (or undoes the recorded command), which can move elements. The empty-SoundFont error now says no SoundFont is selected rather than claiming the engine was created without one. Comment/message only; no behaviour change. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-20` | `7776f06eb4` | `32f0a26e5b` | `da3885cb` | Phase B writer routing (`feat/write470-routing`, `84b04f2cb3`; this merge has its tree): `ExportOptions::native_version` defaults to 5.00 and accepts 4.70 for MSCX/MSCZ, other versions return `Unsupported`; `export_score_with_warnings()` returns bytes plus structured warnings with stable codes, text and an optional source element id, and a refusing export carries them in `Error::export_warnings`; the byte-only API refuses a warning-bearing export instead of discarding warnings. The 4.70 route converts instrument/part names and applies a semantic guard, accepting a narrow verified subset and refusing everything else with warnings (`engrave/WRITE470.md`). Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0); the preserved 4.7.6 oracle opened the minimal 4.70 fixture produced on both hosts. |

| `engrave-21` | `e2c5fe0f36` | `32f0a26e5b` | `da3885cb` | Phase B conversions (`feat/write470-conversions`, `ae51b6826b`; this merge has its tree): 4.70 export now preserves notes, names, IDs, ordinary braces, half-note ties (including a paired pitch edit) and the bounded pedal case — oracle semantics match the frozen fixtures — and warns with the frozen `B470_*` codes carrying `{kind,id}` records for shared parts, stave-sharing labels, page locks, group brackets, instrument group-name overrides, unequal-pitch ties, unsupported features and unsupported target versions. Stave-sharing labels are now retained through 5.00 ingest and export (their 5.00 output changes by design; no golden covers a label-bearing score). Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). Still refused with warnings: general rhythms/instruments, styles, excerpts and the five degraded feature families. |

| `engrave-22` | `57da9e66ca` | `32f0a26e5b` | `da3885cb` | Phase B degradations (`feat/write470-degrade`, `b92d90904c`; this merge has its tree): excerpts are preserved (`[]` warnings) and the five degraded families now export as genuine 4.70 scores with the exact frozen warning arrays instead of refusing — shared parts (`B470_SHARED_PART_DROPPED` `P_P`/`R_R`), stave-sharing labels (`H_H`/`I_I`), page locks (`C_C..C_C`/`H_H..H_H`), group brackets (`B_B:1`/`B_B:2`, ordinary brace kept) and instrument group-name overrides (one `B470_INSTRUMENT_GROUP_NAME_DROPPED` for part `P_P`); 26 exports per platform match the preserved oracle, focused tests 9/9, and cross-build 5.00 bytes differ only in `programRevision`. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-23` | `c0aabb421c` | `32f0a26e5b` | `da3885cb` | Phase B must-fix round (`fix/write470-gaps`, `559cc84545`; this merge has its tree): the invalid-tie guard now covers hidden and staff-hidden parts, an unaddressable tie uses the nearest source identity or the documented non-EID `locator:…` form instead of a fabricated sentinel (empty/sentinel/malformed identities are `InvalidArgument` with no artifact), and active instrument group naming emits the frozen `B470_INSTRUMENT_GROUP_NAME_DROPPED`. Part EIDs remain unrepresentable in 4.70 (numeric part IDs survive; `DEFECT-ENGRAVE-024`). Focused tests 14/14, API 21/21, 26 oracle exports plus paired-edit/refusal controls with all 87 archive checksums verified. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-24` | `51db3ae00b` | `32f0a26e5b` | `da3885cb` | Phase B identity hardening (`test/warning-identity`, `7f659c8ec0`; this merge has its tree): twelve negative warning-identity cases — empty kind, empty id, sentinel id, malformed range key, malformed bracket key, garbage and incomplete locator — reject as `InvalidArgument` with no output in both formats, so a meaningless warning can never be published. Test-only; no production behaviour change. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-25` | `ac0876b0e4` | `32f0a26e5b` | `da3885cb` | 4.70 excerpt selection (`feat/write470-excerpt`, `82ffad2d1b`; this merge has its tree): a named excerpt now exports as a standalone genuine 4.70 score in MSCX and MSCZ — `excerpts.mscx` `v_v` and `linked_parts.mscx` `v_v` both export with `[]` warnings, retain their body/notes/grace ties/names/links, and were reopened successfully by the preserved oracle and re-exported by the verified old Windows 4.7.6 host (the old host's own MSCX export drops nested excerpts; MSCZ retains them). The 5.00 writer and master conversion mapping are unchanged. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-26` | `fd25d800bf` | `32f0a26e5b` | `da3885cb` | Interruptible 4.70 conversion (`feat/write470-cancel`, `0ed6e4d9a9`; this merge has its tree): the converted XML polls the capture cancellation callback every ≤4 KiB, so a client cancel or deadline aborts mid-conversion with `Cancelled` and publishes nothing, while the working/output budgets stay enforced. The host's request cancellation reaches that callback; eight host cases observed the abort at serialization unit 2, destinations stayed absent, `-32800`/`DeadlineExceeded` were returned as documented, and a following export in the same session succeeded. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-27` | `c0b6eee825` | `32f0a26e5b` | `da3885cb` | Build portability (`fix/climits`, `5ef76556a5`; this merge has its tree): the translation units that used integer-limit macros now include `<climits>` directly, so a canonical Linux build needs no `-include climits` workaround. No behaviour change. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

| `engrave-28` | `451143b716` | `32f0a26e5b` | `da3885cb` | Cooperative cancellation for long operations (`feat/cancel-hooks`, `f3563d22cf`; this merge has its tree): `RenderOptions` and `AudioOptions` gain a `std::function<bool()> cancelled` following `CaptureOptions`' owner-thread/no-reentry contract. Render checks before drawing, at page boundaries, before final encoding and afterward (`score_render.cpp:37,95,98,104,110`); audio checks before each synthesis block, before encoding, around final flush (`soundtrackwriter.cpp:161,182,209,167,195,215,124,129`) and every encoder file write (`streamfile.h:78`). Aborts map to `Cancelled` and clean intermediates. Public signatures unchanged, std-only headers. Windows and linux-host suites pass (30 output/audio tests, IO suite, frozen 5.00 byte checks, eight 4.70 captures); granularity tests prove exact abort points and byte-identical recovery. Gate green on Windows (213 identical/38 allowed/0 different/0 missing) and linux-host (186/65/0/0). |

The superproject's `.gitmodules` now tracks `branch = main`.

## History: mcp/v4.7.x (frozen)

webmscore and the `patches/*.patch` queue are retired; the last executable pre-fork state is the local tag `pre-fork-baseline` (`f8e7d26`). The branches `mcp/v4.7.5` and `mcp/v4.7.6` and their `mcp-*` tags represent frozen pre-hard-fork history.

### `mcp/v4.7.5` (base `v4.7.5` = `3654226c`), tag `mcp-4.7.5-1` = `e70fdef0`

| Commit | Topic | Origin |
| --- | --- | --- |
| `40ca8e33` | engraving: EngravingConfiguration builds without Qt and UI (NO_QT_SUPPORT branches; Qt builds unchanged) | our patch + webmscore header |
| `699f3142` | mpe: articulation profiles read with muse JSON instead of QJson | our patch |
| `35706a70` | midi: Qt-free MIDI export and MidiFile (also in Qt builds: truncated headers throw "unexpected EOF", write failures are reported) | our patch |
| `b53201e7` | midi: Qt-free MIDI import | our patch |
| `12139a10` | guitarpro: import fixes (GPX nesting, bounds, PTB) and Qt-free build | our patch |
| `244d9969` | global: harden ZIP container parsing and read-only seek | our patch |
| `9894398d` | draw: select the requested face from TrueType collections | our patch |
| `78760f6d` | engraving: `UndoStack::clearHistory` | our patch |
| `66894604` | build: missing includes, `RenderingContext` aggregate again (brace-init in buildRenderingCtx), `unsigned short` for `ushort` | our patch |
| `e70fdef0` | TEMPORARY: no `IApplication` version query in MSCX/MusicXML writers | webmscore; to be reverted in roadmap step 3 |

Dropped from webmscore (source-reviewed, then compiled and executed without them):
EngravingItem bbox shim, `PrintStackTrace` assert hook, profiler `TIMER_*` arity,
public `Ret::m_data`, `mpemodule` comments, `__EMSCRIPTEN__` guards, renamed font
and resource paths (`/fonts/*.woff2`, `:/mpe/*`, `FinaleMaestroText-Regular.otf`),
the brotli submodule with `FT_CONFIG_OPTION_USE_BROTLI`, and every hunk in files the
native build does not compile (CMake, web/, audio module, notation, playback, project).

Only the tip is built natively (the native build needs every topic); intermediate
commits are not built on their own, and no Qt build of the fork is run.
`.gitmodules` records `branch = mcp/v4.7.6` so `git submodule update --remote`
stays on the pinned line.

Evidence for this pin is in validation.md ("Roadmap step 1").

### `mcp/v4.7.6` (base `v4.7.6`), tag `mcp-4.7.6-1` = `30387998`

The ten `mcp/v4.7.5` topics cherry-picked without conflicts;
`git range-diff v4.7.5..mcp-4.7.5-1 v4.7.6..mcp/v4.7.6` reports every patch
identical (`=`). v4.7.6 adds no source file to the compiled modules.
Tag `mcp-4.7.6-2` = `e099f16b` adds `Revert "TEMPORARY: ..."` on `mcp/v4.7.6`: the host
registers an `IApplication` (native/noqt_application.cpp), so the writers report the
real version again. `mcp/v4.7.6` now carries the nine permanent topics only (net).
Later pins on `mcp/v4.7.6` (no rebase; each adds commits on top):
- `mcp-4.7.6-3` = `811634a1`: `bca4331a` audio: `RESOURCE_TYPE_MAP` holds `muse::String` (the
  only Qt type on the audio engine side); `811634a1` engraving/tests: UTF-16 literals in the
  playback tests so they build without Qt.
- `mcp-4.7.6-4` = `6cd717f2`: `d653005f` audio: fixed Ogg stream serial (libopusenc drew it
  from `rand()`; MSVC's `rand()` state is per-thread, while glibc's is process-global, so
  repeated Linux exports differed in the serial while Windows happened to repeat);
  `6cd717f2` opusenc:
  initialize the OpusTags vendor string (the vendored copy passed an uninitialized stack
  buffer; released libopusenc 0.2.1 formats it).
