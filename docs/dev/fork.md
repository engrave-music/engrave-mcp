# engrave engine fork

`engrave` is a native, headless C++ score engraving and audio playback engine library, maintained as a hard fork of MuseScore core at https://github.com/Locietta/engrave. It is completely free of Qt, GUI, and desktop dependencies. The host consumes it as the shallow submodule `vendor/engrave`.

## Upstream provenance

Ancestral upstream synchronization terminated at `upstream-final` (syncing upstream MuseScore commit `32f0a26e5b` and framework commit `da3885cb` after absorbing `muse_framework` into `muse/`). Since that synchronization point, development moves forward independently. Upstream fixes arrive strictly by cherry-pick and adaptation with upstream SHA, framework origin if applicable, technical rationale, and focused test evidence.

## Engine structure

The fork contains its own standalone xmake build and public C++ boundary:

- `xmake.lua`, `xmake/engrave.lua` — engine library targets (`engrave` and module libraries), generated version metadata (`engrave_version.h`), and pinned third-party package dependencies under `xmake/packages/`.
- `xmake/tests.lua` — opt-in test targets (`--engrave_tests=y`), building the public API, import, playback, and adapter regression suites.
- `engrave/include/engrave/` — public header interface (`engrave.h`, `engine.h`, `document.h`, `output.h`, `audio.h`, `edit.h`, `query.h`, `export_warning.h`, `error.h`, `version.h`). Host code includes only headers from this directory.
- `engrave/assets.json` and `tools/install_assets.py` — SHA-256 and byte-size manifest (348 pinned files including fonts, instruments, templates, MPE profiles, and license notices) and standalone installer.

## Hard-fork rules

- **Forward-only history**: `main` moves forward strictly through reviewed feature commits and merges. Force-pushes and rebases of published history are prohibited.
- **Cherry-picks only**: Upstream changes are never merged in bulk. Specific commits are fetched and cherry-picked/adapted on demand. Reference branches `upstream/musescore` and `upstream/muse-framework` are frozen.
- **Pin commits on fork main by SHA**: Every engine state pinned by `vendor/engrave` in the host is a commit on fork `main`. Pins are not tagged; tags are reserved for real version releases.
- **Branch hygiene**: Feature branches (`feat/...`, `fix/...`) merge into `main` and are deleted.

## Bumping the engine pin

To update `vendor/engrave` to a newer commit:

1. Validate the engine commit thoroughly in the fork checkout across supported formats and render/audio goldens on both native Windows and Linux (`linux-host`).
2. Push the engine commit to fork `main` first (`git -C vendor/engrave push origin main`). The remote submodule pin must resolve on GitHub before or at the time the host commit is pushed.
3. Update the gitlink in the host repository:
   ```powershell
   git add vendor/engrave
   ```
4. The host bump commit message carries the validation evidence:
   - Submodule commit range (`<old-sha>..<new-sha>`)
   - Description of changes included in the bump
   - Engine gate results (`tests/engine_gate.py` or fork `tests/gate.py`) on Windows and Linux
