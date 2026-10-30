# MuseScore fork

The engine is consumed from the fork https://github.com/Locietta/engrave as the
submodule `vendor/musescore` (shallow, fetched by SHA). webmscore and the
`patches/*.patch` queue are retired; the last executable pre-fork state is the local
tag `pre-fork-baseline` (`f8e7d26`). The superproject pins `mcp-4.7.6-4` until phase A2.

The fork's development line is `main`, rebased onto unreleased MuseScore 5.0 as a hard
fork (`engrave`). It is merge-only: never rebased, and never force-pushed. As verified
and accepted on 2026-10-29 (A1 close-out), `main` stands at commit `0adbf67ed7`
(hardened `tools/sync-upstream.py` and 29-step test suite), based on:
- Upstream MuseScore `0ea613f`
- Commit `ca2b19d2ea` ("engrave: TRIM list and tools/sync-upstream.py")
- Merge `7150f2b05e` ("Absorb muse_framework ecdc6e7 at muse/ (subtree)", second parent
  framework commit `ecdc6e7`).

`main` was fast-forward pushed to GitHub; no force push was used.

Mirror branches track upstream history without local modifications:
- `upstream/musescore`: upstream `main` (`0ea613f`).
- `upstream/muse-framework`: full `muse_framework` history (7917 commits, tip `ecdc6e7`).

The old branches `mcp/v4.7.5`, `mcp/v4.7.6` and their `mcp-*` tags are frozen history.

## Rules

- **Branch policy**: `main` moves forward strictly via merge commits; force-pushes are
  prohibited. A sync runs on a temporary branch `sync/<sha>` and fast-forwards `main`
  via `git merge --ff-only`. The branch moves only by this final fast-forward. Nothing
  is pushed automatically.
- **Pins**: Starting in phase A2, every pin pinned by the superproject carries a tag
  `engrave-<n>` on `main`. Each pin records both the upstream SHA and the framework SHA in
  the ledger.
- **Preconditions and environment**:
  - Dedicated sync clone: operated solely by the orchestrator.
  - Worktree hygiene: refuses to start unless there are no staged, unstaged, untracked, or
    ignored files anywhere (`status --porcelain --ignored=matching --untracked-files=all`
    must be completely clean). Refuses gitlink directories (`muse/`, `muse_deps/`) that
    already hold files, which `git status` does not inspect.
  - Safe git operations: never uses forced checkouts (`checkout -f`). All git commands
    run with `--literal-pathspecs` so trimmed names (e.g. `item[ab].txt`) are never treated
    as globs. Output is configured to UTF-8 with `backslashreplace` so legacy console code
    pages (such as GBK) cannot crash the tool or mangle paths.
- **Sync script**: Synchronization is automated with `tools/sync-upstream.py`:
  - Subcommands: `absorb` (initial one-time absorption), `sync [--target SHA]`, `continue`,
    and `abort`.
  - Stage machine: state lives in `.git/sync-upstream.json` (version 3), written atomically
    (temporary file plus `os.replace`) before each mutation:
    `start -> temp-branch -> upstream-merge -> upstream-commit`
    `-> framework-merge -> check -> fast-forward -> cleanup`.
  - Resumption and abort: `continue` and `abort` work cleanly from every stage boundary
    (including right after merge outcome persistence or after fast-forward before cleanup).
    `continue` resumes after human conflict resolution. `abort` discards everything since
    the sync started (including resolution work staged or committed) and restores the branch.
    Rerunning `sync` automatically aborts any leftover temporary sync branch first.
  - Persisted removal plan: planned removals of trimmed paths and their report entries are
    written to state before anything is removed from index or worktree. Resuming replays the
    plan; `abort` deletes planned paths untracked by the restored branch so the worktree
    is left completely clean.
  - Report sections: summary output, commit messages, and final report record:
    1. Upstream changes discarded on trimmed paths (`discardedChanges`).
    2. New upstream files dropped under trimmed prefixes (`droppedAdditions`).
    3. Conflict resolutions that differ from the merge sides (`resolutions`).
- **Tool-made merge commits only**:
  - All merge commits are created by the tool with provenance trailers: `Sync-Upstream: <sha>`
    on the upstream merge, and `Framework-Commit: <sha>` on the framework merge.
  - Foreign merge commits (missing trailers, wrong trailers, or unexpected parents) are
    refused with instructions.
  - On conflict, the human resolves and `git add`s the resolved paths, then runs `continue`;
    the tool creates the merge commit and validates provenance.
