# Defects and known limitations

This is the current defect ledger for the engine fork and the host. It records
open issues, accepted limitations and a compact index of resolved ones. Full
investigations and per-phase evidence live in Git history (the curated docs
snapshot is commit `704295d`); `notes/` is ignored scratch space and is cited
only as provenance.

Evidence levels: **SOURCE-REVIEWED** (inspected in tracked source),
**EXECUTED** (compiled/run and observed), **REPORTED** (recorded in a prior
investigation).

## Open and carried

| ID | Issue | Status | Evidence | Impact |
|---|---|---|---|---|
| 001 | LeakSanitizer findings at `engrave-14` | Open | EXECUTED, SOURCE-REVIEWED | The long-lived host is not proven leak-clean across repeated document lifecycles. Address-only ASan runs pass; LSan findings remain documented, not fixed. |
| 003 | Subnormal reverb-tail float divergence | Carried, not adopted | SOURCE-REVIEWED | Linux reverb tails can be slower without FTZ/DAZ. Bit-exactness is preserved; no fix adopted. |
| 004 | Batch rollback may move layout | Accepted contract | SOURCE-REVIEWED, REPORTED | A failed batch reloads a snapshot (or undoes a command) when the unwind is not trusted, which can move elements. Documented, not promised bit-exact. |
| 013 | Pitch edit can leave a mismatched tie | Open | SOURCE-REVIEWED, EXECUTED | An exported mismatched tie may not survive MusicXML reimport; no diagnostic is emitted. |
| 016 | Writer numeric precision in save/reopen | Open | SOURCE-REVIEWED, EXECUTED | Serialized dimensions can lose precision and alter later output. |
| 018 | MIDI and workspace-estimate capture gaps | Partially resolved | SOURCE-REVIEWED, EXECUTED | Live score capture and position JSON are bounded; MIDI capture and the workspace estimate are not a hard memory bound. |
| 024 | 4.70 cannot represent part EIDs | Accepted format limitation | EXECUTED, SOURCE-REVIEWED | Shared-part downgrades keep numeric part IDs but cannot write part EIDs; representable element EIDs are preserved. |
| 025 | 4.70 refuses scores with generated accidentals | Open, bounded-contract gap | EXECUTED | Accidental conversion is not implemented. A score that needs generated accidentals is refused with `B470_UNSUPPORTED_FEATURE` rather than degraded. |
| 026 | Standalone MSCX drops style blocks and reflows on reopen | Open | EXECUTED | MSCX round trips can shift a segment by up to 25.26 pt and change 23,909 pixels; the same scores round-trip exactly as MSCZ. Page margins and default style blocks are not written. |
| 027 | Dynamic placement edit can be ignored | Open; unrelated to the upstream grouping change | EXECUTED | On Windows at `engrave-33`, a batch setting an imported dynamic's `VELOCITY` to 73 and `placement` to `above` reports success, but placement remains `below` immediately and after MSCX reopen. Repairing this existing editing behavior is outside the grouping adaptation. |
| 028 | Default-valued text-frame overrides may lose their override status | Open; outside the grouping adaptation | SOURCE-REVIEWED | The native writer omits unstyled properties that equal the current default. An explicit no-frame or zero-rounding override can reopen as styled and follow a later style change. This is a general serialization behavior present at `engrave-33`. |

## Accepted limitations (not defects to chase)

- **012 / 015 — snapshot rendering.** Historical A5 export jobs rendered MSCZ
  snapshots, which was not bit-identical to a direct render. M4 renders the live
  document, so this no longer affects exports; save/reopen layout differences
  remain and are why live export exists.
- **Audio latency variance.** M4 accepts warm WAV latency as an unresolved
  variance limit. Full replays produced passing and failing runs at the same
  binary under host contention. Functional parity, byte-identical audio and
  cancellation pass; the p95 is an observation, not a guarantee.
- **Filesystem corners.** Complex DACLs, SMB semantics, device namespaces and
  multi-file transactions are explicit non-goals; see [scope.md](scope.md).
- **No isolation mode.** A render/audio/writer crash ends the session. Use one
  disposable server per untrusted file operationally.
- **Complex MusicXML dynamics.** Exact round trips for arbitrary mixtures of
  conflicting dynamics, literal pseudo-markup and unsupported vendor glyphs are
  outside the compatibility target. Source review of candidate `8eb0c25a52`
  found that a container with `ff`, `pp`, textual `sf`, and `p` can change the
  resulting dynamic type when exported and reimported; this has not been
  executed. This is separate from the chosen initial grouping playback change.

## Resolved (index)

| ID | Resolution |
|---|---|
| 002 | Corrected the historical A4 "ASan clean" claim: it was address-only with `detect_leaks=0`, not leak-clean. |
| 005 | Host asset staging moved to the engine installer (`b53ff3b`), 345 files / 28,380,646 bytes verified. |
| 006 | Phase C accepted 2026-11-01 by Astra with no must-fix items. |
| 007 | Parked public API work adopted by M4 (positions and excerpt selection). |
| 008 | Encoder finalization reports codec failure at `engrave-16`. |
| 009 | Audio storage failures return public `ErrorCode::Io` at `engrave-16`. |
| 010 | Engine gate runs diagnostics, IO-boundary, installer and manifest checks (`1fb47a7`). |
| 011 | Host adapter ported to the C5–C8 engine API; built and smoke-tested on both hosts. |
| 014 | Live rendering keeps the source version for a clean score; save/reopen reports the writer version. |
| 017 | Successful-response schemas no longer inherit request bounds. |
| 019 | Audio cache and staged copy each receive half the export budget (`engrave-17`). |
| 020 | Snapshot pins take precedence over older direct-engine allowances. |
| 021 | Worker cache monitor retired with the worker path; engine/sink quotas remain. |
| 022 | Host duration projection removed; repeat-expanded projection is not used. |
| 023 | Stave-sharing labels survive 5.00 ingest/export at `engrave-21`. |

## Reporting a new issue

A reviewer may report anything; the orchestrator triages it against
[scope.md](scope.md). Ordinary single-user local export regressions are fixed.
Out-of-scope findings are recorded as non-goals, not reopened as defects. New
entries need an ID, the observed behavior, the evidence level, and the affected
pin or commit.
