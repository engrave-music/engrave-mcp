# Third-party notices

- `FluidR3Mono_License.md` contains the MIT notice for `FluidR3Mono_GM.sf3`.
  `scripts/bootstrap.py` downloads this SoundFont from MuseScore tag `2.1`.
  The notice is byte-identical to that tag
  (SHA-256 `09926f9451f751408abd65162c99343f840234d2a708584808fd46427633063f`).
- `codecs/` contains the license texts from FLAC 1.4.3, LAME 3.100, Opus
  1.5.2, and libopusenc 0.2.1, pinned in `vendor/engrave/bazel/deps.bzl`.
- `deps/` contains the notices from statically built Bazel dependencies.
  - `amc/` for amc 2.8.1
  - `unordered_dense/` for unordered_dense 5.3.3
  - `ngcpp_proxy/` for Proxy 4.1.0
  - `glaze/` for Glaze 9.1.0
  - `utfcpp/` for utfcpp 4.1.1
  - `pugixml/` for pugixml 1.15
  - `harfbuzz/` for HarfBuzz 12.3.0, including its Microsoft Universal Shaping Engine tables
  - `asio/` for the host's `@asio` 1.36.0 dependency (BSL-1.0)
- `engine/mnxdom-LICENSE` contains the MIT notice from the engine's bundled
  mnxdom music theory header.
- `freetype/FTL.TXT` contains the elected FreeType Project License (FTL) from
  the pinned FreeType 2.14.3 package.

Portions of this software are copyright © 2026 The FreeType Project (www.freetype.org). All rights reserved.

At package time, the packagers collect upstream license files under
`licenses/runtime/` and retain provenance under `licenses/conda-meta/` for
the Pixi packages that supply bundled shared libraries. The FreeType notice
above covers the bundled FreeType libraries.
