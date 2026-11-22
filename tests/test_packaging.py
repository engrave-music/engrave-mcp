"""Exercise the notice and runtime selection used by the native packagers."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from _package_common import (  # noqa: E402
    collect_package_notices,
    collect_pixi_runtime_notices,
    collect_vc_redist_notices,
    validate_soundfont,
)
from package_windows import build_windows_dll_map  # noqa: E402


def write_fixture(path: Path, contents: str = "") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")
    return path


class StaticNoticeTests(unittest.TestCase):
    def test_notices_use_canonical_paths(self):
        notices = collect_package_notices(ROOT, ROOT / "vendor/engrave")
        destinations = [destination for _, _, destination in notices]
        self.assertEqual(len(destinations), len({destination.casefold() for destination in destinations}))
        self.assertTrue(
            {
                "LICENSE",
                "engine-LICENSE.txt",
                "FluidR3Mono_License.md",
                "freetype/FTL.TXT",
                "deps/amc/LICENSE",
                "deps/asio/LICENSE_1_0.txt",
                "deps/asio/COPYING",
                "deps/glaze/LICENSE",
                "deps/harfbuzz/COPYING",
                "deps/harfbuzz/hb-ucd-COPYING",
                "deps/harfbuzz/ms-use-COPYING",
                "deps/ngcpp_proxy/LICENSE",
                "deps/pugixml/LICENSE.md",
                "deps/unordered_dense/LICENSE",
                "deps/utfcpp/LICENSE",
                "engine/mnxdom-LICENSE",
                "thirdparty-intervaltree-LICENSE",
                "thirdparty-kors_logger-LICENSE",
                "engine-THIRD_PARTY_NOTICES.md",
                "font-OFL.txt",
                "font-smufl-README.md",
            }.issubset(destinations)
        )
        codecs = {destination for _, source, destination in notices if source.is_relative_to(ROOT / "licenses/codecs")}
        self.assertEqual(
            codecs,
            {
                "codecs/flac/COPYING.FDL",
                "codecs/flac/COPYING.GPL",
                "codecs/flac/COPYING.LGPL",
                "codecs/flac/COPYING.Xiph",
                "codecs/lame/COPYING",
                "codecs/lame/LICENSE",
                "codecs/opus/COPYING",
                "codecs/opusenc/COPYING",
                "codecs/opusenc/RESAMPLE-COPYING",
            },
        )
        for destination in destinations:
            self.assertFalse(destination.startswith("runtime/"), destination)
            self.assertNotIn("gplv2.txt", destination.casefold())
            self.assertNotIn("fdk-aac", destination.casefold())

    def test_requires_engine_license_txt(self):
        with tempfile.TemporaryDirectory() as temporary:
            engine = Path(temporary) / "engrave"
            write_fixture(engine / "LICENSE-unrelated", "not the engine license")
            with self.assertRaisesRegex(RuntimeError, r"Missing required package notice: .*LICENSE\.txt"):
                collect_package_notices(ROOT, engine)

    def test_soundfont_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            impostor = write_fixture(root / "FluidR3Mono_GM.sf3", "not fluidr3mono")
            custom = write_fixture(root / "Custom.sf2", "sf2")
            custom_license = write_fixture(root / "CUSTOM-LICENSE.txt", "MIT")

            self.assertEqual(validate_soundfont(None, None), [])
            self.assertEqual(
                validate_soundfont(custom, custom_license),
                [("soundfont", custom_license, "soundfont-CUSTOM-LICENSE.txt")],
            )
            with self.assertRaisesRegex(RuntimeError, "requires --soundfont-license"):
                validate_soundfont(impostor, None)
            with self.assertRaisesRegex(RuntimeError, "requires --soundfont"):
                validate_soundfont(None, custom_license)
            with self.assertRaisesRegex(RuntimeError, "not a regular file"):
                validate_soundfont(root / "missing.sf3", None)
            with self.assertRaisesRegex(RuntimeError, "requires --soundfont-license"):
                validate_soundfont(custom, None)
            with self.assertRaisesRegex(RuntimeError, "SoundFont license is not a regular file"):
                validate_soundfont(custom, root / "missing.txt")

    def test_windows_release_redist_overlays_pixi(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            library_bin = root / "pixi/Library/bin"
            pixi_crt = write_fixture(library_bin / "vcruntime140.dll")
            cairo = write_fixture(library_bin / "cairo.dll")
            redist = root / "redist"
            crt = write_fixture(redist / "x64/Microsoft.VC145.CRT/VCRUNTIME140.dll")
            crt_extra = write_fixture(redist / "x64/Microsoft.VC145.CRT/vcruntime140_1.dll")
            write_fixture(redist / "x64/Microsoft.VC145.MFC/vcruntime140.dll")
            write_fixture(redist / "x64/Debug_NonRedist/cairo.dll")
            write_fixture(redist / "x64/Debug_NonRedist/vcruntime140d.dll")
            write_fixture(redist / "arm64/Microsoft.VC145.CRT/cairo.dll")

            self.assertEqual(
                build_windows_dll_map(library_bin, None), {"vcruntime140.dll": pixi_crt, "cairo.dll": cairo}
            )
            self.assertEqual(
                build_windows_dll_map(library_bin, str(redist)),
                {"vcruntime140.dll": crt, "vcruntime140_1.dll": crt_extra, "cairo.dll": cairo},
            )


class RuntimeNoticeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.repo = self.root / "repo"
        self.environment = self.root / "env"

    def add_package(self, name: str, installed_file: str, licenses: tuple[str, ...] = ()) -> None:
        # Point extracted_package_dir at a non-existent path to prove Rattler cache independence.
        write_fixture(
            self.environment / f"conda-meta/{name}-1.0-0.json",
            json.dumps(
                {
                    "name": name,
                    "files": [installed_file],
                    "extracted_package_dir": str(self.root / "missing-rattler-cache" / name),
                }
            ),
        )
        for license_path in licenses:
            write_fixture(self.repo / "licenses/runtime" / name / license_path, f"{name} upstream notice")

    def test_copies_only_the_owners_and_checked_in_licenses(self):
        self.add_package("cairo", "Library/bin/CAIRO.DLL" if os.name == "nt" else "Library/bin/cairo.dll", ("COPYING",))
        self.add_package("vc14_runtime", "Library/bin/vcruntime140.dll", ("LICENSE.TXT",))
        for name in ("freetype", "libfreetype", "libfreetype6"):
            self.add_package(name, f"Library/bin/{name}.dll")
        self.add_package("unused", "Library/bin/unused.dll", ("COPYING",))
        bundled = [
            write_fixture(self.environment / f"Library/bin/{name}.dll")
            for name in ("cairo", "vcruntime140", "freetype", "libfreetype", "libfreetype6")
        ]
        bundled.append(write_fixture(self.root / "redist/vcruntime140_1.dll"))

        notices = collect_pixi_runtime_notices(self.repo, self.environment, bundled)

        self.assertEqual(
            {destination for _, _, destination in notices},
            {
                "conda-meta/cairo-1.0-0.json",
                "conda-meta/freetype-1.0-0.json",
                "conda-meta/libfreetype-1.0-0.json",
                "conda-meta/libfreetype6-1.0-0.json",
                "conda-meta/vc14_runtime-1.0-0.json",
                "runtime/cairo/COPYING",
                "runtime/vc14_runtime/LICENSE.TXT",
            },
        )
        for _, source, destination in notices:
            if destination.startswith("runtime/"):
                package = destination.split("/")[1]
                self.assertEqual(source.read_text(encoding="utf-8"), f"{package} upstream notice")

    @unittest.skipIf(os.name == "nt", "ELF loader paths may be symlinks")
    def test_resolves_library_symlink_for_conda_ownership(self):
        versioned = write_fixture(self.environment / "lib/libsample.so.1.2")
        alias = versioned.with_name("libsample.so.1")
        alias.symlink_to(versioned.name)
        self.add_package("sample", "lib/libsample.so.1.2", ("nested/LICENSE",))
        notices = collect_pixi_runtime_notices(self.repo, self.environment, [alias])
        self.assertEqual(
            {destination for _, _, destination in notices},
            {"conda-meta/sample-1.0-0.json", "runtime/sample/nested/LICENSE"},
        )

    def test_rejects_unattributed_bundled_file(self):
        bundled = write_fixture(self.environment / "lib/extra.so")
        with self.assertRaisesRegex(RuntimeError, "No Pixi package owns bundled library: .*extra.so"):
            collect_pixi_runtime_notices(self.repo, self.environment, [bundled])

    def test_requires_checked_in_licenses_for_bundled_libraries(self):
        bundled = write_fixture(self.environment / "lib/sample.so")
        self.add_package("sample", "lib/sample.so")
        with self.assertRaisesRegex(RuntimeError, "Missing checked-in runtime license files for sample"):
            collect_pixi_runtime_notices(self.repo, self.environment, [bundled])

    def test_vc_redist_license_resolution(self):
        redist = self.root / "redist"
        redist.mkdir()
        with self.assertRaisesRegex(RuntimeError, "Missing checked-in runtime license files for vc14_runtime"):
            collect_vc_redist_notices(self.repo, str(redist))

        write_fixture(self.repo / "licenses/runtime/vc14_runtime/LICENSE.TXT", "vc14 fallback")
        fallback = collect_vc_redist_notices(self.repo, str(redist))
        self.assertEqual([dst for _, _, dst in fallback], ["runtime/vcredist/LICENSE.TXT"])

        direct_license = write_fixture(redist / "License.txt", "direct vc redist license")
        direct = collect_vc_redist_notices(self.repo, str(redist))
        self.assertEqual(direct, [("vcredist", direct_license.resolve(), "runtime/vcredist/License.txt")])


if __name__ == "__main__":
    unittest.main()
