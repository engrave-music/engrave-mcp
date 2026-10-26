#pragma once

namespace music {

struct ExportOptions {
    int page = 0;
    int png_dpi = 144;
    bool draw_page_background = false;
    bool transparent = true;
    bool midi_expand_repeats = true;
    bool midi_export_rpns = true;
};

} // namespace music
