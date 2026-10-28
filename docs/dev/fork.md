# MuseScore fork

The engine is consumed from the fork https://github.com/Locietta/engrave as the
submodule `vendor/musescore` (shallow, fetched by SHA). webmscore and the
`patches/*.patch` queue are retired; the last executable pre-fork state is the local
tag `pre-fork-baseline` (`f8e7d26`). The superproject pins `mcp-4.7.6-4` until phase A2.

The fork's development line is `main`, rebased onto unreleased MuseScore 5.0 as a hard
fork (`engrave`). It is merge-only: never rebased, and never force-pushed. As executed
on 2026-10-28, `main` stands at:
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
  prohibited. A sync runs on a temporary branch `sync/<sha>` and fast-forwards `main`.
  Nothing is pushed automatically.
- **Pins**: Starting in phase A2, every pin pinned by the superproject carries a tag
  `engrave-<n>` on `main`. Each pin records both the upstream SHA and the framework SHA in
  the ledger.
- **Sync script**: Synchronization is automated with `tools/sync-upstream.py`:
  - Subcommands: `absorb` (initial one-time absorption), `sync [--target SHA]`, `continue`,
    and `abort`.
  - State is tracked in `.git/sync-upstream.json`. An interrupted sync can be resumed with
    `continue` after human conflict resolution, or aborted with `abort`. Rerunning `sync`
    automatically aborts any leftover temporary sync branch first.
- **Two merge commits**: Each sync performs two distinct merges rather than combining parents:
  1. `Merge upstream/musescore <U>`
  2. If the framework gitlink moved, `Merge upstream/muse-framework <F> into muse/` using
     `-s ort -Xsubtree=muse`, with the previously merged framework commit as the merge base.
- **Rename detection disabled**: The upstream merge runs `-s ort -Xno-renames`. Relative to the
  upstream merge base our side "deletes" all trimmed paths and "adds" all of `muse/`, which
  caused `ort` rename detection to pair a trimmed appshell QML file with a framework header
  (observed). Disabling renames avoids this; an upstream rename of a file we changed surfaces
  as a modify/delete conflict for human inspection.
- **Pre-merge validation**:
  - The target upstream SHA must exist on `upstream/musescore`.
  - The framework SHA `F` named by the target's `muse` gitlink must exist on the
    `upstream/muse-framework` mirror. Upstream occasionally pins a framework commit that is
    not on `muse_framework` `main` (observed in 2 of the last 60 bumps, e.g. upstream
    `9625e32ad4 -> 353f5f612a`). The script refuses such targets; pick a neighbouring
    upstream commit.
  - The framework commit must never move backwards.
- **TRIM list**: Fork root file `TRIM` specifies paths dropped from the build. Syntax: one
  path per line, `#` comments, trailing `/` denotes directory prefix, otherwise exact file;
  no globs. Initially contains `.gitmodules` and `muse_deps` (prebuilt dependency recipes are
  not used; xmake provides dependencies natively).
- **Conflict handling and index checks**:
  - The `muse` gitlink conflict (`ort` moves it to `muse~<sha>`) is dropped from the index.
  - Modify/delete conflicts on `TRIM` paths automatically keep the deletion and are reported.
  - New upstream files added under trimmed prefixes are automatically deleted and reported.
  - All other conflicts stop the script for human intervention.
  - Tree integrity checks before committing: index contains no gitlink, no path listed in
    `TRIM` exists in the tree, `muse/` is non-empty, and `muse/` equals the framework tree
    when there is no local framework delta.
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
