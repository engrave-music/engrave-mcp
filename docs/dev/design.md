# Design and rationale

This document explains how `engrave-mcp` is put together. The exact wire contract is defined in [tools.md](tools.md).

## 1. System architecture

```
MCP client
   │  stdio JSON-RPC (one request per line, max 4 MiB)
   ▼
Host layer (engrave_mcp/)
   ├─ transport: reader thread, bounded admission, writer thread, cancellation
   ├─ registry + JSON codecs: the fourteen-tool contract
   ├─ session: owner dispatch, audio export queue (session I/O thread)
   ├─ live export: render/encode on in-memory document, staging, publication
   └─ watchdog + process-wide publication gate
   │  serialized calls                        │  prepared audio exports, one at a time
   ▼                                          ▼
Owner thread (one per process)             Audio lane thread (one per process)
   ├─ up to 16 open documents                 ├─ PreparedAudio::run: synthesis, mix and
   ├─ engrave core: DOM, layout,              │  encode straight into host staging
   │  import/export, snapshots, positions     └─ audio export publication and report
   ├─ Cairo renderer
   └─ audio setup: score walk, track and SoundFont loads (prepare_audio)

Engine-internal pools: per-track synthesis workers and an encoder thread per
audio render; PNG deflate workers for multi-page PNG renders.
```

Host sources in `engrave_mcp/` include only the engine's public headers under `engrave/...`, enforced by `scripts/check_native_includes.py` during builds. The engine is a separate library in `vendor/engrave` compiled into the host binary. For the data-oriented architecture of the engine's offline audio render pipeline, event stores, and synthesis, see [vendor/engrave/docs/playback.md](../../vendor/engrave/docs/playback.md).

## 2. Single-writer DOM owner thread, multiple documents

Every operation that reads or writes a score runs on a single dedicated owner thread per process: open, create, query, edit, layout, visual rendering, snapshots, the setup half of an audio export, and close. Transport threads handle JSON-RPC parsing and scheduling but never invoke engine functions directly.

`score_edit` applies batches atomically by default (using an internal pre-batch snapshot). Set `atomic: false` to retain the successfully applied prefix after a zero-mutation execution rejection.

Up to 16 documents can remain open concurrently in one session; they share the engine and serialize DOM access on the owner thread. Score engraving involves deep pointer structures and shared state (font tables, style metrics, rendering contexts), so single-threaded DOM ownership avoids complex multi-threaded synchronization in the engraving core. The engine parallelizes inside a call where the work is DOM-free: per-track audio synthesis, audio encoding and PNG deflate (multi-page PNG exports render up to 16 pages per engine call for that reason, with bytes identical to one call per page).

### Audio lane

An audio export (`async_session.cpp`, `api_output.cpp`) has two halves:

