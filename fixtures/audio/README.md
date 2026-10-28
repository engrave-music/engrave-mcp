# Audio Mix Fixtures

## `mix.mscz`

`mix.mscz` is an MSCZ fixture containing an embedded MuseScore 4 `audiosettings.json` file configuring default mixer settings for a score.

### Generation Method

1. **Base Score**: Created from `fixtures/golden/created.json` ("Native Evening / 原生晚曲"), which contains three instruments:
   - Part 1: Violin (midiProgram 41, trackId 0, instrumentId `violin`)
   - Part 2: Piano (midiProgram 1, trackId 1, instrumentId `piano`)
   - Part 3: Cello (midiProgram 43, trackId 2, instrumentId `baryton`)
2. **MSCZ Export**: Exported as an uncompressed/compressed MSCZ archive via native MCP `score_create` and `score_export` (`format: "mscz"`).
3. **Audio Settings Injection**: Added `audiosettings.json` at the root of the ZIP archive following the MuseScore 4 schema used by `ProjectAudioSettings` (`vendor/musescore/src/project/internal/projectaudiosettings.cpp`) and `NotationSoloMuteState` (`vendor/musescore/src/notation/internal/notationsolomutestate.cpp`):
   - Root `master` object containing `volumeDb: 0.0` and `balance: 0.0`.
   - `tracks` array containing entries for each part/instrument (`partId`, `instrumentId`, `out: { volumeDb, balance }`, and `soloMuteState: { mute, solo }`).
   - Cello (`partId: "3"`, `instrumentId: "baryton"`) is set to `soloMuteState: { "mute": true, "solo": false }`.

### Test Purpose

This fixture verifies that:
- `score_open` reads the desktop mixer settings (`audiosettings.json`) from the MSCZ container.
- `score_audio_tracks` and score metadata (`score_get`, `metadata.json`) report default mixer properties per track (`volumeDb`, `balance`, `solo`, `muted`, `forceMute`), showing Cello as `muted: true`.
- Plain audio export (`score_export` format `wav`) renders the score with Cello silent by default.
- Supplying an explicit export option `mix` overriding `muted: false` for Cello brings the Cello track back in the rendered audio.
