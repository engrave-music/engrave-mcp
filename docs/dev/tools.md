# Tool contract

The server exposes twelve tools. This page is the human-readable contract; the
generated [engrave_mcp/tools.json](../../engrave_mcp/tools.json) is the machine-readable
one and is checked by the `tool_schema` lane.

## Envelope and revisions

Every result is a JSON object in `structuredContent`:

```json
{"ok": true,  "...": "tool-specific fields"}
{"ok": false, "error": {"code": "RevisionConflict", "message": "...", "...": "optional detail"}}
```

Failures also set the MCP `isError` flag. `error` may carry `document`,
`current_revision`, `revision`, `op_index`, `applied`, `created`, `removed`,
`replaced`, `poisoned`, `published_artifacts` or `export_warnings`.

Document revisions are decimal strings. A newly opened or created document
starts at `"1"`. A batch that changes the score increments it (including a failed
batch that wrote before failing), and `restore_checkpoint` increments it; an
empty or no-op successful batch or preflight failure leaves it unchanged.
`expected_revision`, when supplied, is checked before the operation and a
mismatch returns `RevisionConflict` with `current_revision`. Revisions are per
document.

## `score_open`

Opens a score from `path`, or from base64 `data`.

| Field           | Type    | Notes                                                          |
| --------------- | ------- | -------------------------------------------------------------- |
| `path`          | string  | Mutually exclusive with `data` (oneOf).                        |
| `data`          | string  | Base64 bytes; decoded limit 8 MiB.                             |
| `format`        | enum    | `mscz`, `mscx`, `musicxml`, `mxl`, `midi`, `guitarpro`, `mnx`. |
| `filename_hint` | string  | Helps format detection for `data`.                             |
| `strict_ids`    | boolean | Reject input with unusable element IDs instead of repairing.   |

Returns `{ok, document, revision, diagnostics[]}`. Pre-4.6 MuseScore files are
rejected with `UnsupportedFormat`.

## `score_create`

Creates a score from `template_id` (oneOf) or `template_path` (oneOf), with
optional `title`, `subtitle`, `composer`, `lyricist`. Returns
`{ok, document, revision, diagnostics[]}`. Template IDs are listed by
`runtime_info` in `templates`.

## `score_close`

`document` (required). Returns `{ok: true}`.

## `score_outline`

`document` (required), optional `expected_revision`, optional
`score: {excerpt}` to outline one excerpt. Returns
`{ok, revision, outline}`, where `outline` contains `title`, `subtitle`,
`composer`, `poet`, `duration`, `key`, `timesig`, `pages`, `measure_count`,
`mscore_version`, `file_version`, `parts[]`, `measures[]`, `signatures[]` and
`excerpts[]`.

## `score_query`

Three modes, selected by `mode`:

| Mode           | Required                                       | Returns                                              |
| -------------- | ---------------------------------------------- | ---------------------------------------------------- |
| `elements`     | `document`, `mode`, optional `query`           | `{elements[], total, next_cursor, revision, mode}`   |
| `properties`   | `document`, `mode`, `target`, optional `props` | `{properties[], revision, mode}`                     |
| `audio_tracks` | `document`, `mode`, optional `score`           | `{audio_tracks: {master, tracks[]}, revision, mode}` |

`query` (elements) accepts `score: {excerpt}`, `types[]`, `staves[]`,
`voices[]`, `first_measure`, `measure_count`, `from_measure`, `to_measure`
(EIDs), `from_tick`/`to_tick` (`{num, den}`), `props[]`, `layout_boxes`,
`limit`, `cursor`. `next_cursor` is an integer or `null`.

`target` (properties) is an EID (`{"kind":"id","id":...}`) or an address
(`{"kind":"address","address":{...}}`).

## `score_describe`

`scope` selects the catalogue:

- `types` → `{types[]}`
- `type` (requires `type`) → `{type: {type, props[]}}`
- `styles` → `{styles[]}`

## `score_edit`

`document` (required), `ops` (required, max 1,024), optional
`expected_revision`. Returns `{ok, revision, applied, ops[], created[], removed[],
replaced[], diagnostics[]}` where `applied` is the count of executed operations.

Batches use **prefix-commit** semantics: each operation is all-or-nothing on its
own, while the batch as a whole has no implicit rollback. The host decodes and
preflights the batch before writing, then applies operations `0..n-1` in order.
If decoding or preflight rejects operation `k`, nothing changes and the error
reports `applied: 0`, `op_index: k`, and the rejection reason (`code` /
`message`). If operation `k` is rejected during execution, operations `0..k-1`
remain applied, operation `k` has no effect, the batch stops, the document is
not poisoned, and the error reports `applied: k`, `op_index: k`, the applied
prefix's `revision`, `created`, `removed`, `replaced`, and the rejection
reason.

`poisoned: true` (with a hint to call `restore_checkpoint` or reopen the file)
occurs only if an unexpected engine failure happens after an operation has
started writing. On a poisoned document, reads, queries, and non-native exports
still work, while write batches, `checkpoint`, and native (`mscx`/`mscz`) saves
are refused with `Poisoned` until `restore_checkpoint` succeeds or the document
is closed and reopened. Use `checkpoint` before a risky batch and
`restore_checkpoint` on failure when all-or-nothing rollback is needed.

