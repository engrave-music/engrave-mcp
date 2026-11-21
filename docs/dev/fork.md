# Engine Fork and Branching

Development spans two repositories: the host (`engrave-mcp`) and the engine
submodule (`vendor/engrave`, maintained as a hard fork of MuseScore core at
`https://github.com/Locietta/engrave`). Both use `main` as the single long-lived
integration branch and reserve tags for formal releases.

## Host Repository (`engrave-mcp`)

- `main` is the only long-lived branch and tracks `origin/main`
  (`https://github.com/Locietta/engrave-mcp`).
- Develop on short-lived topic branches (`feat/...`, `fix/...`, `cleanup/...`).
  Merge to `main` once verified, then delete the topic branch.
- Reserve tags for formal version releases (`v<version>`).
- Exchange commits across machines by pushing and fetching branches through Git
  remotes, never via git bundles or copied worktrees. Before deleting a remote
  topic branch, verify its commits are contained in `main`; preserve unmerged
  work and frozen `upstream/*` reference branches.

## Engine Fork (`vendor/engrave`)

`engrave` is a native, headless C++ score engraving and audio playback engine
library, free of GUI and desktop dependencies, consumed by the host as a
shallow submodule.

### Upstream Provenance

The fork starts from upstream MuseScore commit `32f0a26e5b`, with the MuseScore
framework (commit `da3885cb`) absorbed into `muse/`. Development moves forward
independently; upstream fixes arrive only by cherry-pick and adaptation with the
upstream SHA, framework origin (if applicable), rationale, and focused test
evidence.

### Upstream 5.0 Ports

Fork commits `6ddac86873..ea7de5b29c` port these items from upstream MuseScore
`32f0a26e5b..08d70c1a6a`:

1. **Read/write and layout fixes**: `d923bdfee6` (skip duplicate tuplet
   elements), `5471ed55d1` (#35281, nested tuplet brackets on shared staves),
   `651875562b` (tuplet tick at construction), `6eb74953a6` (derived
   `ends_note_anchored_line`), `72360d181f` (chords take no glissandos), and
   `959c852dab` (Guitar Pro slide glissandos on their start note). The fork
   adds its own safety for note spanners, excerpts, and grace chords across
   range and meter rewrites.
2. **User-authored stave-sharing maps**: PR #35239 (`521a4951d5` through merge
   `9c58704883`) with the typed parent handling of `3da2b2351b`. Authored maps
   (`StaveSharingState`) persist on `SharedPart`. Resolved maps, the map
   indicators (`StaveSharingChange`), and gap rests are layout output in
   `LayoutDataStore`. Multi-measure rest detection checks authored music on
   enabled sharing origins before layout resolves the shared staff's content.
   `write470` drops the authored maps, any serialized
   indicators, and the `staveSharingChangeMinDistance` style, which 4.7.6 does
   not read.
3. **Jianpu numbered notation**: `82ba7e5736` (#31626) and `e93f1e4aae`
   (#35681, missing Jianpu text styles). Digits, octave dots, and duration
   lines are layout output in `LayoutDataStore`. The 24 `jianpu*` styles are in
   the 4.70 style catalog. `write470` strips them from scores without Jianpu
   and refuses Jianpu staff types, custom Jianpu key numbering, and
   `jianpu_number` text-style references (`B470_UNSUPPORTED_FEATURE`). Under
   the default numbering, which follows the score key, it drops the inactive
   tonic.

Not ported: `bendAlign` (`a9eb9927`, `457c8a5c`) and the legacy chord-line
offset conversion of `daa0e5f3`, which the fork does not need because it
already reads and writes the 5.0 `<o1>`–`<o4>` offsets.

### Engine Structure

- `MODULE.bazel`, `BUILD.bazel`, `bazel/`: Standalone Bazel targets
  (`//:engrave` over `//:engrave_core` and `//:engrave_audio`), isolated version
  metadata (`@engrave_config//:muse_generated`), relocatable Clang binary PCH
  (`bazel/pch/engraving_pch.h`), and hash-pinned third-party dependencies
  (`bazel/deps.bzl`, `bazel/third_party/*.BUILD.bazel`).
- `//:engine_tests` (`BUILD.bazel`): Public API, import, playback, and adapter
  regression suites.
- `engrave/include/engrave/`: Public C++ header interface (`engrave.h`,
  `engine.h`, `document.h`, `output.h`, `audio.h`, `edit.h`, `query.h`,
  `export_warning.h`, `error.h`, `version.h`). Host code includes only headers
  from this directory.
- `engrave/assets.json` and `tools/install_assets.py`: Runtime asset manifest
  and standalone installer reading committed Git blobs via
  `git cat-file --batch`.
- `docs/`: Engine design and contract documentation (`code-style-guide.md`,
  `playback.md`, `playback-harness.md`, `mnx.md`, `write470.md`, `assets.md`).

### Hard-Fork Rules

- **Forward-only history**: `main` tracks `origin/main`
  (`https://github.com/Locietta/engrave.git`) and moves forward strictly through
  reviewed commits and merges. Force-pushes and rebases of published history are
  prohibited.
- **Cherry-picks only**: Upstream changes are never merged in bulk. Reference
  branches `upstream/musescore` and `upstream/muse-framework` are frozen.
- **Pin by SHA**: Every `vendor/engrave` pin in the host points to a commit on
  fork `main`. Submodule pins are never tagged.
- **Branch hygiene**: Topic branches (`feat/...`, `fix/...`) merge into `main`
  and are deleted.

To inspect deeper history in the shallow submodule checkout without altering the
pin:

```bash
git -C vendor/engrave fetch --deepen=256 origin main
```

## Bumping the Engine Pin

1. Validate per-step engine changes with `tests/engine_gate.py --quick` (or
   `vendor/engrave/tests/gate.py --quick`), and run the full engine gate,
   default host lanes, and validation on both Windows and Linux before bumping
   the pin.
2. Push the engine commit to fork `main` first
   (`git -C vendor/engrave push origin main`) so the public submodule reference
   resolves before or when the host commit is pushed.
3. Update the gitlink in the host repository:
   ```powershell
   git add vendor/engrave
   ```
4. Record the validation evidence in the host bump commit message:
   - Submodule commit range (`<old-sha>..<new-sha>`)
   - Summary of changes included in the bump
   - Engine gate results (`tests/engine_gate.py` or fork `tests/gate.py`) on
     Windows and Linux
