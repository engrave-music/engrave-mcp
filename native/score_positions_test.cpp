#include "noqt_runtime.h"
#include "score_positions.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Usage: score_positions_test ASSETS");
        music::initialize_native_runtime(argv[1]);
        const auto project = music::load_native_score(R"(<score-partwise version="4.0">
<part-list><score-part id="P1"><part-name>Piano</part-name></score-part></part-list>
<part id="P1"><measure number="1"><attributes><divisions>1</divisions>
<time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef>
</attributes><barline location="left"><repeat direction="forward"/></barline>
<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note>
<barline location="right"><repeat direction="backward"/></barline></measure></part>
</score-partwise>)", "musicxml");
        auto* score = project->masterScore();
        for (bool expanded : {false, true}) {
            for (bool segments : {false, true}) {
                score->setExpandRepeats(expanded);
                const auto data = music::score_positions(score, segments);
                expect(score->expandRepeats() == expanded, "Position query changed repeat setting");
                expect(data.at("elements").size() == 1 && data.at("events").size() == 2,
                       "Position query failed to expand the repeat");
            }
        }
        bool rejected = false;
        try { music::score_positions(nullptr, false); }
        catch (const std::invalid_argument&) { rejected = true; }
        expect(rejected, "Null score was accepted");
        std::cout << "Position queries leave both initial repeat settings unchanged; null score rejected\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
