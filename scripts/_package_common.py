"""Notices and assets shared by the native package builders."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

Notice = tuple[str, Path, str]

REQUIRED_HOST_NOTICES = (
    "FluidR3Mono_License.md",
    "README.md",
    "codecs/flac/COPYING.FDL",
    "codecs/flac/COPYING.GPL",
    "codecs/flac/COPYING.LGPL",
    "codecs/flac/COPYING.Xiph",
    "codecs/lame/COPYING",
    "codecs/lame/LICENSE",
    "codecs/opus/COPYING",
    "codecs/opusenc/COPYING",
    "deps/amc/LICENSE",
    "deps/asio/COPYING",
    "deps/asio/LICENSE_1_0.txt",
    "deps/glaze/LICENSE",
    "deps/harfbuzz/COPYING",
    "deps/harfbuzz/ms-use-COPYING",
    "deps/ngcpp_proxy/LICENSE",
    "deps/pugixml/LICENSE.md",
    "deps/unordered_dense/LICENSE",
    "deps/utfcpp/LICENSE",
    "engine/mnxdom-LICENSE",
    "freetype/FTL.TXT",
)

ENGINE_THIRDPARTY_NOTICES = (
    ("muse/framework/audio/thirdparty/fluidsynth/fluidsynth-2.3.3/LICENSE", "thirdparty-fluidsynth-LICENSE"),
    ("src/engraving/thirdparty/dtl/COPYING", "thirdparty-dtl-COPYING"),
    ("src/importexport/midi/thirdparty/beatroot/COPYING", "thirdparty-beatroot-COPYING"),
    ("muse/framework/global/thirdparty/kors_logger/LICENSE", "thirdparty-kors_logger-LICENSE"),
    ("src/engraving/thirdparty/intervaltree/LICENSE", "thirdparty-intervaltree-LICENSE"),
)

STAGED_FONT_DIRS = (
    "bravura",
    "campania",
    "edwin",
    "finalebroadway",
    "finalemaestro",
    "gootville",
    "leland",
    "mscore",
    "musejazz",
    "petaluma",
)


def resolve_engine_root(root: Path, override: Path | None = None) -> Path:
    return (override or root / "vendor/engrave").resolve()


def _require_notice(path: Path) -> None:
    if not path.is_file():
        raise RuntimeError(f"Missing required package notice: {path}")


def _validate_destinations(notices: list[Notice]) -> None:
    seen = set()
    for _, _, destination in notices:
        key = destination.casefold()
        if "gplv2.txt" in key.split("/") or "fdk-aac" in key:
            raise RuntimeError(f"Forbidden package notice: {destination}")
        if key in seen:
            raise RuntimeError(f"Duplicate package notice destination: {destination}")
        seen.add(key)


def collect_package_notices(root: Path, engine_root: Path) -> list[Notice]:
    """Return (category, source, path relative to the package's license directory)."""
    host_notices = root / "licenses"
    _require_notice(root / "LICENSE")
    for relative in REQUIRED_HOST_NOTICES:
        _require_notice(host_notices / relative)
    notices = [("host", root / "LICENSE", "LICENSE")]
    notices.extend(
        ("host", path, path.relative_to(host_notices).as_posix())
        for path in sorted(host_notices.rglob("*"))
        if path.is_file()
    )

    engine_license = engine_root / "LICENSE.txt"
    _require_notice(engine_license)
    engine_licenses = [path for path in sorted(engine_root.glob("LICENSE*")) if path.is_file()]
    notices.extend(("engine", path, f"engine-{path.name}") for path in engine_licenses)
    for source, destination in (
        ("THIRD_PARTY_NOTICES.md", "engine-THIRD_PARTY_NOTICES.md"),
        *ENGINE_THIRDPARTY_NOTICES,
    ):
        path = engine_root / source
        _require_notice(path)
        notices.append(("engine", path, destination))

    fonts_root = engine_root / "fonts"
    for source, destination in (("README.md", "font-README.md"), ("OFL.txt", "font-OFL.txt")):
        path = fonts_root / source
        _require_notice(path)
        notices.append(("font", path, destination))
    smufl = fonts_root / "smufl/README.md"
    if smufl.is_file():
        notices.append(("font", smufl, "font-smufl-README.md"))
    for font in STAGED_FONT_DIRS:
        directory = fonts_root / font
        found = [
            path
            for path in sorted(directory.glob("*"))
            if path.is_file() and any(word in path.name.upper() for word in ("OFL", "LICENSE", "COPYING", "README"))
        ]
        # The shared fonts README supplies the MScore notice.
        if not found and font != "mscore":
            raise RuntimeError(f"Missing notice for staged font: {directory}")
        notices.extend(("font", path, f"font-{font}-{path.name}") for path in found)

    _validate_destinations(notices)
    return notices


def _environment_paths(environment: Path, path: Path) -> tuple[str, ...]:
    # Conda may record either the loader's symlink or the versioned file it points to.
    candidates = ((path.absolute(), environment.absolute()), (path.resolve(), environment.resolve()))
    return tuple(
        dict.fromkeys(
            os.path.normcase(p.relative_to(root).as_posix()) for p, root in candidates if p.is_relative_to(root)
        )
    )


def _pixi_package_license_files(record: dict, source: Path) -> list[Path]:
    package = record["name"]
    extracted = record.get("extracted_package_dir")
    if not extracted:
        raise RuntimeError(f"Missing extracted package directory in {source}")
    directory = Path(extracted) / "info/licenses"
    files = [path for path in sorted(directory.rglob("*")) if path.is_file()]
    if not files:
        raise RuntimeError(f"Missing runtime license files for {package} under {directory}")
    text_directories = {path.parent for path in files if path.suffix.lower() == ".txt"}
    return [
        path for path in files if not (path.suffix.lower() in {".docx", ".rtf"} and path.parent in text_directories)
    ]


def collect_pixi_runtime_notices(environment: Path, bundled_paths: list[Path]) -> list[Notice]:
    """Read provenance and licenses for the Pixi packages that supply bundled libraries."""
    installed_paths = {path: keys for path in bundled_paths if (keys := _environment_paths(environment, path))}
    if not installed_paths:
        return []

    wanted = {key for keys in installed_paths.values() for key in keys}
    owners = {}
    metadata = {}
    for source in sorted((environment / "conda-meta").glob("*.json")):
        record = json.loads(source.read_text(encoding="utf-8"))
        for entry in record.get("files", ()):
            key = os.path.normcase(Path(entry).as_posix())
            if key in wanted:
                owners[key] = source
                metadata[source] = record

    matched = set()
    for path, keys in installed_paths.items():
        owner = next((owners[key] for key in keys if key in owners), None)
        if owner is None:
            raise RuntimeError(f"No Pixi package owns bundled library: {path}")
        matched.add(owner)

    notices = []
    for source in sorted(matched):
        record = metadata[source]
        package = record["name"]
        notices.append(("pixi", source, f"conda-meta/{source.name}"))
        if package in {"freetype", "libfreetype", "libfreetype6"}:
            continue
        directory = Path(record["extracted_package_dir"]) / "info/licenses"
        for path in _pixi_package_license_files(record, source):
            notices.append(("pixi", path, f"runtime/{package}/{path.relative_to(directory).as_posix()}"))

    _validate_destinations(notices)
    return notices


def collect_vc_redist_notices(vc_redist_dir: str, environment: Path) -> list[Notice]:
    """Locate the Visual C++ Runtime license for DLLs bundled from VCToolsRedistDir."""
    redist = Path(vc_redist_dir).resolve()
    direct = [
        path
        for path in sorted(redist.glob("*"))
        if path.is_file() and path.suffix.lower() == ".txt" and "license" in path.name.lower()
    ]
    if direct:
        notices = [("vcredist", path, f"runtime/vcredist/{path.name}") for path in direct]
        _validate_destinations(notices)
        return notices

    conda_meta = environment / "conda-meta"
    if conda_meta.is_dir():
        for source in sorted(conda_meta.glob("vc14_runtime-*.json")):
            record = json.loads(source.read_text(encoding="utf-8"))
            if record.get("name") != "vc14_runtime":
                continue
            directory = Path(record["extracted_package_dir"]) / "info/licenses"
            notices = [
                ("vcredist", path, f"runtime/vcredist/{path.relative_to(directory).as_posix()}")
                for path in _pixi_package_license_files(record, source)
            ]
            _validate_destinations(notices)
            return notices

    raise RuntimeError(
        f"Missing Visual C++ Runtime license for DLLs bundled from {redist}; "
        "expected a license .txt file in VCToolsRedistDir or vc14_runtime metadata in Pixi"
    )


def validate_soundfont(soundfont: Path | None, soundfont_license: Path | None = None) -> list[Notice]:
    if soundfont is None:
        if soundfont_license is not None:
            raise RuntimeError("--soundfont-license requires --soundfont")
        return []
    if not soundfont.is_file():
        raise RuntimeError(f"SoundFont is not a regular file: {soundfont}")
    if soundfont_license is not None:
        if not soundfont_license.is_file():
            raise RuntimeError(f"SoundFont license is not a regular file: {soundfont_license}")
        return [("soundfont", soundfont_license, f"soundfont-{soundfont_license.name}")]
    if soundfont.name == "FluidR3Mono_GM.sf3":
        return []
    raise RuntimeError(f"Custom SoundFont {soundfont.name} requires --soundfont-license")


def stage_package_notices(destination: Path, notices: list[Notice]) -> None:
    _validate_destinations(notices)
    for _, source, relative in notices:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def stage_engine_assets(engine_root: Path, destination: Path) -> None:
    subprocess.run([sys.executable, str(engine_root / "tools/install_assets.py"), str(destination)], check=True)
    if not (destination / "assets.json").is_file():
        raise RuntimeError(f"Missing staged asset manifest: {destination / 'assets.json'}")


def stage_soundfont(soundfont: Path | None, destination: Path) -> None:
    if soundfont is None:
        return
    if not soundfont.is_file():
        raise RuntimeError(f"SoundFont is not a regular file: {soundfont}")
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(soundfont, destination / soundfont.name)