- **Framework provenance and validation**:
  - Target upstream SHA must exist on `upstream/musescore`.
  - The framework SHA `F` named by the target's `muse` gitlink must exist on
    `upstream/muse-framework` (refuses unmirrored commits, e.g. upstream
    `9625e32ad4 -> 353f5f612a`; pick a neighbouring commit).
  - The previous framework commit is read from the newest `Framework-Commit` trailer on the
    first-parent chain (falling back to the subject of absorb commit `7150f2b05e`) and
    validated against git history and local subtree delta.
  - Framework reachable only via a side parent is refused: the sync merge base must match
    the recorded first-parent framework commit to prevent dropping framework history.
  - The framework commit must never move backwards.
  - If the framework gitlink moved, the second merge runs `Merge upstream/muse-framework <F>
    into muse/` using `-s ort -Xsubtree=muse`. If unchanged, the second merge is skipped.
- **Rename detection disabled (`-Xno-renames`)**:
  - Upstream merge runs `-s ort -Xno-renames`. Relative to the merge base, our side deletes
    all trimmed paths and adds `muse/`, which caused `ort` rename detection to match a trimmed
    appshell QML file with a framework header (observed). Disabling renames prevents this.
  - Consequences of `-Xno-renames`: upstream renames appear as delete + add; an upstream
    rename of a file we changed surfaces as a modify/delete conflict plus a separate
    addition; directory renames are not propagated (additions and edits can remain at old
    paths); moves into or out of trimmed prefixes require human review.
- **TRIM policy**:
  - Fork root file `TRIM` specifies paths dropped from the build. Syntax: one path per line,
    `#` comments, trailing `/` denotes directory prefix, otherwise exact file; no globs.
    Initially contains `.gitmodules` and `muse_deps`.
  - TRIM belongs to `main`: a sync never changes TRIM. An upstream target that adds or
    modifies `TRIM` is refused. Tree validation asserts `TRIM` remains identical.
- **Conflict handling**:
  - `muse` gitlink conflict: `ort` creates a displaced `muse~<sha>` entry. It is dropped
    only when it is a 160000 gitlink whose stage-3 SHA matches the target's `muse` gitlink,
    and the path exists neither in the target tree, our start branch, nor the merge base.
    Any other collision is treated as a real conflict for human inspection.
  - Modify/delete conflicts on `TRIM` paths keep the deletion and are reported.
  - New upstream files added under trimmed prefixes are deleted and reported.
  - All other conflicts stop the script for human intervention.
- **Tree integrity checks**: Before fast-forwarding, verifies no gitlink in index, no path
  in `TRIM` exists, `muse/` is non-empty, and `muse/` equals framework tree when there is no
  local framework delta.
- **Documented limits**:
  - No proof that a human resolution preserved other content: the tool does not prove
    resolution correctness, but prints all paths whose resolution differs from both merge
    sides in the final report for reviewer inspection.
  - Fast-forwardable upstream targets not supported: upstream never contains our commits;
    every sync produces a merge commit.
  - Side-parent framework reachability refused: cannot determine if content reached `muse/`.
  - Absorb limitation: `absorb` can leave a half-finished merge if its final commit fails.
- **Submodule verification**: `scripts/prepare.py` refuses to stage resources when the
  submodule is not at the recorded commit or has local changes.
- **Practicalities**:
  - Push over SSH (`git@github.com:Locietta/engrave.git`). Pushing large histories over
    HTTPS fails in this environment with HTTP 408.
  - Line endings on Windows: work clones require `core.eol=lf` and `core.autocrlf=false`
    because upstream `.gitattributes` declares `* text=auto`.
  - The orchestrator's local work clone is `.upstream/engrave` (blobless clone).

## Ledger: main

Foundation commits and pinned releases on `main`. Pins start at A2 (`engrave-1`).

| Pin / Milestone | Commit | Upstream SHA | Framework SHA | Notes |
| --- | --- | --- | --- | --- |
| absorb (A1 baseline) | `7150f2b05e` | `0ea613f` | `ecdc6e7` | Not a pin. Absorbed muse_framework into `muse/` via subtree. Base for A1/A2. |
| main tip (A1 accepted) | `0adbf67ed7` | `0ea613f` | `ecdc6e7` | Not a pin. Hardened sync tool and 29-step test suite; accepted by Terra. |
| `engrave-1` | `3a0fff5731` | `0ea613f` | `ecdc6e7` | First pin. Merges `a2/port` (`2c6b77282f`, the 5.0 port at parity) into `main`; the tree differs from `2c6b77282f` only in `tools/`. Validated on Windows and linux-host (superproject `engrave/a2` `c090d81`); superproject pins it at `abc13ca`. |

The superproject's `.gitmodules` now tracks `branch = main`.

## History: mcp/v4.7.x (frozen)

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
