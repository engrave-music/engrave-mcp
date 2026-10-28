# MuseScore fork

The engine is consumed from the fork https://github.com/Locietta/MuseScore as the
submodule `vendor/musescore` (shallow, fetched by SHA). webmscore and the
`patches/*.patch` queue are retired; the last executable pre-fork state is the local
tag `pre-fork-baseline` (`f8e7d26`).

Rules:
- Fork branches are never force-pushed. A rebase creates a new branch; every SHA
  the superproject pins carries a tag `mcp-<upstream version>-<n>`.
- Review rebases with `git range-diff <old-base>..<old-tip> <new-base>..<new-tip>`.
- `scripts/prepare.py` refuses to stage resources when the submodule is not at the
  recorded commit or has local changes.
- Topic commits carry only deltas the native build compiles and needs. Upstream
  code paths and resource names are kept; the host adapts to them.

## Ledger

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
`.gitmodules` records `branch = mcp/v4.7.5` so `git submodule update --remote`
stays on the pinned line.

Evidence for this pin is in validation.md ("Roadmap step 1").

### `mcp/v4.7.6` (base `v4.7.6`), tag `mcp-4.7.6-1` = `30387998`

The ten `mcp/v4.7.5` topics cherry-picked without conflicts;
`git range-diff v4.7.5..mcp-4.7.5-1 v4.7.6..mcp/v4.7.6` reports every patch
identical (`=`). v4.7.6 adds no source file to the compiled modules.
Tag `mcp-4.7.6-2` = `e099f16b` adds `Revert "TEMPORARY: ..."` on `mcp/v4.7.6`: the host
registers an `IApplication` (native/noqt_application.cpp), so the writers report the
real version again. `mcp/v4.7.6` now carries the nine permanent topics only (net).