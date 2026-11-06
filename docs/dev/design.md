# Design and rationale

This document explains how `engrave-mcp` is put together and why. It describes the current design and rationale. The exact wire contract is defined in [tools.md](tools.md).

## 1. System architecture

```
MCP client
   │  stdio JSON-RPC (one request per line, max 4 MiB)
   ▼
Host layer (engrave_mcp/)
   ├─ transport: reader thread, bounded admission, writer thread, cancellation
   ├─ registry + JSON codecs: the nine-tool contract
   ├─ live export: render/encode on in-memory document, staging, publication
   └─ watchdog + process-wide publication gate
   │  serialized calls
   ▼
Owner thread (one per process)
   ├─ up to 16 open documents
   ├─ engrave core: DOM, layout, import/export, positions
   ├─ Cairo renderer
   └─ FluidSynth offline audio + encoders
```

Host sources in `engrave_mcp/` include only the engine's public headers under `engrave/...`, enforced by `scripts/check_native_includes.py` during builds. The engine is a separate library in `vendor/engrave` compiled into the host binary.

## 2. One owner thread, multiple documents

All score engine operations execute on a single dedicated owner thread per process: open, create, query, edit, render, audio synthesis, and close. Transport threads handle JSON-RPC parsing and scheduling but never invoke engine functions directly.

Up to 16 documents can remain open concurrently in one session; they share the engine and are served sequentially. Score engraving involves deep pointer structures and process-global state (font tables, style metrics, rendering contexts, and offline synthesis state). Serializing calls on one thread avoids complex multi-threaded synchronization in the engraving core. A long-running export blocks operations on other open documents in the same server process; cooperative cancellation allows clients to abort stuck or superseded requests.

## 3. Direct in-memory export

Exports run directly against the live in-memory score document on the owner thread. In-memory rendering avoids layout reflow and metadata changes that occur during file serialization and reopening, reduces export latency to milliseconds, and removes child-process lifecycle complexity.

The tradeoff is deliberate: an unhandled fault in rendering, audio synthesis, or score writing terminates the server process and loses unsaved edits across open documents. There is no internal sandbox mode. For untrusted input files or isolated batch processing, isolation is handled operationally by running separate, disposable server processes.

## 4. Durable publication

Exports must never leave incomplete or corrupted files at the requested destination:

1. The host creates a private staging directory next to the destination (`0700` on POSIX, a restricted DACL on Windows).
2. Artifacts are generated and size-checked in staging against the configured resource budget.
3. Completed files are flushed to disk (`FlushFileBuffers` on Windows, `fsync` on POSIX) and atomically renamed into place (`MoveFileExW` with `REPLACE_EXISTING | WRITE_THROUGH` on Windows; `rename` and directory `fsync` on POSIX).
4. On Windows, existing DACLs at the destination are preserved on a best-effort basis.

Publication is atomic per file rather than across an entire multi-file export. If a later page of a multi-page export fails, previously published pages remain visible, and the returned error payload lists all successfully published files in `published_artifacts`. Staging directory cleanup on failure or cancellation is best effort.

## 5. Cancellation and the watchdog

Clients cancel running operations using the standard MCP `notifications/cancelled` notification. The transport thread flags the request and dispatches an Asio cancellation; pending queued calls are dropped immediately.

The engine polls cooperative cancellation hooks at defined boundaries:

- Page boundaries and draw preparation in visual rendering
- Synthesis blocks and audio encoder chunk writes in audio rendering
- Periodic byte intervals (every ≤4 KiB) during score conversion and export

A cancelled operation stops execution and publishes no partial output at the destination.

If a cancelled live export remains blocked on the owner thread for more than 5 seconds, the watchdog intervenes. The watchdog acquires the process publication gate to allow any in-flight write to complete cleanly, then terminates the process with exit code `70`. If stdout write errors occur, the process exits with code `74`. Worker threads are never terminated asynchronously.

## 6. Runtime quotas and bounds

| Bound                    | Value                 | Enforcement point    |
| ------------------------ | --------------------- | -------------------- |
| Transport request line   | 4 MiB                 | `runtime_limits.h`   |
| Pending input queue      | 64 lines / 32 MiB     | Transport reader     |
| In-flight tool calls     | 64                    | Transport admission  |
| Response payload         | 16 MiB                | Transport writer     |
| Output message queue     | 48 MiB / 256 messages | Transport writer     |
| Open documents           | 16 concurrent         | `host_api.cpp`       |
| Decoded score input      | 8 MiB                 | Engine input check   |
| Edit batch size          | 1,024 operations      | Tool registry        |
| Export output budget     | 1 GiB                 | `ExportLimits`       |
| Advisory export deadline | 120 seconds           | `ExportLimits`       |
| Diagnostic text length   | 16 KiB                | Report serialization |
| Query result items       | 4,096 elements        | Engine query handler |

The export budget is enforced during rendering and staging before publication; it is a file size quota rather than a hard limit on process virtual memory. Audio exports allocate half the budget to the encoder intermediate file and half to the staged artifact.

## 7. Performance characteristics and limitations

- Vector rendering (PDF, SVG) runs in single-digit milliseconds for standard scores once fonts are loaded.
- Audio synthesis latency scales with score duration, voice count, and SoundFont complexity.
- Audio synthesis cancellation contains cooperative polling, but thread scheduling variance on Windows can cause cancellation latency to exceed 30 ms under high system load.
- Memory usage: logical file quotas are enforced, but process virtual memory is not capped by an OS sandbox.

## 8. Non-goals

- No concurrent editing of a single document across multiple threads.
- No multi-document transactional rollback.
- No background task scheduling or streaming playback sessions.
- No in-process memory isolation or crash recovery for corrupted score structures.
- No support for MuseScore file formats older than version 4.60.
- No GUI, desktop display, screen capture, or windowing dependencies.
