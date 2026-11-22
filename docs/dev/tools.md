# MCP score tools

The host negotiates protocol versions 2024-11-05, 2025-03-26, 2025-06-18 and 2025-11-25. An unsupported version receives 2025-11-25. After initialization, `tools/list` publishes the active input schemas and annotations. Tool revision 3 returns a structured object with `ok` and a text representation. Errors set `isError` and carry `error.code` and `error.message`. Reads of notation have a bar-aligned text representation.

## Documents

| Tool            | Arguments and result                                                                                                                                                                 |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `score_create`  | Supply a template ID or local template path. Omit both to use `Treble_Clef`. Optional metadata fields are `title`, `subtitle`, `composer` and `lyricist`.                            |
| `score_open`    | Supply a local `path` or base64 `data`, with optional `format`, `filename_hint` and `strict_ids`. The decoded input limit is 8 MiB. Stdio limits the complete request line to 4 MiB. |
| `score_outline` | Read metadata, parts, staff IDs, measures, signatures and excerpts. Page counts can be null until the score has been laid out.                                                       |
| `score_close`   | Release the document and its named checkpoints.                                                                                                                                      |

Create and open return `document`, `revision`, `outline` and `diagnostics`. Revisions are canonical unsigned decimal strings. Pass the returned string as `expected_revision` to guard a subsequent operation. Element IDs are persistent strings scoped to the document. Supply an `excerpt` EID to select an excerpt on supported tools. Omitting it selects the master.

## Phrase entry and readback

`score_write_phrase` takes a document and a list of lanes. Each lane starts at a 1-based measure, staff and voice. Staff and voice default to 1. The offset defaults to `"0"` and uses an exact whole-note fraction measured from the beginning of that bar. Concert pitch is the default. Set `pitch_mode` to `written` to enter transposed notation.

The server starts in JSON mode. Select the other prototype with `--phrase-syntax dsl` or `ENGRAVE_MCP_PHRASE_SYNTAX=dsl`. A command-line choice takes precedence. Each server advertises only the selected lane syntax.

```json
{
  "document": "doc-session-1",
  "expected_revision": "1",
  "lanes": [
    {
      "measure": 1,
      "events": [
        { "pitches": ["Eb4", "G4"], "duration": "1/2", "tie": ["Eb4"] },
        { "pitches": ["Eb4"] },
        { "pitches": [] },
        { "bar": true }
      ]
    }
  ]
}
```

In DSL mode the same lane has `"content": "[Eb4~ G4]:h Eb4:q r:q |"`. A checked barline must coincide with a real measure boundary, including pickups. A quarter is the default duration. Duration suffixes accept an exact fraction or `w h q e s 32 64`, with up to two augmentation dots. Chords use brackets. Select individual ties with `~` inside the chord, or append it to the chord to tie every member. Drum keys use `d36` or `[d36 d42]`. JSON uses an integer `drums` array. Empty pitch and drum arrays enter rests.

For a triplet, use `3:2(A4:e G4:e F4:e)` or an object with `tuplet: "3:2"` and an `events` array. Tuplet members may be chords or rests. Before-graces use `acciac`, `appog`, `grace4`, `grace16` and `grace32`. After-graces use `after8`, `after16` and `after32`. Each wraps a note or chord, as in `acciac([D5 F#5]:e) E5:q after16(D5:s)`. The structured spelling uses ordered `grace_before` and `grace_after` arrays with `kind`, `duration` and `pitches`.

`score_read_music` starts at `measure` (default 1) and reads `count` bars (default up to 4, maximum 128). Restrict the window with 1-based `staves` and `voices` arrays. Set `include_ids` to address elements in later edits. Structured results give the staff contexts, measure-local events and durations, concert and written spellings, drum keys and kit names, ties, graces, tuplet groups, repeat barlines, spanners and voltas. Written meters retain their spelling, including `6/8`.

## Edit, query and checkpoints

`score_edit` applies up to 1024 operations in order. Atomic mode is the default. With `atomic: false`, an execution rejection can leave an applied prefix. The error reports the failing operation index, applied count and revision when available. Set `detail: true` for per-operation created-element slots. Use a bare EID for a persisted target. Within a batch, `{"from_op":0,"item":1}` refers to item 1 of operation 0. Both indices are zero-based.

Use `runtime_info` with `{"scope":"ops","op":"enter_notes"}` for the fields and example for one operation. The `at` position accepts `{"measure":1,"staff":1,"voice":1,"offset":"3/8"}`. The runtime catalogue supplies property and style value kinds. Values use JSON booleans, numbers, strings, integer lists and named objects such as `{"x":1,"y":2}` or `{"r":0,"g":0,"b":255,"a":255}`.

`score_query` accepts four modes. `elements` takes optional `query` filters and pagination. `properties` takes a `target` and optional list of `props`. `page_count` prepares layout. `audio_tracks` returns the selected score's playback tracks and default mix.

`checkpoint`, `restore_checkpoint` and `drop_checkpoint` each take a document and name. Checkpoints share a 64 MiB in-memory budget. Restoring a checkpoint advances the revision and preserves public IDs. A document that reports a poisoned edit can be recovered by restoring a checkpoint or reopening the file.

## Export and runtime

`score_export` takes a document, format and local path. PNG and SVG use a directory containing one file per page. Other formats take a file path. PNG defaults to an opaque white background. Set `options.background` to false and `options.transparent` to true for transparency. Use `first_page`, `last_page` and PNG `dpi` in `options` to control rendering.

Available formats are `mscz`, `mscx`, `musicxml`, `mxl`, `midi`, `pdf`, `png`, `svg`, `wav`, `flac`, `opus`, `mp3`, `aac`, `measure_positions` and `segment_positions`. Position exports contain page-local 1200-DPI geometry and repeat-expanded playback times in milliseconds. Success reports the source revision, published artifacts with integer byte sizes and any engine warnings. Publication failures identify files already visible.

Native exports use version 5.00 by default. Set `options.native_version` to `"4.70"` for compatibility export. Inspect degradation warnings and refusals before distributing the result. [Engine compatibility details](../../vendor/engrave/docs/write470.md)

`runtime_info` returns capabilities, limits and tool revision. Select `templates`, `instruments`, `types`, `type`, `styles` or `ops` with `scope`. Instrument lookup accepts a substring `query`. For a single type use its `type` name, or use `op` to look up a single operation. `runtime_configure` accepts an `add_font` path and an ordered `soundfonts` list. An empty list clears the SoundFont selection.
