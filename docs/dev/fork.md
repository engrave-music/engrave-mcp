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
