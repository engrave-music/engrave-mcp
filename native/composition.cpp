#include "composition.h"

#include <cstdint>
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace music {

namespace {

std::string xml_escape(const std::string& str) {
    std::string out;
    out.reserve(str.size());
    for (char c : str) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

void validate_xml_string(const std::string& str, const char* field_name) {
    if (str.find("\xEF\xBF\xBE") != std::string::npos || str.find("\xEF\xBF\xBF") != std::string::npos)
        throw std::invalid_argument(std::string(field_name) + " contains a forbidden XML character");
    if (str.size() > 256) {
        throw std::invalid_argument(std::string(field_name) + " exceeds maximum length of 256 bytes");
    }
    for (unsigned char c : str) {
        if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) {
            throw std::invalid_argument(std::string(field_name) + " contains invalid XML 1.0 control byte");
        }
    }
}

const char* ticks_to_type(int ticks) {
    switch (ticks) {
        case 120:  return "16th";
        case 240:  return "eighth";
        case 480:  return "quarter";
        case 960:  return "half";
        case 1920: return "whole";
        default:   return "";
    }
}

struct PitchInfo {
    char step;
    int alter;
};

static const PitchInfo CHROMATIC_SCALE[12] = {
    {'C', 0}, // 0: C
    {'C', 1}, // 1: C#
    {'D', 0}, // 2: D
    {'D', 1}, // 3: D#
    {'E', 0}, // 4: E
    {'F', 0}, // 5: F
    {'F', 1}, // 6: F#
    {'G', 0}, // 7: G
    {'G', 1}, // 8: G#
    {'A', 0}, // 9: A
    {'A', 1}, // 10: A#
    {'B', 0}  // 11: B
};

struct NoteEntry {
    std::vector<int> pitches;
    int ticks;
};

struct MeasureData {
    std::vector<NoteEntry> notes;
};

struct PartData {
    std::string name;
    int midiProgram;
    std::vector<MeasureData> measures;
};

} // namespace

std::string composition_xml(const nlohmann::json& score) {
    if (!score.is_object()) {
        throw std::invalid_argument("Score must be a JSON object");
    }

    for (auto it = score.begin(); it != score.end(); ++it) {
        const std::string& key = it.key();
        if (key != "title" && key != "tempo" && key != "parts") {
            throw std::invalid_argument("Unknown key at root level: " + key);
        }
    }

    std::string title = "Untitled";
    if (score.contains("title")) {
        if (!score["title"].is_string()) {
            throw std::invalid_argument("Title must be a string");
        }
        title = score["title"].get<std::string>();
    }
    validate_xml_string(title, "Title");

    int tempo = 100;
    if (score.contains("tempo")) {
        const auto& tempo_val = score["tempo"];
        if (!tempo_val.is_number_integer() || tempo_val.is_boolean()) {
            throw std::invalid_argument("Tempo must be an integer");
        }
        int64_t t = tempo_val.get<int64_t>();
        if (t < 20 || t > 300) {
            throw std::invalid_argument("Tempo must be between 20 and 300");
        }
        tempo = static_cast<int>(t);
    }

    if (!score.contains("parts") || !score["parts"].is_array()) {
        throw std::invalid_argument("Score must contain a 'parts' array");
    }

    const auto& parts_json = score["parts"];
    if (parts_json.empty() || parts_json.size() > 15) {
        throw std::invalid_argument("Parts count must be between 1 and 15 (MIDI channel 10 is reserved)");
    }

    std::vector<PartData> validated_parts;
    validated_parts.reserve(parts_json.size());
    std::optional<size_t> expected_measure_count;

    for (const auto& part_json : parts_json) {
        if (!part_json.is_object()) {
            throw std::invalid_argument("Part must be a JSON object");
        }

        for (auto it = part_json.begin(); it != part_json.end(); ++it) {
            const std::string& key = it.key();
            if (key != "name" && key != "midiProgram" && key != "measures") {
                throw std::invalid_argument("Unknown key in part: " + key);
            }
        }

        if (!part_json.contains("name") || !part_json["name"].is_string()) {
            throw std::invalid_argument("Part must contain a string 'name'");
        }
        std::string part_name = part_json["name"].get<std::string>();
        validate_xml_string(part_name, "Part name");

        if (!part_json.contains("midiProgram")) {
            throw std::invalid_argument("Part must contain 'midiProgram'");
        }
        const auto& midi_prog_val = part_json["midiProgram"];
        if (!midi_prog_val.is_number_integer() || midi_prog_val.is_boolean()) {
            throw std::invalid_argument("Part midiProgram must be an integer");
        }
        int64_t prog = midi_prog_val.get<int64_t>();
        if (prog < 1 || prog > 128) {
            throw std::invalid_argument("Part midiProgram must be between 1 and 128");
        }

        if (!part_json.contains("measures") || !part_json["measures"].is_array()) {
            throw std::invalid_argument("Part must contain a 'measures' array");
        }
        const auto& measures_json = part_json["measures"];
        if (measures_json.empty() || measures_json.size() > 128) {
            throw std::invalid_argument("Measures count must be between 1 and 128");
        }

        if (!expected_measure_count.has_value()) {
            expected_measure_count = measures_json.size();
        } else if (measures_json.size() != *expected_measure_count) {
            throw std::invalid_argument("All parts must have the same measure count");
        }

        PartData part_data;
        part_data.name = std::move(part_name);
        part_data.midiProgram = static_cast<int>(prog);
        part_data.measures.reserve(measures_json.size());

        for (const auto& measure_json : measures_json) {
            if (!measure_json.is_object()) {
                throw std::invalid_argument("Measure must be a JSON object");
            }

            for (auto it = measure_json.begin(); it != measure_json.end(); ++it) {
                if (it.key() != "notes") {
                    throw std::invalid_argument("Unknown key in measure: " + it.key());
                }
            }

            if (!measure_json.contains("notes") || !measure_json["notes"].is_array()) {
                throw std::invalid_argument("Measure must contain a 'notes' array");
            }
            const auto& notes_json = measure_json["notes"];
            if (notes_json.size() > 16) {
                throw std::invalid_argument("Notes count exceeds maximum of 16 per measure");
            }

            MeasureData meas_data;
            meas_data.notes.reserve(notes_json.size());
            int total_measure_ticks = 0;

            for (const auto& note_json : notes_json) {
                if (!note_json.is_object()) {
                    throw std::invalid_argument("Note must be a JSON object");
                }

                for (auto it = note_json.begin(); it != note_json.end(); ++it) {
                    const std::string& key = it.key();
                    if (key != "pitches" && key != "ticks") {
                        throw std::invalid_argument("Unknown key in note: " + key);
                    }
                }

                if (!note_json.contains("ticks")) {
                    throw std::invalid_argument("Note must contain 'ticks'");
                }
                const auto& ticks_val = note_json["ticks"];
                if (!ticks_val.is_number_integer() || ticks_val.is_boolean()) {
                    throw std::invalid_argument("Note ticks must be an integer");
                }
                int64_t ticks = ticks_val.get<int64_t>();
                if (ticks != 120 && ticks != 240 && ticks != 480 && ticks != 960 && ticks != 1920) {
                    throw std::invalid_argument("Note ticks must be one of 120, 240, 480, 960, 1920");
                }
                total_measure_ticks += static_cast<int>(ticks);

                if (!note_json.contains("pitches") || !note_json["pitches"].is_array()) {
                    throw std::invalid_argument("Note must contain a 'pitches' array");
                }
                const auto& pitches_json = note_json["pitches"];
                if (pitches_json.size() > 8) {
                    throw std::invalid_argument("Note pitches count exceeds maximum of 8");
                }

                std::vector<int> pitches;
                pitches.reserve(pitches_json.size());
                std::set<int> unique_pitches;

                for (const auto& pitch_val : pitches_json) {
                    if (!pitch_val.is_number_integer() || pitch_val.is_boolean()) {
                        throw std::invalid_argument("Pitch must be an integer");
                    }
                    int64_t p = pitch_val.get<int64_t>();
                    if (p < 12 || p > 127) {
                        throw std::invalid_argument("Pitch must be between 12 and 127 (MusicXML octave 0..9)");
                    }
                    int p_int = static_cast<int>(p);
                    if (!unique_pitches.insert(p_int).second) {
                        throw std::invalid_argument("Duplicate pitch in note entry");
                    }
                    pitches.push_back(p_int);
                }

                meas_data.notes.push_back(NoteEntry{std::move(pitches), static_cast<int>(ticks)});
            }

            if (total_measure_ticks != 1920) {
                throw std::invalid_argument("Measure duration must sum to exactly 1920 ticks");
            }

            part_data.measures.push_back(std::move(meas_data));
        }

        validated_parts.push_back(std::move(part_data));
    }

    std::string xml;
    xml.reserve(4096);

    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<score-partwise version=\"4.0\">\n";
    xml += "  <work>\n";
    xml += "    <work-title>" + xml_escape(title) + "</work-title>\n";
    xml += "  </work>\n";

    xml += "  <part-list>\n";
    for (size_t i = 0; i < validated_parts.size(); ++i) {
        const auto id = "P" + std::to_string(i + 1);
        const auto& part = validated_parts[i];
        xml += "<score-part id=\"" + id + "\"><part-name>" + xml_escape(part.name) + "</part-name>";
        xml += "<score-instrument id=\"" + id + "-I1\"><instrument-name>" + xml_escape(part.name) + "</instrument-name></score-instrument>";
        xml += "<midi-instrument id=\"" + id + "-I1\"><midi-channel>" + std::to_string(i < 9 ? i + 1 : i + 2) + "</midi-channel>";
        xml += "<midi-program>" + std::to_string(part.midiProgram) + "</midi-program></midi-instrument></score-part>\n";
    }
    xml += "</part-list>\n";
    for (size_t i = 0; i < validated_parts.size(); ++i) {
        const auto& part = validated_parts[i];
        xml += "<part id=\"P" + std::to_string(i + 1) + "\">\n";
        for (size_t m = 0; m < part.measures.size(); ++m) {
            xml += "<measure number=\"" + std::to_string(m + 1) + "\">\n";
            if (m == 0) {
                xml += "<attributes><divisions>480</divisions><time><beats>4</beats><beat-type>4</beat-type></time>";
                xml += "<clef><sign>G</sign><line>2</line></clef></attributes>";
                if (i == 0) xml += "<direction><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>" + std::to_string(tempo)
                    + "</per-minute></metronome></direction-type><sound tempo=\"" + std::to_string(tempo) + "\"/></direction>";
            }
            for (const auto& note : part.measures[m].notes) {
                const auto count = std::max(size_t{1}, note.pitches.size());
                for (size_t p = 0; p < count; ++p) {
                    xml += "<note>";
                    if (p) xml += "<chord/>";
                    if (note.pitches.empty()) xml += "<rest/>";
                    else {
                        const int midi = note.pitches[p];
                        const auto pitch = CHROMATIC_SCALE[midi % 12];
                        xml += "<pitch><step>" + std::string(1, pitch.step) + "</step>";
                        if (pitch.alter) xml += "<alter>1</alter>";
                        xml += "<octave>" + std::to_string(midi / 12 - 1) + "</octave></pitch>";
                    }
                    xml += "<duration>" + std::to_string(note.ticks) + "</duration><voice>1</voice><type>" + ticks_to_type(note.ticks) + "</type></note>\n";
                }
            }
            xml += "</measure>\n";
        }
        xml += "</part>\n";
    }
    xml += "</score-partwise>\n";
    return xml;
}
}
