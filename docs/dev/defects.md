# Defects and known limitations

This document records active defects and known limitations for the engine and host.

## Open and carried issues

| ID  | Issue                                                         | Status            | Evidence                  | Details                                                                                                                                                                                                                                                                                 |
| --- | ------------------------------------------------------------- | ----------------- | ------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 016 | Writer numeric precision in save/reopen                       | Open limitation   | SOURCE-REVIEWED, EXECUTED | Serialized floating-point layout dimensions (`precision = 6`) can lose precision across export/reopen cycles and alter visual placement; live exports are unaffected, and enabling shortest-round-trip doubles globally churns all 25 allow-listed MSCX goldens.                        |
| 018 | MIDI and workspace-estimate bounds                            | Partially bounded | SOURCE-REVIEWED, EXECUTED | Score capture, visual rendering (`RenderOptions::max_bytes`), audio rendering (`AudioOptions::max_bytes`/`max_write_bytes`), and position JSON are bounded by byte limits; MIDI export and workspace estimation do not establish a hard process memory bound.                           |
| 024 | 4.70 cannot represent part EIDs                               | Format limitation | EXECUTED, SOURCE-REVIEWED | Shared-part downgrades preserve numeric part IDs but cannot write part element IDs (EIDs); representable element EIDs are preserved.                                                                                                                                                    |
| 028 | Default-valued text-frame overrides                           | Open limitation   | EXECUTED, SOURCE-REVIEWED | The score writer omits unstyled `TextBase` properties matching current defaults, so an explicit no-frame or zero-rounding override can reopen as styled. Probing `force = true` churned 5 golden fixtures; kept as a documented limitation.                                             |
| 030 | Windows MAX_PATH length on long output roots                  | Open limitation   | EXECUTED                  | Destination paths and private staging folders exceeding 260 characters on Windows can trigger filesystem IO errors; short temporary directory roots are required (`\\?\` extended paths are an explicit non-goal in [scope.md](scope.md)).                                              |
| 031 | MusicXML `<harmony>` cannot preserve `play=false`             | Format limitation | SOURCE-REVIEWED, EXECUTED | MusicXML 4.0 `<harmony>` (`musicxml.xsd`) has no playback-mute attribute, so chord symbols authored with `Harmony.play = false` reopen from MusicXML as playable on the chord-symbol MIDI channel (audio export already mutes chord-symbol pseudo-tracks by default).                   |
| 032 | Legacy `Note::play_events` and `<Bend>` absent from MPE audio | Open limitation   | SOURCE-REVIEWED, EXECUTED | MuseScore 4+/5 MPE playback (`PlaybackEventsRenderer`) renders modern `GuitarBend` (`ElementType::GUITAR_BEND`) rather than legacy MuseScore 3 `<Events><Event>` (`Note::play_events`) or legacy `<Bend>` curves, whereas legacy MIDI export (`compatmidirender.cpp`) still reads both. |

## Current limitations

- **Layout reflow across file round trips**: Live in-memory exports preserve exact visual placement. Saving to disk and reopening can reflow elements slightly due to XML serialization and margin defaults.
- **Audio synthesis latency and cancellation variance**: Audio synthesis and cancellation cleanup latency vary with score density, SoundFont complexity, system load, and CPU scheduling. Documented latency targets are idle-machine sampled observations rather than hard real-time guarantees.
- **Windows path length limitations**: Paths approaching or exceeding Windows `MAX_PATH` require short output directories or short temporary roots to prevent staging and atomic move failures.
- **Filesystem scope**: Complex DACLs, SMB authentication, and multi-file transaction guarantees are explicit non-goals (see [scope.md](scope.md)).
- **No in-process isolation**: Engine faults in rendering or synthesis terminate the server process. Operational isolation requires running disposable server processes per file or task.
- **Complex MusicXML dynamics round trips**: Combinations of conflicting dynamic markings and pseudo-markup may be normalized to standardized dynamic types upon import and export.

## Reporting a new issue

Every defect entry must state its evidence level: `EXECUTED` (verified by a running test or probe on native Windows or Linux) versus `SOURCE-REVIEWED` (identified by inspection of code and specifications). New entries require a unique identifier, an observed reproduction, the evidence level, and triage against [scope.md](scope.md). Findings outside the defined single-user local filesystem scope are documented as non-goals rather than tracked defects.
