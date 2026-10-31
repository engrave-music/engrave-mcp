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
| DEFECT-ENGRAVE-005 | Host asset staging must move to installer | Open | EXECUTED | `generate_resources.py --check` fails at `engrave-14`; A5 must use `install_assets.py` |
| DEFECT-ENGRAVE-006 | Phase C formal status | Implementation complete / Formal acceptance PENDING | EXECUTED, SOURCE-REVIEWED | Formal closeout pending doc corrections, this ledger, and GPT-6 Astra review |
| DEFECT-ENGRAVE-007 | Parked API work | Parked | SOURCE-REVIEWED | Runtime font/SoundFont, exact positions, and excerpt export deferred to `a5/api-gaps` |

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

- **Status**: Open / Pending A5 host migration.
- **Evidence Level**: EXECUTED.
- **Description**: `python -B scripts/generate_resources.py --check` fails at `engrave-14` because `src/engraving/engraving.qrc` was retired in C7, while host `scripts/prepare.py` still calls that generator. The QRC lists are gone by design; A5 must stage runtime assets using the fork's `tools/install_assets.py` + `engrave/assets.json` (345 files, 28,380,646 bytes) before cutover.
- **Impact**: Frozen v1 host resource staging cannot be regenerated against `engrave-14` without adaptation.
- **Next Step**: Integrate `tools/install_assets.py` and verify packaging during A5 host cutover.

### 6. Phase C formal status (DEFECT-ENGRAVE-006)

- **Status**: Implementation complete, formal acceptance PENDING.
- **Evidence Level**: EXECUTED, SOURCE-REVIEWED.
- **Description**: Phase C (C0–C8) implementation is complete and gate-green at `ab9e33c39b` on both Windows and physical Linux `linux-host` (Terra re-ran both full gates at the exact tag: Windows 213 identical / 38 hash-allowed / 0 different / 0 missing; Linux 186 / 65 / 0 / 0; 124 commands, zero failures). However, Terra's closeout review verdict is: REJECT formal Phase C closeout for now (`notes/cleanup/c8/REVIEW-TERRA.md`). Closeout requires:
  1. Governance documentation correction pass (completed in this cycle).
  2. A durable, tracked in-repository defect record (this document).
  3. The separate GPT-6 Astra phase review, which has not yet been performed.
- **Impact**: Phase C cannot be represented as formally accepted until the Astra phase review is completed and accepted.
- **Next Step**: Submit documentation correction pass for the GPT-6 Astra phase review.

### 7. Parked API work (DEFECT-ENGRAVE-007)

- **Status**: Parked / Pending `a5/api-gaps`.
- **Evidence Level**: SOURCE-REVIEWED.
- **Description**: Runtime font/SoundFont changes, exact measure/segment positions, and excerpt score-file export remain required, to be implemented on the cleaned boundary as `a5/api-gaps`.
- **Impact**: Excerpt export, runtime font configuration, and exact geometry are not yet exposed on the cleaned engine API boundary.
- **Next Step**: Implement on branch `a5/api-gaps` against `engrave-14` with standard pin, test, and golden governance.
