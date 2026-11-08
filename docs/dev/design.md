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

If cancellation arrives before publication begins, the operation stops execution and publishes no partial output at the destination. Once publication has started, however, a late cancellation does not abort already-staged writes: publication proceeds, and the export can finish successfully or return an I/O error if a subsequent file write fails, reporting exactly the files that were already published (`published_artifacts`).

If a cancelled live export remains blocked on the owner thread, a watchdog timer expires after 5 seconds. The watchdog claims the request, waits 250 ms for committed reports, and attempts to acquire the process-wide publication gate before terminating the process with exit code `70`. Because the publication gate stays held through report serialization and synchronous stdout delivery, unread or blocked stdout on the owner thread can defer the watchdog exit indefinitely, ensuring a file that has become visible is reported before termination. If a stdout write error is detected, the server ceases new publications and exits with code `74` after best-effort cleanup of active owners (up to 500 ms); staging files may remain, pending reports cannot be delivered, and already-visible files stay on disk. Worker threads are never terminated asynchronously.

## 6. Runtime quotas and bounds

| Bound                        | Value                    | Enforcement point                                  |
| ---------------------------- | ------------------------ | -------------------------------------------------- |
| Transport request line       | 4 MiB                    | `runtime_limits.h`                                 |
| Pending input queue          | 64 lines / 32 MiB        | Transport reader                                   |
| In-flight tool calls         | 64                       | Transport admission                                |
| Response payload             | 16 MiB                   | Transport writer                                   |
| Output message queue         | 48 MiB / 256 messages    | Transport writer                                   |
| Open documents               | 16 concurrent            | `host_api.cpp`                                     |
| Decoded score input          | 8 MiB                    | Engine input check                                 |
| Edit batch size              | 1,024 operations         | Tool registry                                      |
| Export output budget         | 1 GiB (`output_bytes`)   | `ExportLimits` (`runtime_limits.h`)                |
| Advisory export deadline     | 120 seconds (`deadline`) | `ExportLimits` (`runtime_limits.h`)                |
| Export/report preflight text | 16 KiB (UTF-8 safe)      | Export reports and transport-generated diagnostics |
| Engine/tool message text     | Up to 16 MiB             | Not separately bounded (16 MiB response cap)       |
| Query result items           | 4,096 elements           | Engine query handler (`max_query_results`)         |

The export budget (1 GiB, `ExportLimits::output_bytes`) is enforced during rendering and staging before publication. It represents the cumulative total output size across all files in a multi-file export (`api_output.cpp:406`), not a per-file quota or a hard limit on process virtual memory. Audio exports allocate half the budget (512 MiB) to the encoder intermediate file and half to the staged artifact, plus up to 1 MiB for header-rewrite traffic.

The 16 KiB diagnostic limit (`k_diagnostic_limit` in `api_output.cpp`, and UTF-8 safe truncation in `mcp_transport.cpp`) applies only to export reports and transport-generated diagnostics, not to arbitrary engine or tool messages. Other messages (such as `runtime_info` error details) are not separately bounded and are subject only to the 16 MiB response cap.

### Windows heap policy

On Windows, the host executables (`engrave-mcp.exe` and test/probe binaries) and the engine's test executables embed an application manifest that selects the Windows Segment Heap. The engine library does not embed a heap manifest; programs that embed it choose their own heap. This policy addresses score audio teardown, which frees roughly 1.3 million small objects with over half released on threads other than the allocating thread, where the default NT heap is slow. On the dense-audio test, the Segment Heap lowers synthesis and encode cleanup p95 by about 25–30% for approximately 6.5% more peak working set. An alternative allocator (mimalloc) cuts cleanup by ~47% but costs ~24% more memory and requires an extra DLL, so it is not adopted.

## 7. Performance characteristics and limitations

- Under measured test conditions, vector rendering (PDF, SVG) for standard scores once fonts are loaded has been observed in single-digit milliseconds (an observation under measured conditions, not an architectural guarantee).
- Audio synthesis latency scales with score duration, voice count, and SoundFont complexity.
- Audio cancellation is observed within a fraction of a millisecond; the time until the export call returns is dominated by tearing down playback events and the audio graph (tens of milliseconds for dense scores, slower on Windows).
- Memory usage: logical file quotas are enforced, but process virtual memory is not capped by an OS sandbox.

## 8. Non-goals

- No concurrent editing of a single document across multiple threads.
- No multi-document transactional rollback.
- No background task scheduling or streaming playback sessions.
- No in-process memory isolation or crash recovery for corrupted score structures.
- No support for MuseScore file formats older than version 4.60.
- No GUI, desktop display, screen capture, or windowing dependencies.
