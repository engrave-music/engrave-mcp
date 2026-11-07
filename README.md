# engrave-mcp

`engrave-mcp` is a native C++ MCP server that gives LLM agents a real music
engraving engine: create, inspect, edit, render and export scores over stdio
JSON-RPC.

The engine is **engrave**, a pinned hard fork of the MuseScore 5.0 core
([github.com/Locietta/engrave](https://github.com/Locietta/engrave), consumed as
`vendor/engrave`). It is compiled directly into the server. There is no Qt, Node.js,
WebAssembly or desktop MuseScore dependency at runtime.

The goal is interop, not legacy compatibility: an agent-made score must open and remain
editable in MuseScore, and a MuseScore score must open and remain editable by
the agent. The reader accepts MuseScore format 4.60 up to 5.00 and rejects versions
above 5.00 or prior to 4.60. MSCZ and MusicXML inputs with embedded images are rejected.
The default MSCZ writer emits 5.00 format; a bounded 4.70 writer targeting MuseScore 4.7.0–4.7.6
is available for MuseScore 4.x interop where the score stays inside its documented subset.

## What it can do

- Open MSCZ/MSCX, MusicXML/MXL, MIDI, Guitar Pro (GP3–GP7/GPX/PTB) and MNX.
- Create a score from an engine template.
- Read score outlines, query elements/measures/segments/properties/audio tracks,
  and describe element types and style keys.
- Apply atomic edit batches (23 operations) with strict revisions; a failed
  batch rolls back.
- Export score files, PDF/PNG/SVG vector and raster pages, synthesized audio
  (WAV/FLAC/Opus/MP3/AAC) and layout position data directly from the **live**
  in-memory document, with durable publication.
- Hot-load fonts and replace SoundFonts at runtime.

## Quick start (Windows)

Prerequisites: Git, [Pixi](https://pixi.sh/), and the Visual Studio C++ x64
toolchain (MSVC 14.51+).

```powershell
pixi install --locked
pixi run python scripts/bootstrap.py --soundfont
pixi run python scripts/prepare.py
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
```

Run the protocol smoke test against the build:

```powershell
# The dev build links the Pixi native libraries; put them on PATH first.
$env:PATH = "$PWD\.pixi\envs\default\Library\bin;$env:PATH"

$env:ENGRAVE_MCP_BINARY   = "$PWD/build/xmake-windows/engrave-mcp.exe"
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
$env:ENGRAVE_MCP_SOUNDFONT = "$PWD/.runtime/soundfonts/FluidR3Mono_GM.sf3"
pixi run python tests/integration/test_mcp_smoke.py
```

To run the server manually, pass resources explicitly; `--soundfont` is optional:

```powershell
build/xmake-windows/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

A packaged build finds its sibling `assets/` directory automatically. Windows
and Linux build, test and packaging details are in [docs/dev/build.md](docs/dev/build.md).

## The nine tools

| Tool             | What it does                                                                                  |
| ---------------- | --------------------------------------------------------------------------------------------- |
| `score_open`     | Open from `path` or base64 `data` (+ `format`, `filename_hint`, `strict_ids`).                |
| `score_create`   | Create from `template_id` or `template_path` (+ `title`, `subtitle`, `composer`, `lyricist`). |
| `score_close`    | Close a document and free its memory.                                                         |
| `score_outline`  | Title/metadata, parts, measures, signatures, page count, revision.                            |
| `score_query`    | `mode`: `elements`, `properties`, or `audio_tracks`.                                          |
| `score_describe` | `scope`: `types`, `type` (with `type`), or `styles`.                                          |
| `score_edit`     | Atomic `ops` batch with optional `expected_revision`.                                         |
| `score_export`   | `kind`: `score`, `render`, `audio`, or `positions`.                                           |
| `runtime_info`   | `action`: `get`, `add_font`, or `set_soundfonts`.                                             |

Every successful result carries `ok: true`. Failures carry
`ok: false` and `error: {code, message, ...}` and set the MCP `isError` flag.
The full contract is in [docs/dev/tools.md](docs/dev/tools.md) and the generated
schema in [engrave_mcp/tools.json](engrave_mcp/tools.json).

Schematic of the arguments and the `structuredContent` each call returns:

```json
{"method":"tools/call","params":{"name":"score_create",
  "arguments":{"template_id":"Treble_Clef","title":"Prelude"}}}
{"ok":true,"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1","revision":"1","diagnostics":[]}
```

```json
{"method":"tools/call","params":{"name":"score_export",
  "arguments":{"document":"doc-807a1e3391c253a993f2c46ac95aedb4-1","kind":"render",
    "destination":{"kind":"file","path":"C:/Scores/prelude.pdf"},
    "options":{"format":"pdf"}}}}
{"ok":true,"source_revision":"1",
 "artifacts":[{"path":"C:/Scores/prelude.pdf","mime":"application/pdf","size":"48213"}]}
```

## Client configuration

After packaging (for example to `artifacts/windows-next`), register the server
in your MCP client configuration using absolute paths:

```json
{
  "mcpServers": {
    "engrave-mcp": {
      "command": "C:/path/to/engrave-mcp/artifacts/windows-next/engrave-mcp.exe",
      "args": [
        "--soundfont",
        "C:/path/to/engrave-mcp/artifacts/windows-next/soundfonts/FluidR3Mono_GM.sf3"
      ]
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
complex/protected DACLs and ACL merging, SMB network share authentication, device namespaces
and extended-length paths, cross-volume or multi-file transactional publication,
and interactive/streaming playback. See [docs/dev/scope.md](docs/dev/scope.md) for the triage policy.

## Repository layout

```
engrave_mcp/     MCP transport, tool registry, JSON codecs, live export, publication
vendor/engrave/  pinned engrave engine (submodule, pinned by commit)
xmake.lua        host build; includes engine build from vendor/engrave
scripts/         bootstrap, build, packaging dev tools
tests/           acceptance lane runner, integration tests, support, benchmarks, test data
docs/dev/        design, tool contract, build, validation, engine fork guide
```

## Documentation

- [Design and rationale](docs/dev/design.md)
- [Tool contract](docs/dev/tools.md)
- [Build and test](docs/dev/build.md)
- [Validation and test lanes](docs/dev/validation.md)
- [Engine fork guide](docs/dev/fork.md)
- [Supported scope and non-goals](docs/dev/scope.md)
- [Open defects and limitations](docs/dev/defects.md)
- [Branching and tags](docs/dev/branching.md)
- [Code style guide](docs/dev/code-style-guide.md)
- [中文说明](README.zh-CN.md)
