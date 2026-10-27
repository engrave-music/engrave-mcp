# Golden fixtures

Inputs for `scripts/golden.py` and `scripts/test_roundtrip.py`. Copied verbatim
from MuseScore test data (GPL-3.0, same license as the engine) so the goldens do
not depend on the vendored source path.

| File | Source |
| --- | --- |
| `created.json` | `score_create` input (same score as `scripts/mcp_smoke.py`) |
| `repeat_with_2_voltas.mscx` | webmscore-4.7.5-1 `src/engraving/tests/playback/playbackmodel_data/repeat_with_2_voltas/repeat_with_2_voltas.mscx` |
| `repeat70.mscx` | MuseScore v4.7.6 `src/engraving/tests/repeat_data/repeat70.mscx` (open volta after exhausted repeats) |
| `pedal.mscx` | webmscore-4.7.5-1 `src/importexport/mei/tests/data/pedal-01.mscx`, first Pedal given a user `placement`/`offset` override (v4.7.6 writes these, PR #34917) |
| `hairpins_and_repeats.mscx` | webmscore-4.7.5-1 `src/engraving/tests/playback/playbackcontext_data/dynamics/hairpins_and_repeats.mscx` |
| `linked_parts.mscx` | webmscore-4.7.5-1 `src/engraving/tests/parts_data/linked-ties-parts.mscx` (excerpts) |
| `lyrics.mscx` | webmscore-4.7.5-1 `vtest/scores/lyrics-1.mscx` |
| `slurs.mscz` | webmscore-4.7.5-1 `vtest/scores/slurs-11.mscz` |
| `drums_import.musicxml` | webmscore-4.7.5-1 `src/importexport/musicxml/tests/data/testImportDrums.xml` |
| `perc_stem_dir.musicxml` | MuseScore v4.7.6 `src/importexport/musicxml/tests/data/testInferredPercStemDir.xml` |
| `bend.gp` | webmscore-4.7.5-1 `src/importexport/guitarpro/tests/data/bend.gp` |
| `bends.mid` | webmscore-4.7.5-1 `src/importexport/midi/tests/midiexport_data/testBends1-ref.mid` |

`../evening.musicxml` is also part of the golden set.
