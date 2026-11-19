# engrave engine fork

`engrave` is a native, headless C++ score engraving and audio playback engine library, maintained as a hard fork of MuseScore core at https://github.com/Locietta/engrave. It is completely free of Qt, GUI, and desktop dependencies. The host consumes it as the shallow submodule `vendor/engrave`.

## Upstream provenance

The fork is based on upstream MuseScore commit `32f0a26e5b`, with the MuseScore framework (commit `da3885cb`) absorbed into `muse/`. Development moves forward independently. Upstream fixes arrive strictly by cherry-pick and adaptation with upstream SHA, framework origin if applicable, technical rationale, and focused test evidence.

## Engine structure

The fork contains its own standalone Bazel (`MODULE.bazel`, `BUILD.bazel`) build and public C++ boundary:

- `MODULE.bazel`, `BUILD.bazel`, `bazel/` — engine library targets (`//:engrave` over `//:engrave_core` and `//:engrave_audio`), isolated version metadata (`@engrave_config//:muse_generated`), relocatable Clang binary PCH (`bazel/pch/engraving_pch.h`), and hash-pinned third-party dependencies (`bazel/deps.bzl`, `bazel/third_party/*.BUILD.bazel`).
- `//:engine_tests` (`BUILD.bazel`) — test and probe filegroup building the public API, import, playback, and adapter regression suites.
- `engrave/include/engrave/` — public header interface (`engrave.h`, `engine.h`, `document.h`, `output.h`, `audio.h`, `edit.h`, `query.h`, `export_warning.h`, `error.h`, `version.h`). Host code includes only headers from this directory.
- `engrave/assets.json` and `tools/install_assets.py` — runtime asset path manifest (fonts, instruments, templates, MPE profiles, and license notices) and standalone installer reading committed Git blobs via `git cat-file --batch`.

## Hard-fork rules

- **Forward-only history**: `main` moves forward strictly through reviewed feature commits and merges. Force-pushes and rebases of published history are prohibited.
- **Cherry-picks only**: Upstream changes are never merged in bulk. Specific commits are fetched and cherry-picked/adapted on demand. Reference branches `upstream/musescore` and `upstream/muse-framework` are frozen.
- **Pin commits on fork main by SHA**: Every engine state pinned by `vendor/engrave` in the host is a commit on fork `main`. Pins are not tagged; tags are reserved for real version releases.
- **Branch hygiene**: Feature branches (`feat/...`, `fix/...`) merge into `main` and are deleted.

## Bumping the engine pin

To update `vendor/engrave` to a newer commit:

1. Validate the engine commit with `tests/engine_gate.py --quick` (or `tests/gate.py --quick`) per step, and run the full engine gate, default host lanes, and Linux validation (`ssh user@<linux-host>`) before bumping the pin.
2. Push the engine commit to fork `main` first (`git -C vendor/engrave push origin main`). The remote submodule pin must resolve on GitHub before or at the time the host commit is pushed.
3. Update the gitlink in the host repository:
   ```powershell
   git add vendor/engrave
   ```
4. The host bump commit message carries the validation evidence:
   - Submodule commit range (`<old-sha>..<new-sha>`)
   - Description of changes included in the bump
   - Engine gate results (`tests/engine_gate.py` or fork `tests/gate.py`) on Windows and Linux
