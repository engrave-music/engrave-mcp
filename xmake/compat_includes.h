/* Standard headers the MuseScore 5.0 engine expects to arrive transitively.
 *
 * Upstream compiles with Qt and (optionally) a precompiled header, so some
 * engine headers use standard-library types without including their own
 * headers. The Qt-free, PCH-less build does not provide them:
 *
 *   src/engraving/dom/selectionfilter.h:79        std::variant after only
 *                                                 "mscore.h" and "chord.h"
 *   src/engraving/rendering/paintoptions.h:40     std::function after only
 *                                                 "types/color.h"
 *
 * MuseScore 4.7.6 hit the same class of breakage and fixed it in the sources
 * (fork topic T9, "build: missing includes"). Until a 5.0 equivalent lands,
 * xmake.lua force-includes this file for every target (see the compat_includes
 * block). Delete this file and that call once the engine includes what it uses.
 *
 * C sources (FluidSynth, LAME) are force-included as well, so the C++
 * headers only appear for C++ translation units.
 */
#pragma once

#ifdef __cplusplus
#include <functional>
#include <variant>
#endif
