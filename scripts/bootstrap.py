"""Bootstrap the pinned upstream checkout and optional SoundFont asset."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOUNDFONT = ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3"
SOUNDFONT_URL = "https://cdn.jsdelivr.net/gh/musescore/MuseScore@2.1/share/sound/FluidR3Mono_GM.sf3"
SOUNDFONT_SHA256 = "cfcd66d89e8386823400eca64934b14fbea7bf48ba1f00d21189af1262794ec2"


def run(*arguments):
    subprocess.run(arguments, check=True)


def query(*arguments):
    return subprocess.check_output(arguments, text=True).rstrip("\r\n")


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def ensure_brotli(source):
    relative = "thirdparty/brotli"
    path = source / relative
    status = query("git", "-C", str(source), "submodule", "status", "--", relative)
    if not status:
        raise RuntimeError(f"Missing submodule entry: {relative}")
    if status[0] == "-":
        if path.exists() and (not path.is_dir() or any(path.iterdir())):
            raise RuntimeError(f"{relative} is present but not an initialized submodule; refusing to overwrite")
        run("git", "-C", str(source), "submodule", "update", "--init", "--", relative)
        status = query("git", "-C", str(source), "submodule", "status", "--", relative)
    if not status or status[0] != " ":
        raise RuntimeError(f"{relative} is modified or at a different commit; refusing to overwrite")


def ensure_source():
    pin = json.loads((ROOT / "upstream.lock.json").read_text(encoding="utf-8-sig"))
    source = ROOT / pin["sourceDirectory"]
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        run("git", "clone", "--no-recurse-submodules", pin["repository"], str(source))
        run("git", "-C", str(source), "checkout", "--detach", pin["revision"])
    try:
        revision = query("git", "-C", str(source), "rev-parse", "HEAD")
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"{source} is not a Git checkout") from error
    if revision != pin["revision"]:
        raise RuntimeError(
            f"Upstream HEAD {revision} does not match {pin['revision']}; refusing to reset or overwrite"
        )
    ensure_brotli(source)
    print(f"Upstream ready at {revision}")


def ensure_soundfont():
    if SOUNDFONT.exists():
        if not SOUNDFONT.is_file() or digest(SOUNDFONT) != SOUNDFONT_SHA256:
            raise RuntimeError(f"{SOUNDFONT} has an unexpected SHA256; refusing to overwrite")
        print(f"SoundFont already verified: {SOUNDFONT}")
        return
    SOUNDFONT.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{SOUNDFONT.name}.", suffix=".part", dir=SOUNDFONT.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            request = urllib.request.Request(SOUNDFONT_URL, headers={"User-Agent": "musescore-mcp-bootstrap/1"})
            with urllib.request.urlopen(request, timeout=120) as response:
                shutil.copyfileobj(response, output, 1024 * 1024)
        if digest(temporary) != SOUNDFONT_SHA256:
            raise RuntimeError("Downloaded SoundFont SHA256 mismatch; existing files left untouched")
        if SOUNDFONT.exists():
            if not SOUNDFONT.is_file() or digest(SOUNDFONT) != SOUNDFONT_SHA256:
                raise RuntimeError(f"{SOUNDFONT} appeared with an unexpected SHA256; refusing to overwrite")
            print(f"SoundFont already verified: {SOUNDFONT}")
            return
        os.replace(temporary, SOUNDFONT)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Downloaded and verified SoundFont: {SOUNDFONT}")


def ensure_harfbuzz():
    pin = json.loads((ROOT / "upstream.lock.json").read_text(encoding="utf-8-sig"))["harfbuzzDependency"]
    destination = ROOT / pin["sourceDirectory"]
    marker = destination / ".archive-sha256"
    if destination.exists():
        if (marker.is_file() and marker.read_text().strip() == pin["sha256"]
                and (destination / "harfbuzz/src/harfbuzz.cc").is_file()):
            print(f"HarfBuzz source already prepared: {destination}")
            return
        raise RuntimeError(f"{destination} exists without the expected archive marker; refusing to overwrite")
    extractor = shutil.which("7z") or shutil.which("7zz")
    if not extractor:
        raise RuntimeError("HarfBuzz bootstrap requires 7z or 7zz on PATH")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="harfbuzz-", dir=destination.parent) as staging_name:
        staging = Path(staging_name)
        archive = staging / "source.7z"
        request = urllib.request.Request(pin["archiveUrl"], headers={"User-Agent": "musescore-mcp-bootstrap/1"})
        with urllib.request.urlopen(request, timeout=120) as response, archive.open("wb") as output:
            shutil.copyfileobj(response, output, 1024 * 1024)
        if digest(archive) != pin["sha256"]:
            raise RuntimeError("HarfBuzz archive SHA256 mismatch")
        extraction_root = staging / "extracted"
        run(extractor, "x", str(archive), f"-o{extraction_root}", "-y")
        extracted = extraction_root / "harfbuzz"
        if not (extracted / "harfbuzz/src/harfbuzz.cc").is_file():
            raise RuntimeError("Unexpected HarfBuzz archive layout")
        (extracted / ".archive-sha256").write_text(pin["sha256"] + "\n", encoding="utf-8")
        extracted.rename(destination)
    print(f"Downloaded and prepared pinned HarfBuzz source: {destination}")


def main():
    parser = argparse.ArgumentParser(description="Prepare the pinned upstream checkout for a clean build.")
    parser.add_argument(
        "--harfbuzz", action="store_true", help="Download the pinned HarfBuzz sources for xmake",
    )
    parser.add_argument(
        "--soundfont",
        action="store_true",
        help="Download and verify .runtime/soundfonts/FluidR3Mono_GM.sf3",
    )
    arguments = parser.parse_args()
    ensure_source()
    if arguments.harfbuzz:
        ensure_harfbuzz()
    if arguments.soundfont:
        ensure_soundfont()


if __name__ == "__main__":
    main()
