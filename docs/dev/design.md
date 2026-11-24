# Design and rationale

This document explains how `engrave-mcp` is put together, why, and the active architectural contracts workers must target. The exact wire contract is defined in [tools.md](tools.md).

## 1. System architecture

```
MCP client
   │  stdio JSON-RPC (one request per line, max 4 MiB)
   ▼
Host layer (engrave_mcp/)
   ├─ transport: reader thread, bounded admission, writer thread, cancellation
   ├─ registry + JSON codecs: the fourteen-tool contract
   ├─ live export: in-memory render/audio prep, direct staging, publication
   └─ watchdog + process-wide publication gate
   │  serialized DOM calls
   ▼
Owner thread (one per process)
   ├─ up to 16 open documents
   ├─ engrave core: DOM, layout, import/export, snapshots, positions
   ├─ Cairo renderer
   └─ audio playback snapshot / track lowering (DOM-read phase)
   │  immutable DOM-free audio job (one serialized lane)
   ▼
Audio execution lane (target: Phase B4)
   ├─ parallel per-track FluidSynth synthesis workers (deterministic fixed-order mix)
   └─ async encoder writing directly to host staging
```

Host sources in `engrave_mcp/` include only the engine's public headers under `engrave/...`, enforced by `scripts/check_native_includes.py` during builds. The engine is a separate library in `vendor/engrave` compiled into the host binary. For the data-oriented architecture of the engine's offline audio render pipeline, event stores, and synthesis, see [vendor/engrave/docs/playback.md](../../vendor/engrave/docs/playback.md).

## 2. Single-writer DOM owner thread, off-thread audio execution

All operations that touch the score DOM serialize on a single dedicated owner thread per process: open, create, query, edit, layout, visual rendering, snapshots, close, and the initial audio playback lowering step. Transport threads handle JSON-RPC parsing and scheduling but never invoke DOM engine functions directly.

`score_edit` applies batches atomically by default (using an internal pre-batch snapshot). Set `atomic: false` to retain the successfully applied prefix after a zero-mutation execution rejection.

Up to 16 documents can remain open concurrently in one session; they share the engine and serialize DOM access on the owner thread. Score engraving involves deep pointer structures and shared state (font tables, style metrics, rendering contexts), so single-threaded DOM ownership avoids complex multi-threaded synchronization in the engraving core.

Offline audio synthesis, mixing, and encoding touch no DOM pointers once track lowering completes on the owner thread. Rather than blocking queries, edits, and saves across all open documents for the duration of a multi-second audio export, the target execution model (Phase B4) runs the DOM-free audio stages on a single serialized background audio lane — using an internal worker pool for parallel per-track synthesis (with per-task FTZ/DAZ and deterministic fixed-order mixing) and an async encoder pipeline — while keeping the MCP `score_export` tool call request-scoped. Serializing exports on one audio lane bounds memory and avoids cross-export FluidSynth/LAME contention.

## 3. Direct in-memory export

Exports run directly from the live in-memory score document: visual exports render on the owner thread, and audio exports lower their DOM-free playback snapshot directly from the live document before handing off to the audio lane. In-memory preparation avoids layout reflow and metadata changes that occur during file serialization and reopening, reduces visual export latency to milliseconds, and removes child-process lifecycle complexity.

The tradeoff is deliberate: an unhandled fault in rendering, audio synthesis, or score writing terminates the server process and loses unsaved edits across open documents. There is no internal sandbox mode. Crash and session isolation for untrusted input files or batch processing is handled operationally by running separate, disposable server processes; filesystem confinement is owned by host-side OS sandboxing and client tool approvals.

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
- Synthesis blocks, worker/ring waits, and audio encoder chunk writes in audio rendering
- Periodic byte intervals (every ≤4 KiB) during score conversion, export, and internal snapshot capture (`EditMode::Atomic` rollback snapshots and named `checkpoint` serialization; snapshot cancellation lands in Phase B3)

If cancellation arrives before publication begins, the operation stops execution and publishes no partial output at the destination. Once publication has started, however, a late cancellation does not abort already-staged writes: publication proceeds, and the export can finish successfully or return an I/O error if a subsequent file write fails, reporting exactly the files that were already published (`published_artifacts`).