There are 23 operations:

| `op`                | Required fields                               |
| ------------------- | --------------------------------------------- |
| `set_property`      | `target`, `property`, `value`                 |
| `reset_property`    | `target`, `property`                          |
| `set_style`         | `score`, `style`, `value`                     |
| `enter_notes`       | `at`, `duration`, `pitches`                   |
| `set_pitch`         | `note`, `pitch` (optional `tpc`)              |
| `set_duration`      | `chord_rest`, `duration`                      |
| `remove`            | `target`                                      |
| `insert_measures`   | `count` (optional `before`, `kind`)           |
| `remove_measures`   | `first`, `last`                               |
| `set_time_sig`      | `measure`, `sig`                              |
| `set_key_sig`       | `measure`, `key` (optional `staff`)           |
| `insert_clef`       | `at`, `clef`                                  |
| `set_layout_break`  | `measure`, `kind`, `on`                       |
| `add_part`          | `instrument_id` (optional `before`)           |
| `remove_part`       | `part`                                        |
| `add_staff`         | `part`                                        |
| `remove_staff`      | `staff`                                       |
| `generate_excerpts` | `parts`                                       |
| `add_spanner`       | `start`, `end`, `type` (optional `props`)     |
| `add_tie`           | `note`                                        |
| `add_tuplet`        | `first`, `last` (optional `actual`, `normal`) |
| `add_text`          | `at`, `style`, `text`                         |
| `add`               | `anchor`, `type` (optional `props`)           |

Targets are EIDs, addresses, or a score selection; see the generated schema for
the exact shapes.

## `score_export`

Exports from the live document. `kind` selects the export family; every variant
takes `document`, `kind`, `destination`, `options`, and optional
`expected_revision`.

| `kind`      | Destination                           | `options.format`                          | Other options                                                                                                                                         |
| ----------- | ------------------------------------- | ----------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `score`     | `file`                                | `mscz`, `mscx`, `musicxml`, `mxl`, `midi` | `native_version` (`5.00` default, `4.70`), `score.excerpt`, and for MIDI only `midi_expand_repeats`, `midi_rpns`                                      |
| `render`    | `file` (PDF) or `directory` (SVG/PNG) | `pdf`, `png`, `svg`                       | `first_page`, `last_page`, `background`, `score.excerpt`; `dpi` (36–1200) and `transparent` are PNG-only                                              |
| `audio`     | `file`                                | `wav`, `flac`, `opus`, `mp3`, `aac`       | `sample_rate`, `bit_depth`, `bitrate_kbps`, `reverb`, `reverb_send`, `master_volume_db`, `master_balance`, `tracks[]`, `use_repeats`, `score.excerpt` |
| `positions` | `file`                                | —                                         | `position_kind`: `measure` or `segment`, `score.excerpt`                                                                                              |

`destination` is `{"kind":"file","path":...}` or
`{"kind":"directory","path":...}`; directory destinations produce
`page-0001.svg` / `page-0001.png` names. Paths are limited to 4,096 bytes.

### Destination syntax

The host validates the requested path and the resolved existing prefix before
creating anything. Supported syntax is ordinary drive/UNC and POSIX paths,
including non-ASCII UTF-8.

