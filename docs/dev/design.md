# Design and rationale

This document explains how `engrave-mcp` is put together and why. It describes
the current design only; the history of how it got here is in
[history.md](history.md), and the exact wire contract is in [tools.md](tools.md).

## 1. The shape of the system

```
MCP client
   │  stdio JSON-RPC (one request per line, max 4 MiB)
   ▼
Host layer (native/)
   ├─ transport: reader thread, bounded admission, writer thread, cancellation
   ├─ registry + JSON codecs: the nine-tool v2 contract
   ├─ live export: render/encode on the document, staging, publication
   └─ watchdog + process-wide publication gate
   │  serialized calls
   ▼
Owner thread (one per process)
   ├─ up to 16 open documents
   ├─ engrave core: DOM, layout, import/export, positions
   ├─ Cairo renderer
   └─ FluidSynth offline audio + encoders
```

The host only includes the engine's public headers under `engrave/...`;
`native_include_check` enforces that before host targets build. The engine is a
separate, independently buildable project (`vendor/musescore/xmake/engrave.lua`)
that the host includes.

## 2. One owner thread, several documents

Every engine operation runs on one dedicated owner thread: open, create, query,
edit, render, audio, close. The transport thread parses JSON-RPC and never
touches the engine. Up to 16 documents may be open at once; they share the
engine and are served one operation at a time.

Why not per-document threads? MuseScore's score is a deep pointer graph plus
process-global state: font metrics, style tables, print/render state and the
`NO_THREADS` FluidSynth instance. Safe concurrent mutation would require
rewriting the engraving algorithms around fine-grained locking. Serializing is
both faster to reason about and sufficient for agent workloads, where one agent
edits several documents but does not need parallel edits of the same one.

The cost is explicit: a long operation blocks every other document. That is
acceptable because exports are now milliseconds, not hundreds of milliseconds.

## 3. Live export, and why the workers were removed

Exports run against the live in-memory document. There is no worker process, no
job manager, no snapshot cache and no export-mode switch.

The earlier design captured the document to an MSCZ snapshot and rendered it in
a child process. It was deleted for three reasons:

1. **It changed the output.** Save-and-reopen is not bit-identical: layout can
   reflow and provenance fields can change. Snapshot renders differed from a
   direct render beyond the pinned allowance (defects 012 and 015). An export
   that does not match what the agent sees is a correctness problem, not a
   performance one.
2. **It dominated latency.** Capture, process spawn, engine creation, reopen and
   layout added roughly 200–260 ms per export. A warm live PDF/SVG is now a few
   milliseconds of drawing plus publication.
3. **It multiplied failure modes.** Child lifecycle, IPC, leases, snapshots and
   job cancellation were most of the host's complexity.

The trade is deliberate: a crash in render, audio or the score writer terminates
the session and loses unsaved edits in every open document. Open, edit, import
and 4.70 conversion already ran in the owner before M4, so only export was ever
isolated. There is no in-product isolation mode. For untrusted input or a
runaway export, the boundary is the server process: run one disposable,
supervised server per file. That is an operational pattern, not a code path.

## 4. Durable publication

A successful export must never leave a half-written file at the destination.

1. The host creates a private staging directory next to the destination
   (`0700` on POSIX, a protected DACL on Windows).
2. Artifacts are written and size-checked in staging.
3. Each file is flushed (`FlushFileBuffers` on Windows, `fsync` on POSIX), then
   atomically renamed over the destination (`MoveFileExW` with
   `REPLACE_EXISTING | WRITE_THROUGH`, or `rename` followed by a parent-directory
   `fsync`).
4. On Windows the destination's ordinary inherited/existing DACL is preserved on
   a best-effort basis.

Publication is per file, not a transaction. If page 3 of an SVG export fails,
pages 1 and 2 remain visible and the error's `published_artifacts` lists exactly
them. Staging cleanup after a failure or cancellation is best effort; created
parent directories may remain. The supported scope and its non-goals are in
[scope.md](scope.md).

## 5. Cancellation and the watchdog

The client cancels with the standard MCP `notifications/cancelled`. The
transport marks the request and emits a terminal Asio cancellation; queued
requests are dropped. The engine polls a cancellation predicate at coarse,
documented points: before drawing and at page boundaries in render, around
synthesis blocks and encoder writes in audio, and every ≤4 KiB in 4.70 XML
conversion. A cancelled operation publishes nothing.

A cancelled live export that is still blocked after 5 seconds is the one case
where the process gives up. A watchdog claims the request, waits 250 ms for
already-committed reports, takes the process-wide publication gate, and calls
`_Exit(70)`. The gate stays held through report encoding and the synchronous
owner stdout write, so a file that has already become visible is always
reported. If publication already started, the gate keeps the watchdog out and
the file is allowed to finish. Open-but-unread stdout can therefore defer the
exit indefinitely.

A detected stdout write failure refuses new publication and exits 74 after at
most 500 ms of best-effort cleanup of active owners; staging may remain, pending
reports cannot be delivered, and already visible files stay. Threads are never
killed.

## 6. Quotas and bounds

| Bound | Value | Where |
|---|---|---|
| Request line | 4 MiB | `runtime_limits.h` |
| Pending input lines | 64 lines / 32 MiB | transport |
| In-flight tool calls | 64 | transport |
| Single response | 16 MiB | transport |
| Output queue | 48 MiB / 256 messages | transport |
| Open documents | 16 | `host_api.cpp` |
| Decoded score input | 8 MiB | engine `max_input_bytes` |
| Edit batch | 1,024 operations | registry |
| Export output | 1 GiB | `ExportLimits` |
| Export deadline | 120 s, advisory | `ExportLimits` |
| Error/report text | 16 KiB (UTF-8 safe) | export/transport |

The export budget is enforced while encoding and before publication; it is not a
process-memory bound. Audio encoding splits the budget in half between the
encoder's intermediate file and the staged copy, so the default maximum encoded
audio file is 512 MiB, plus up to 1 MiB of header-rewrite traffic. Query results
are bounded by the engine's `max_query_results` (4,096).

## 7. Performance

Accepted M4 evidence (Windows native and physical Linux `linux-host`, engine
`engrave-31`):

| Measurement | Windows | Linux |
|---|---|---|
| Render worst-row median / p95 | 8.348 / 9.534 ms (fixed replay 7.986 / 10.343) | 1.652 / 1.789 ms |
| Dense-score cancellation p95 / max | 8.872 / 27.053 ms | 11.270 / 11.312 ms |
| Warm WAV p95 | 270.094 ms in the accepted run | — |
| Golden (identical / allowed / different / missing / extra) | 307 / 83 / 0 / 0 / 0 | 265 / 125 / 0 / 0 / 0 |

Audio latency is accepted as an **unresolved variance limit**, not a stable
guarantee: full 1,000-call replays at the same binary produced one pass and
failing runs dominated by `engine_call` under host contention. Functional
parity, byte-identical audio and cancellation pass. Treat the warm p95 as an
observation, not a contract.

## 8. What this design does not do

- No concurrent editing of one document, and no multi-document transaction.
- No background jobs, streaming playback, or stateful mix sessions.
- No in-product sandbox; a crash is a session crash.
- No MuseScore version before 4.6, and no promise that every score can be
  written as 4.70. The 4.70 writer is fail-closed: unsupported features produce
  warnings or a refusal, never a silently degraded score.
- No computer use, screenshots or desktop MuseScore dependency.