If a cancelled live export remains blocked (on the owner thread or the background audio lane), a watchdog timer expires after 5 seconds. The watchdog claims the request, waits 250 ms for committed reports, and attempts to acquire the process-wide publication gate before terminating the process with exit code `70`. Even after audio leaves the owner thread, keeping exit `70` is a deliberate policy tradeoff: terminating the process loses unsaved edits in other open documents, but guarantees bounded CPU/memory cleanup instead of leaking a wedged worker until shutdown. Because the publication gate stays held through report serialization and synchronous stdout delivery, unread or blocked stdout can defer the watchdog exit indefinitely, ensuring a file that has become visible is reported before termination. If a stdout write error is detected, the server ceases new publications and exits with code `74` after best-effort cleanup of active owner and background audio tasks (up to 500 ms); staging files may remain, pending reports cannot be delivered, and already-visible files stay on disk. Worker threads are never terminated asynchronously.

## 6. Runtime quotas and bounds

| Bound                        | Value                                               | Enforcement point                                  |
| ---------------------------- | --------------------------------------------------- | -------------------------------------------------- |
| Transport request line       | 4 MiB                                               | `runtime_limits.h`                                 |
| Pending input queue          | 64 lines / 32 MiB                                   | Transport reader                                   |
| In-flight tool calls         | 64                                                  | Transport admission                                |
| Response payload             | 16 MiB                                              | Transport writer                                   |
| Output message queue         | 48 MiB / 256 messages                               | Transport writer                                   |
| Open documents               | 16 concurrent                                       | `host_api.cpp`                                     |
| Decoded score input          | 8 MiB (target: 64 MiB uncompressed file-path limit) | Engine input check (`engrave::max_input_bytes`)    |
| Named checkpoints            | 64 MiB per document                                 | `Document::checkpoint()` (`state.h`)               |
| In-flight snapshot write     | Bounded by snapshot `WriteBudget` (Phase B3)        | `Document::capture_snapshot()`                     |
| Edit batch size              | 1,024 operations                                    | Tool registry                                      |
| Export output budget         | 1 GiB (`output_bytes`)                              | `ExportLimits` (`runtime_limits.h`)                |
| Advisory export deadline     | 120 seconds (`deadline`)                            | `ExportLimits` (`runtime_limits.h`)                |
| Export/report preflight text | 16 KiB (UTF-8 safe)                                 | Export reports and transport-generated diagnostics |
| Engine/tool message text     | Up to 16 MiB                                        | Not separately bounded (16 MiB response cap)       |
| Query result items           | 4,096 elements                                      | Engine query handler (`max_query_results`)         |

Inline base64 `score_open` payloads are already bounded below 4 MiB by the transport line limit, whereas on-disk `.mscz` archives allow individual XML entries up to 64 MiB (128 MiB aggregate in `score_io.cpp`). Aligning the file-path uncompressed score input ceiling (`.mscx`, `.musicxml`) with the 64 MiB single-entry bound removes the artificial mismatch where a large orchestral score opens as `.mscz` but fails as uncompressed XML.

The export budget (1 GiB, `ExportLimits::output_bytes`) is enforced during rendering and staging before publication. It represents the cumulative total output size across all files in a multi-file export (`api_output.cpp`), not a per-file quota or a hard limit on process virtual memory. Audio exports should encode directly to the host's private staging file under the full 1 GiB budget (plus up to 1 MiB for header-rewrite traffic), eliminating the legacy cache-directory temp-file copy and its 512 MiB intermediate / 512 MiB staging split (retired in Phase B4.4).

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
- No multi-document transactional rollback (single-document `score_edit` batches are atomic by default; `atomic: false` opts into prefix-commit).
- No in-server filesystem sandboxing, path allowlists, or deprecated MCP `roots` enforcement (workspace permissions belong to the host's OS sandbox and client tool approvals).
- No stateful asynchronous job APIs (background job handles or polling across tool calls) or streaming playback sessions. Tool calls remain request-scoped; internal worker threads and the serialized audio lane execute strictly within the lifetime of a single export call.
- No in-process memory isolation or crash recovery for corrupted score structures.
- No support for MuseScore file formats older than version 4.60.
- No GUI, desktop display, screen capture, or windowing dependencies.