1. **Setup on the owner thread** (`prepare_audio_export`): validates the destination and mix, creates the staging directory and calls `Document::prepare_audio`, which walks the score and loads every track. The resulting `PreparedAudioExport` refers to no `Document`; its `source_revision` is the revision setup read.
2. **Render on the audio lane** (`finish_audio_export`): `PreparedAudio::run` synthesizes and encodes straight into staging, the `PreparedAudio` is destroyed (freeing the Engine's single audio render), then the lane publishes and writes the report itself.

While the lane renders, the owner keeps serving every other request, including edits, `score_close` of the exporting document and `runtime_configure` (the running render keeps the SoundFonts it loaded; the next export uses the new selection). Audio exports serialize on the lane: the session I/O thread queues a later one (within the 64 in-flight calls) and dispatches its setup only after the lane reports free, which happens after the previous `PreparedAudio` is destroyed. Queued exports never occupy `owner_io`, so cancelling one, or shutting down, drops it at once.

Session teardown sets the shared stop flag, aborts queued audio exports and releases the owner's work guard. After `owner_io` drains, the owner thread releases the lane's work guard and joins the lane thread before destroying `HostApi`, so every `PreparedAudio` is gone before the Engine. The lane never waits on the owner thread.

## 3. Direct in-memory export

Exports run directly against the live in-memory score document. In-memory rendering avoids layout reflow and metadata changes that occur during file serialization and reopening, reduces visual export latency to milliseconds, and removes child-process lifecycle complexity.

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

The engine currently polls cooperative cancellation hooks at defined boundaries:

- Page boundaries and draw preparation in visual rendering
- Audio setup, synthesis blocks (about every millisecond while tracks synthesize in parallel) and encoder writes in audio rendering
- Periodic byte intervals (every ≤4 KiB) during score conversion and export, including the internal snapshot that `Document::capture_snapshot()` writes for an atomic `score_edit` or `score_write_phrase` rollback and for a named `checkpoint`

A cancelled snapshot capture returns `Cancelled` before the batch runs or the checkpoint is stored: the revision is unchanged and the document is not poisoned. A prefix-mode `score_edit` (`atomic: false`) takes no snapshot, so the host checks the flag once before the batch starts.

If cancellation arrives before publication begins, the operation stops execution and publishes no partial output at the destination. Once publication has started, however, a late cancellation does not abort already-staged writes: publication proceeds, and the export can finish successfully or return an I/O error if a subsequent file write fails, reporting exactly the files that were already published (`published_artifacts`).

A running audio render on the lane polls the request's cancellation, session shutdown (EOF) and the stdout-failure flag, so each of them interrupts it; the lane then removes the partial file and its staging directory.

If a cancelled live export remains blocked, a watchdog timer expires after 5 seconds. The watchdog claims the request, waits 250 ms for committed reports, and attempts to acquire the process-wide publication gate before terminating the process with exit code `70`. An audio export stays `EXECUTING` from its owner-thread setup through its render on the lane, so the watchdog covers both halves. Keeping exit `70` is a deliberate policy tradeoff: terminating the process loses unsaved edits in other open documents, but guarantees bounded CPU/memory cleanup rather than leaking a wedged worker. Because the publication gate stays held through report serialization and synchronous stdout delivery (by the owner, or by the lane for audio), unread or blocked stdout can defer the watchdog exit indefinitely, ensuring a file that has become visible is reported before termination. If a stdout write error is detected, the server ceases new publications and exits with code `74` after best-effort cleanup of active tasks (up to 500 ms); `PublicationGate::owner_tasks` counts the running owner task and the running audio-lane task. Staging files may remain, pending reports cannot be delivered, and already-visible files stay on disk. Worker threads are never terminated asynchronously.

## 6. Runtime quotas and bounds

| Bound                              | Current value                                                                   | Enforcement point                                             |
| ---------------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| Transport request line             | 4 MiB                                                                           | `runtime_limits.h`                                            |
| Pending input queue                | 64 lines / 32 MiB                                                               | Transport reader                                              |
| In-flight tool calls               | 64                                                                              | Transport admission                                           |
| Response payload                   | 16 MiB                                                                          | Transport writer                                              |
| Output message queue               | 48 MiB / 256 messages                                                           | Transport writer                                              |
| Open documents                     | 16 concurrent                                                                   | `host_api.cpp`                                                |
| Decoded score input                | 8 MiB                                                                           | Engine input check (`engrave::max_input_bytes`)               |
| Uncompressed file-path score input | 64 MiB                                                                          | `Engine::open(path)` (`engrave::max_uncompressed_file_bytes`) |
| In-flight snapshot write           | 128 MiB (atomic batch); unused part of the 64 MiB checkpoint store (checkpoint) | `WriteBudget` in `Document::capture_snapshot()`               |
| Named checkpoints                  | 64 MiB per document                                                             | `Document::checkpoint()` (`state.h`)                          |
| Edit batch size                    | 1,024 operations                                                                | Tool registry                                                 |
| Export output budget               | 1 GiB (`output_bytes`)                                                          | `ExportLimits` (`runtime_limits.h`)                           |
| Advisory export deadline           | 120 seconds (`deadline`)                                                        | `ExportLimits` (`runtime_limits.h`)                           |
| Export/report preflight text       | 16 KiB (UTF-8 safe)                                                             | Export reports and transport-generated diagnostics            |
| Engine/tool message text           | Up to 16 MiB                                                                    | Not separately bounded (16 MiB response cap)                  |
| Query result items                 | 4,096 elements                                                                  | Engine query handler (`max_query_results`)                    |

`engrave::max_input_bytes` (8 MiB) caps decoded base64 `data` and every `score_open` path that is not uncompressed score XML (`.mscz`, `.mxl`, MIDI, Guitar Pro, MNX) before archive inspection. A path whose extension is `.mscx`, `.musicxml` or `.xml` (case-insensitive), with no conflicting `format`, may be up to `engrave::max_uncompressed_file_bytes` (64 MiB), the same bound `score_io.cpp` applies to each decompressed `.mscz` entry (128 MiB aggregate). A file above 8 MiB must start with `<` (after an optional BOM and whitespace), so a renamed archive is refused before format sniffing can inflate it. Inline base64 `score_open` payloads are already bounded below 4 MiB by the transport line limit. `runtime_info` reports both bounds as `limits.input_bytes` and `limits.uncompressed_file_bytes`.

The snapshot `WriteBudget` charges the uncompressed native archive, its 64 MiB per-entry bound, and the XML escaping and temporary entry buffers (16× the output bound). An atomic batch may write a 128 MiB rollback snapshot; a checkpoint may write only what remains of its document's 64 MiB checkpoint store after the other named checkpoints. Exceeding either returns `ResourceLimit` without mutating the document.

The export budget (1 GiB, `ExportLimits::output_bytes`) is enforced during rendering and staging before publication. It represents the cumulative total output size across all files in a multi-file export (`api_output.cpp`), not a per-file quota or a hard limit on process virtual memory. Audio exports encode straight into host staging with no intermediate copy, so `audio_budget.h` gives the staged file the whole budget as its extent limit (`AudioOptions::max_bytes`) and the budget plus 1 MiB as a write-traffic limit (`max_write_bytes`) for WAV and FLAC header rewrites.

The 16 KiB diagnostic limit (`k_diagnostic_limit` in `api_output.cpp`, and UTF-8 safe truncation in `mcp_transport.cpp`) applies only to export reports and transport-generated diagnostics, not to arbitrary engine or tool messages. Other messages (such as `runtime_info` error details) are not separately bounded and are subject only to the 16 MiB response cap.

### Windows heap policy

On Windows, the host executables (`engrave-mcp.exe` and test/probe binaries) and the engine's test executables embed an application manifest that selects the Windows Segment Heap. The engine library does not embed a heap manifest; programs that embed it choose their own heap. This policy addresses score audio teardown, which frees roughly 1.3 million small objects with over half released on threads other than the allocating thread, where the default NT heap is slow. On the dense-audio test, the Segment Heap lowers synthesis and encode cleanup p95 by about 25–30% for approximately 6.5% more peak working set. An alternative allocator (mimalloc) cuts cleanup by ~47% but costs ~24% more memory and requires an extra DLL, so it is not adopted.

## 7. Performance characteristics and limitations

- Under measured test conditions, vector rendering (PDF, SVG) for standard scores once fonts are loaded has been observed in single-digit milliseconds (an observation under measured conditions, not an architectural guarantee).
- Audio synthesis latency scales with score duration, voice count, and SoundFont complexity. Only an audio export's setup occupies the owner thread; other requests run while it renders, and audio exports wait for each other.
- Audio cancellation is observed within a fraction of a millisecond; the time until the export call returns is dominated by tearing down playback events and the audio graph (tens of milliseconds for dense scores, slower on Windows).
- Memory usage: logical file quotas are enforced, but process virtual memory is not capped by an OS sandbox.

## 8. Non-goals

- No concurrent editing of a single document across multiple threads.
- No multi-document transactional rollback (single-document `score_edit` batches are atomic by default; `atomic: false` opts into prefix-commit).
- No in-server filesystem sandboxing, path allowlists, or deprecated MCP `roots` enforcement (workspace permissions belong to the host's OS sandbox and client tool approvals).
- No stateful asynchronous job APIs (background job handles or polling across tool calls) or streaming playback sessions. Tool calls remain request-scoped.
- No in-process memory isolation or crash recovery for corrupted score structures.
- No support for MuseScore file formats older than version 4.60.
- No GUI, desktop display, screen capture, or windowing dependencies.