| Condition                                                                                                                                                                                                      | Outcome                                                                                    |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| Missing parent directory                                                                                                                                                                                       | Created recursively, then published.                                                       |
| Empty/structurally invalid path, non-directory component, existing wrong destination type                                                                                                                      | `InvalidArgument`, naming the destination.                                                 |
| Invalid UTF-8 or an unencodable resolved path                                                                                                                                                                  | `InvalidArgument`, before creating anything.                                               |
| Windows invalid characters (`< > : " \| ? *`, controls), reserved devices (`CON`, `PRN`, `AUX`, `NUL`, `COM1–9`, `LPT1–9`, including extensions and superscript forms), trailing dots/spaces, empty components | `InvalidArgument`, before creating anything; drive/root syntax and `.`/`..` are preserved. |
| Windows device namespaces (`\\.\`, `\\?\`) and incomplete UNC roots                                                                                                                                            | `InvalidArgument`, before any metadata probe.                                              |
| POSIX filenames                                                                                                                                                                                                | No added restrictions beyond NUL and `/`.                                                  |
| Permission denied                                                                                                                                                                                              | `Io`, retaining the filesystem diagnostic.                                                 |
| Other filesystem failure (I/O, read-only, symlink loop, name too long, disk full)                                                                                                                              | `Io`, retaining the filesystem diagnostic.                                                 |

Complex DACLs, SMB authentication, device namespaces and multi-file
transactions are non-goals; see [scope.md](scope.md).

Success returns `{ok, source_revision, artifacts[], export_warnings[]}`, where
each artifact is `{path, mime, size}` and `size` is a decimal string. A failure
during multi-file publication returns `Io` with `published_artifacts` listing the
files already visible. `export_warnings` carries structured 4.70 degradation
warnings (`B470_*`).

`native_version` accepts `"5.00"` (default) and `"4.70"`. Any other nonempty
bounded string reaches the engine and fails with `Unsupported` plus
`export_warnings`; a refusal publishes nothing.

`options.score` selects the master (`{}`) or an excerpt
(`{"excerpt":"<element-id>"}`). PDF, score, audio and position exports use one
file; PNG/SVG require a directory and write one file per page.

### Positions artifact

A `positions` export writes UTF-8 JSON derived from `engrave::Positions`:

```json
{
  "page_size": { "width": 1000.0, "height": 1500.0 },
  "elements": [
    { "id": 0, "x": 10.0, "y": 20.0, "sx": 100.0, "sy": 40.0, "page": 0 }
  ],
  "events": [{ "elid": 0, "position": 0 }]
}
```

Coordinates and sizes are page-local 1200-DPI layout units; pages are
zero-based. For PNG, multiply by `dpi / 1200`. Element `id` and event `elid` are
local to the artifact, not persistent EIDs. Event `position` is repeat-expanded
playback time in milliseconds, so a repeated passage can reference an element
more than once. Position reads may lay out the score but do not change the
revision.

## `runtime_info`

| `action`         | Fields    | Effect                                                   |
| ---------------- | --------- | -------------------------------------------------------- |
| `get` (default)  | —         | Return capabilities, templates, fonts and limits.        |
| `add_font`       | `path`    | Register a font and invalidate layout.                   |
| `set_soundfonts` | `files[]` | Replace the SoundFont set; an empty list disables audio. |

`get` returns `{ok, tool_revision:"2", capabilities, templates[], fonts[],
limits}`. `capabilities` reports `engine_version`, `import_formats`,
`export_formats`, `render_formats`, `audio_formats`, `audio_sample_rate` and
`soundfonts`. `limits` reports `input_bytes` (8 MiB), `base64_chars` (11,184,812),
`batch_ops` (1,024), `query_results` (4,096), `output_bytes` (1 GiB as a
string), `deadline_ms` (120,000), `cancellation` (`cooperative`) and
`transport_request_bytes` (4 MiB).

## `checkpoint`, `restore_checkpoint`, `drop_checkpoint`

Named in-memory snapshots sharing a 64 MiB per-document retained-data budget.
Checkpoints are in memory only and are the only way to undo edits apart from
reopening the file.

| Tool                 | Required           | Optional            | Returns          | Effect                                                                                                                     |
| -------------------- | ------------------ | ------------------- | ---------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `checkpoint`         | `document`, `name` | `expected_revision` | `{ok, revision}` | Save (or replace) a named in-memory checkpoint; replacing keeps the prior copy on budget refusal; refused when `Poisoned`. |
| `restore_checkpoint` | `document`, `name` | `expected_revision` | `{ok, revision}` | Restore the named checkpoint, preserving public EIDs, advancing `revision`, dirtying layout, and clearing `Poisoned`.      |
| `drop_checkpoint`    | `document`, `name` | `expected_revision` | `{ok, revision}` | Delete the named checkpoint and release its budget share.                                                                  |

## Error codes

Cancellation has two layers. A request cancelled through
`notifications/cancelled` is normally answered with JSON-RPC error `-32800` and
no `structuredContent`; the transport owns that response. The structured
`Cancelled` code below is used when a tool reports a cancellation inside its own
result (for example an engine callback abort). Do not treat the two as
interchangeable.

| Code                | Meaning                                                                                                                                                                                                          |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `InvalidArgument`   | Malformed parameters or destination syntax.                                                                                                                                                                      |
| `InvalidInput`      | Corrupt or unreadable score/archive content.                                                                                                                                                                     |
| `UnsupportedFormat` | Unknown format, or a MuseScore file older than 4.6.                                                                                                                                                              |
| `Unsupported`       | Valid request the engine cannot perform.                                                                                                                                                                         |
| `NotFound`          | Unknown document handle, element, or path.                                                                                                                                                                       |
| `StaleHandle`       | An ID/address no longer resolves.                                                                                                                                                                                |
| `RevisionConflict`  | `expected_revision` does not match.                                                                                                                                                                              |
| `ResourceLimit`     | A documented byte, count or result limit was exceeded.                                                                                                                                                           |
| `EditRejected`      | An edit violates notation or layout constraints.                                                                                                                                                                 |
| `Poisoned`          | An unexpected engine failure after writes started poisoned the document; reads, queries and non-native exports still work, while edits, `checkpoint` and native saves require `restore_checkpoint` or reopening. |
| `EngineFailure`     | Engine-level failure with no more specific code.                                                                                                                                                                 |
| `Io`                | Filesystem failure; check `published_artifacts`.                                                                                                                                                                 |
| `Cancelled`         | The client cancelled before publication.                                                                                                                                                                         |
| `DeadlineExceeded`  | The advisory export deadline expired.                                                                                                                                                                            |
| `WorkerFailed`      | Retained in the advertised enum for wire compatibility; the live-only host does not produce it.                                                                                                                  |
