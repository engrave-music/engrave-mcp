# engrave-mcp

`engrave-mcp` is a native C++ MCP server that gives LLM agents a real music
engraving engine: create, inspect, edit, render and export scores over stdio
JSON-RPC.

The engine is **engrave**, a pinned hard fork of the MuseScore 5.0 core
([github.com/Locietta/engrave](https://github.com/Locietta/engrave), consumed as
`vendor/engrave`). It is compiled into the server. There is no Qt, Node.js,
WebAssembly or desktop MuseScore dependency at runtime.

The goal is interop, not compatibility: an agent-made score must open and remain
editable in MuseScore, and a MuseScore score must open and remain editable by
the agent. MuseScore's legacy surface is not a goal. Only scores from MuseScore
4.6 onward open. The default MSCZ writer emits 5.00; a bounded 4.70 writer is
available for MuseScore 4.x interop where the score stays inside its documented
subset.

## What it can do

- Open MSCZ/MSCX, MusicXML/MXL, MIDI, Guitar Pro (GP3–GP7/GPX/PTB) and MNX.
- Create a score from an engine template.
- Read the outline, query elements/measures/segments/properties/audio tracks,
  and describe element types and style keys.
- Apply atomic edit batches (23 operations) with strict revisions; a failed
  batch rolls back.
- Export score files, PDF/PNG/SVG pages, audio (WAV/FLAC/Opus/MP3/AAC) and
  position data from the **live** document, with durable publication.
- Hot-load fonts and replace SoundFonts at runtime.

## Quick start (Windows)

Prerequisites: Git, [Pixi](https://pixi.sh/), and the Visual Studio C++ x64
toolchain.

```powershell
pixi install -e native --locked
pixi run -e native python scripts/bootstrap.py --soundfont
pixi run -e native python scripts/prepare.py
pwsh -NoProfile -File scripts/build-native.ps1 -Jobs 8
```

Run the protocol smoke test against the build:

```powershell
# The dev build links the Pixi native libraries; put them on PATH first.
$env:PATH = "$PWD\.pixi\envs\native\Library\bin;$env:PATH"

$env:ENGRAVE_MCP_BINARY   = "$PWD/build/xmake-windows/engrave-mcp.exe"
$env:ENGRAVE_MCP_RESOURCES = "$PWD/.runtime/assets"
$env:ENGRAVE_MCP_SOUNDFONT = "$PWD/.runtime/soundfonts/FluidR3Mono_GM.sf3"
pixi run -e native python scripts/mcp_smoke.py
```

To run the server by hand, pass resources explicitly; `--soundfont` is optional:

```powershell
build/xmake-windows/engrave-mcp.exe --resources .runtime/assets `
  --soundfont .runtime/soundfonts/FluidR3Mono_GM.sf3
```

A packaged build finds its sibling `assets/` directory automatically. Windows
and Linux build, test and packaging details are in [docs/dev/build.md](docs/dev/build.md).

## The nine tools

| Tool | What it does |
|---|---|
| `score_open` | Open from `path` or base64 `data` (+ `format`, `filename_hint`, `strict_ids`). |
| `score_create` | Create from `template_id` or `template_path` (+ `title`, `subtitle`, `composer`, `lyricist`). |
| `score_close` | Close a document and free its memory. |
| `score_outline` | Title/metadata, parts, measures, signatures, page count, revision. |
| `score_query` | `mode`: `elements`, `properties`, or `audio_tracks`. |
| `score_describe` | `scope`: `types`, `type` (with `type`), or `styles`. |
| `score_edit` | Atomic `ops` batch with optional `expected_revision`. |
| `score_export` | `kind`: `score`, `render`, `audio`, or `positions`. |
| `runtime_info` | `action`: `get`, `add_font`, or `set_soundfonts`. |

Every successful result carries `ok: true`. Failures carry
`ok: false` and `error: {code, message, ...}` and set the MCP `isError` flag.
The full contract is in [docs/dev/tools.md](docs/dev/tools.md) and the generated
schema in [native/tools.json](native/tools.json).

Schematic of the arguments and the `structuredContent` each call returns. This is
not a wire transcript: a real session starts with `initialize` and wraps each
call in a JSON-RPC 2.0 request/response with an id.

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
with absolute paths:

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

## Design in one screen

- **One engine, one owner thread.** All document work is serialized on a
  dedicated owner thread. Up to 16 documents can be open; they share the engine.
  Concurrent editing of one document is not supported, by design.
- **Live export.** Exports run against the in-memory document. The earlier
  worker/snapshot/job design was deleted: save-and-reopen changed layout, added
  hundreds of milliseconds and created a large failure surface.
- **Durable publication.** Output is written to a private staging directory,
  flushed, then atomically renamed into place. Multi-file exports are per-file
  atomic, not a transaction; an error reports exactly which files were already
  published.
- **Cooperative cancellation + watchdog.** Long operations poll a cancellation
  flag. A cancelled export that stays blocked for 5 seconds triggers a
  process-wide publication gate and exit 70 rather than killing threads.
- **Explicit quotas.** 4 MiB request line, 64 in-flight calls, 16 MiB response,
  1 GiB export budget, 8 MiB decoded input, 16 documents.

The reasoning behind these choices is in [docs/dev/design.md](docs/dev/design.md).

## Scope and non-goals

Supported: ordinary single-user local filesystems on Windows and Linux, ordinary
inherited permissions, the documented path syntax, and scores from MuseScore 4.6
onward.

Explicit non-goals: MuseScore 1.x–4.5 readers, full MuseScore compatibility,
complex/protected DACLs and ACL merging, SMB authentication, device namespaces
and extended-length paths, cross-volume or multi-file transactional
publication, and interactive/streaming playback. See
[docs/dev/scope.md](docs/dev/scope.md) for the triage rule.

## Repository layout

```
native/            MCP transport, registry, JSON codecs, live export, publication
vendor/engrave   pinned engrave engine (submodule, tag engrave-31)
xmake.lua          host build; includes the engine build from vendor/engrave
scripts/           bootstrap, build, packaging, lane runner, benchmarks
tests/             fixtures, golden baselines, audio corpora
docs/dev/          design, tool contract, build, validation, fork ledger, history
```

## Documentation

- [Design and rationale](docs/dev/design.md)
- [Tool contract](docs/dev/tools.md)
- [Build and test](docs/dev/build.md)
- [Validation lanes and accepted evidence](docs/dev/validation.md)
- [Engine fork and pin ledger](docs/dev/fork.md)
- [Supported scope and non-goals](docs/dev/scope.md)
- [Open defects](docs/dev/defects.md)
- [Project history](docs/dev/history.md)
- [Branching and tags](docs/dev/branching.md)
- [中文说明](README.zh-CN.md)
