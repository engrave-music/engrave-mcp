"""Notices and assets shared by the native package builders."""

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from bootstrap import SOUNDFONT_SHA256

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
    "codecs/opusenc/RESAMPLE-COPYING",
    "deps/amc/LICENSE",
    "deps/asio/COPYING",
    "deps/asio/LICENSE_1_0.txt",
    "deps/glaze/LICENSE",
    "deps/gtl/LICENSE",
    "deps/gtl/NOTICE",
    "deps/harfbuzz/COPYING",
    "deps/harfbuzz/hb-ucd-COPYING",
    "deps/harfbuzz/ms-use-COPYING",
    "deps/ngcpp_proxy/LICENSE",
    "deps/pugixml/LICENSE.md",
    "deps/unordered_dense/LICENSE",
    "deps/utfcpp/LICENSE",
    "engine/mnxdom-LICENSE",
    "freetype/FTL.TXT",
)

REQUIRED_RUNTIME_LICENSE_PACKAGES = (
    "cairo",
    "fontconfig",
    "libexpat",
    "libiconv",
    "libintl",
    "libpng",
    "libxcb",
    "libzlib",
    "pixman",
    "vc14_runtime",
    "xorg-libx11",
    "xorg-libxau",
    "xorg-libxdmcp",
)

ENGINE_THIRDPARTY_NOTICES = (
    ("muse/framework/audio/thirdparty/fluidsynth/fluidsynth-2.3.3/LICENSE", "thirdparty-fluidsynth-LICENSE"),
    ("src/engraving/thirdparty/dtl/COPYING", "thirdparty-dtl-COPYING"),
    ("src/importexport/midi/thirdparty/beatroot/COPYING", "thirdparty-beatroot-COPYING"),
    ("muse/framework/global/thirdparty/kors_logger/LICENSE", "thirdparty-kors_logger-LICENSE"),
    ("src/engraving/thirdparty/intervaltree/LICENSE", "thirdparty-intervaltree-LICENSE"),
)


def staged_font_dirs(engine_root: Path) -> tuple[str, ...]:
    manifest = json.loads((engine_root / "engrave/assets.json").read_text(encoding="utf-8"))
    dirs = set()
    for target in manifest.get("files", {}):
        parts = Path(target).parts
        if len(parts) >= 3 and parts[0] == "fonts" and parts[1] != "smufl":
            dirs.add(parts[1])
    if not dirs:
        raise RuntimeError(f"No staged font directories found in {engine_root / 'engrave/assets.json'}")
    return tuple(sorted(dirs))


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


def _checked_in_runtime_licenses(root: Path, package: str) -> list[Path]:
    directory = root / "licenses/runtime" / package
    files = [path for path in sorted(directory.rglob("*")) if path.is_file()] if directory.is_dir() else []
    if not files:
        raise RuntimeError(f"Missing checked-in runtime license files for {package} under {directory}")
    return files


def collect_package_notices(root: Path, engine_root: Path) -> list[Notice]:
    """Return (category, source, path relative to the package's license directory)."""
    host_notices = root / "licenses"
    _require_notice(root / "LICENSE")
    for relative in REQUIRED_HOST_NOTICES:
        _require_notice(host_notices / relative)
    for package in REQUIRED_RUNTIME_LICENSE_PACKAGES:
        _checked_in_runtime_licenses(root, package)

    runtime_dir = host_notices / "runtime"
    notices = [("host", root / "LICENSE", "LICENSE")]
    notices.extend(
        ("host", path, path.relative_to(host_notices).as_posix())
        for path in sorted(host_notices.rglob("*"))
        if path.is_file() and not path.is_relative_to(runtime_dir)
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
    for source, destination in (
        ("README.md", "font-README.md"),
        ("OFL.txt", "font-OFL.txt"),
        ("smufl/README.md", "font-smufl-README.md"),
    ):
        path = fonts_root / source
        _require_notice(path)
        notices.append(("font", path, destination))
    for font in staged_font_dirs(engine_root):
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


def collect_pixi_runtime_notices(root: Path, environment: Path, bundled_paths: list[Path]) -> list[Notice]:
    """Read conda-meta provenance and checked-in licenses for bundled Pixi libraries."""
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
        directory = root / "licenses/runtime" / package
        for path in _checked_in_runtime_licenses(root, package):
            notices.append(("pixi", path, f"runtime/{package}/{path.relative_to(directory).as_posix()}"))

    _validate_destinations(notices)
    return notices


def collect_vc_redist_notices(root: Path, vc_redist_dir: str) -> list[Notice]:
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

    directory = root / "licenses/runtime/vc14_runtime"
    notices = [
        ("vcredist", path, f"runtime/vcredist/{path.relative_to(directory).as_posix()}")
        for path in _checked_in_runtime_licenses(root, "vc14_runtime")
    ]
    _validate_destinations(notices)
    return notices


def _file_sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


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
    if _file_sha256(soundfont) == SOUNDFONT_SHA256:
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
