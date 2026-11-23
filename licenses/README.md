# Third-party notices

- `FluidR3Mono_License.md` contains the MIT notice for `FluidR3Mono_GM.sf3`.
  `scripts/bootstrap.py` downloads this SoundFont from MuseScore tag `2.1`.
  The notice is byte-identical to that tag
  (SHA-256 `09926f9451f751408abd65162c99343f840234d2a708584808fd46427633063f`).
- `codecs/` contains the license texts from FLAC 1.4.3, LAME 3.100, Opus
  1.5.2, and libopusenc 0.2.1 (including its bundled `resample.c` BSD-3-Clause
  notice), pinned in `vendor/engrave/bazel/deps.bzl`.
- `deps/` contains the notices from statically built Bazel dependencies.
  - `amc/` for amc 2.8.1
  - `gtl/` for gtl 1.3.1 (Apache-2.0, including its upstream `NOTICE` file attributing `btree.hpp` to The Abseil Authors)
  - `unordered_dense/` for unordered_dense 5.3.3
  - `ngcpp_proxy/` for Proxy 4.1.0
  - `glaze/` for Glaze 9.1.0
  - `utfcpp/` for utfcpp 4.1.1
  - `pugixml/` for pugixml 1.15
  - `harfbuzz/` for HarfBuzz 12.3.0, including `hb-ucd.cc` (ISC, generated from Unicode 17.0.0 character data under `Unicode-3.0`) and its Microsoft Universal Shaping Engine tables
  - `asio/` for the host's `@asio` 1.36.0 dependency (BSL-1.0)
- `engine/mnxdom-LICENSE` contains the MIT notice from the engine's bundled
  mnxdom music theory header.
- `freetype/FTL.TXT` contains the elected FreeType Project License (FTL) from
  the pinned FreeType 2.14.3 package.
- `runtime/` contains verified upstream license texts for the pinned Pixi
  runtime shared libraries (platform-filtered at package time so each package
  includes only the libraries it actually bundles).

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.

Binaries are compiled with the hermetic LLVM/xclang toolchain (`Apache-2.0 WITH LLVM-exception`; MinGW-w64 runtime on Windows). At package time, the packagers select matching notices from `licenses/runtime/` and retain filtered `licenses/conda-meta/` provenance for the Pixi packages that supply bundled shared libraries. The FreeType notice above covers the bundled FreeType libraries.
