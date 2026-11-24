# engrave-mcp

`engrave-mcp` is a native C++ MCP server that gives LLM agents a real music
engraving engine: create, inspect, edit, render and export scores over stdio
JSON-RPC.

The engine is **engrave**, a pinned hard fork of the MuseScore 5.0 core
([github.com/engrave-music/engrave](https://github.com/engrave-music/engrave), consumed as
`vendor/engrave`). It is compiled directly into the server as a standalone native
C++ binary with no Node.js, WebAssembly, or desktop MuseScore dependency at runtime.

The goal is interop, not legacy compatibility: an agent-made score must open and remain
editable in MuseScore, and a MuseScore score must open and remain editable by
the agent. The reader accepts MuseScore format 4.60 up to 5.00 and rejects versions
above 5.00 or prior to 4.60. MSCZ and MusicXML inputs with embedded images are rejected.
The default MSCZ writer emits 5.00 format; a bounded 4.70 writer targeting MuseScore 4.7.0–4.7.6
is available for MuseScore 4.x interop where the score stays inside its documented subset.

## What it can do

- Speak MCP over stdio JSON-RPC in both modern stateless mode (`2026-07-28`) and
  legacy session mode (`2025-11-25` / `2025-06-18`).
- Open MSCZ/MSCX, MusicXML/MXL, MIDI, Guitar Pro (GP3–GP7/GPX/PTB) and MNX.
- Create a score from an engine template.
- Read score outlines, inspect bar-aligned music windows (`score_read_music`),
  query elements/properties/page counts/audio tracks (`score_query`), and
  discover templates, instruments, types, styles and edit operations
  (`runtime_info`).
- Write musical phrases with compact notation (`score_write_phrase`) and apply
  edit batches (28 operations) with strict revisions (`score_edit`). Batches
  are atomic by default (`atomic: false` opts into prefix-commit). Named
  checkpoints give explicit rollback.
- Export score files, PDF/PNG/SVG vector and raster pages, synthesized audio
  (WAV/FLAC/Opus/MP3) and layout position data directly from the **live**
  in-memory document, with durable publication.
- Hot-load fonts and replace SoundFonts at runtime (`runtime_configure`).

## Quick start (Windows)

Prerequisites: Git and [Pixi](https://pixi.sh/) (Bazelisk and the hermetic `xclang` C++23 toolchain are provisioned automatically).

Clone with `--recursive`, or initialize the submodule before installing:

```powershell
git submodule update --init
pixi install --locked
pixi run python scripts/bootstrap.py --soundfont
pixi run python scripts/prepare.py
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 12
```

Run the protocol smoke test against the build:

```powershell
$env:ENGRAVE_MCP_BINARY   = "$PWD/build/release/engrave-mcp.exe"
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
$env:ENGRAVE_MCP_SOUNDFONT = "$PWD/.runtime/soundfonts/FluidR3Mono_GM.sf3"
pixi run python tests/integration/test_mcp_smoke.py
```

To run the development binary manually (via `pixi run` so Pixi's native DLLs are on `PATH`), pass resources explicitly; `--soundfont` is optional:

```powershell
pixi run build/release/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

A packaged build finds its sibling `assets/` directory automatically. Windows
and Linux build, test, and packaging details are in [docs/dev/build.md](docs/dev/build.md):

- **Development builds**: Use `scripts/bbuild.py --profile <release|debug|releasedbg|sanitize> -j 12 build engrave_mcp` (stages to `build/<profile>` with shared Bazel caches in `build/.bazel_repo_cache` and `build/.bazel_disk_cache`). Reserve `scripts/build-native.ps1` / `scripts/build-native.sh` for clean builds at the pinned engine (they reset `vendor/engrave` via `git submodule update --init`).
- **Acceptance lanes**: Use `tests/run_lanes.py` (builds needed targets automatically; `--no-build` skips building, `--profile` selects `build/<profile>`, `--list-lanes` prints the schedule) with `--output` under `notes/runs/<name>`; see [docs/dev/validation.md](docs/dev/validation.md).

## Tools

| Tool                 | What it does                                                                                                                    |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `score_open`         | Open from `path` or base64 `data` (+ `format`, `filename_hint`, `strict_ids`).                                                  |
| `score_create`       | Create from `template_id` or `template_path` (+ `title`, `subtitle`, `composer`, `lyricist`).                                   |
| `score_close`        | Close a document and free its memory.                                                                                           |
| `score_outline`      | Title/metadata, parts, measures, signatures, page count, revision.                                                              |
| `score_read_music`   | Read a bar-aligned music window (`measure`, `count`, optional `staves`, `voices`, `excerpt`, `include_ids`).                    |
| `score_write_phrase` | Enter compact musical notation into measure/staff/voice `lanes` (per-lane `pitch_mode`; optional `expected_revision`).          |
| `score_query`        | `mode`: `elements`, `properties`, `page_count`, or `audio_tracks`.                                                              |
| `score_edit`         | Apply up to 1,024 `ops` (atomic by default; set `atomic: false` for prefix-commit) with `expected_revision`.                    |
| `score_export`       | Export `format` (`mscz`, `pdf`, `png`, `svg`, `wav`, `flac`, `opus`, `mp3`, positions, etc.) to `path`.                         |
| `runtime_info`       | Inspect capabilities, limits, and catalogs (`scope`: `overview`, `templates`, `instruments`, `types`, `type`, `styles`, `ops`). |
| `runtime_configure`  | Hot-load a font (`add_font`) and/or replace the ordered `soundfonts` list at runtime.                                           |
| `checkpoint`         | Save a named in-memory snapshot of a document.                                                                                  |
| `restore_checkpoint` | Restore a named snapshot; clears a poisoned document.                                                                           |
| `drop_checkpoint`    | Delete a named snapshot.                                                                                                        |

Every successful result carries `ok: true`. Failures carry
`ok: false` and `error: {code, message, ...}` and set the MCP `isError` flag.
The full contract is in [docs/dev/tools.md](docs/dev/tools.md) and the generated
schema in [engrave_mcp/tools.json](engrave_mcp/tools.json).

Schematic of the arguments and the `structuredContent` each call returns:

```json
{"method":"tools/call","params":{"name":"score_create",
  "arguments":{"template_id":"Treble_Clef","title":"Prelude"}}}
{"ok":true,"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1","revision":"1","outline":{"...":"..."},"diagnostics":[]}
```

```json
{"method":"tools/call","params":{"name":"score_export",
  "arguments":{"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1",
    "format":"pdf","path":"C:/Scores/prelude.pdf"}}}
{"ok":true,"source_revision":"1",
 "artifacts":[{"path":"C:/Scores/prelude.pdf","mime":"application/pdf","size":48213}]}
```

## Client configuration

After packaging (for example to `artifacts/windows-next`), register the server
in your MCP client configuration using an absolute path. A packaged bundle
automatically discovers its sibling `assets/` directory, and auto-loads
`soundfonts/FluidR3Mono_GM.sf3` when that SoundFont was packaged alongside it
(pass `--soundfont` or call `runtime_configure` for packages without it or to
select another SoundFont):

```json
{
  "mcpServers": {
    "engrave-mcp": {
      "command": "C:/path/to/engrave-mcp/artifacts/windows-next/engrave-mcp.exe"
    }
  }
}
```

## Architecture overview

- **One engine, one owner thread.** All score engine operations are serialized on a
  dedicated owner thread. Up to 16 documents can remain open concurrently; they share the engine.
  Concurrent editing of one document is not supported by design.
- **Direct in-memory export.** Exports execute directly against the in-memory document,
  avoiding save-and-reopen layout reflow and minimizing export latency.
- **Durable publication.** Output is written to a private staging directory,
  flushed to disk, and atomically renamed into place. Multi-file exports are atomic per file;
  errors report which files were already published.
- **Cooperative cancellation and watchdog.** Long operations poll a cancellation
  flag. Before publication starts, cancellation publishes nothing. Once publication begins,
  late cancellation can still finish successfully or report already-published files. If a cancelled
  live export remains blocked, a watchdog timer (5 seconds) waits for the publication gate held through
  synchronous report delivery (deferred indefinitely if stdout is unread), then exits 70 rather than killing threads.
- **Explicit quotas.** 4 MiB request line, 64 in-flight calls, 16 MiB response cap,
  1 GiB export file budget, 8 MiB decoded input limit, and up to 16 open documents.

The reasoning behind these choices is in [docs/dev/design.md](docs/dev/design.md).

## Scope and non-goals

Supported: ordinary single-user local filesystems on Windows and Linux, ordinary
inherited permissions, the documented path syntax, and scores from MuseScore 4.6
onward.

Explicit non-goals: MuseScore 1.x–4.5 readers, full MuseScore desktop feature parity,
in-server filesystem sandboxing or deprecated MCP `roots` enforcement (workspace
isolation is owned by host-side OS sandboxes and client tool approvals),
complex/protected DACLs and ACL merging, SMB network share authentication, device namespaces
and extended-length paths, cross-volume or multi-file transactional publication,
and interactive/streaming playback. See [docs/dev/scope.md](docs/dev/scope.md) for the triage policy.

## Repository layout

```
engrave_mcp/     MCP transport, tool registry, JSON codecs, live export, publication
vendor/engrave/  pinned engrave engine (submodule, pinned by commit)
licenses/        third-party and runtime license notices bundled by the packagers
MODULE.bazel     Bzlmod dependencies and hermetic xclang toolchain
BUILD.bazel      host targets; consumes @engrave from vendor/engrave
clice.toml       shared host/engine editor, formatting and lint configuration
scripts/         bootstrap, Bazel build wrapper, packaging dev tools
tests/           acceptance lane runner, integration tests, support, benchmarks, test data
docs/dev/        design, tool contract, build, validation, engine fork guide
```

## Documentation

- [Design and rationale](docs/dev/design.md)
- [Tool contract](docs/dev/tools.md)
- [Build and test](docs/dev/build.md)
- [Validation and test lanes](docs/dev/validation.md)
- [Engine fork and branching](docs/dev/fork.md)
- [Supported scope and non-goals](docs/dev/scope.md)
- [Open defects and limitations](docs/dev/defects.md)
- [Code style guide](docs/dev/code-style-guide.md)
- [中文说明](README.zh-CN.md)
