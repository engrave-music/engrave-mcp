"""Bootstrap the pinned engine submodule and optional SoundFont asset."""

import argparse
import hashlib
import os
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SUBMODULE = "vendor/engrave"
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


def ensure_source():
    path = ROOT / SUBMODULE
    status = query("git", "-C", str(ROOT), "submodule", "status", "--", SUBMODULE)
    if not status:
        raise RuntimeError(f"Missing submodule entry: {SUBMODULE}")
    if status[0] == "-":
        if path.exists() and (not path.is_dir() or any(path.iterdir())):
            raise RuntimeError(f"{SUBMODULE} is present but not an initialized submodule; refusing to overwrite")
        run(
            "git",
            "-C",
            str(ROOT),
            "submodule",
            "update",
            "--init",
            "--depth",
            "1",
            "--",
            SUBMODULE,
        )
        status = query("git", "-C", str(ROOT), "submodule", "status", "--", SUBMODULE)
    if not status or status[0] != " ":
        raise RuntimeError(f"{SUBMODULE} is modified or at a different commit; refusing to reset or overwrite")
    try:
        revision = query("git", "-C", str(path), "rev-parse", "HEAD")
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"{path} is not a Git checkout") from error
    print(f"Engine submodule ready at {revision}")


def ensure_soundfont():
    if SOUNDFONT.exists():
        if not SOUNDFONT.is_file() or digest(SOUNDFONT) != SOUNDFONT_SHA256:
            raise RuntimeError(f"{SOUNDFONT} has an unexpected SHA256; refusing to overwrite")
        print(f"SoundFont already verified: {SOUNDFONT}")
        return
    SOUNDFONT.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=f".{SOUNDFONT.name}.", suffix=".part", dir=SOUNDFONT.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            request = urllib.request.Request(SOUNDFONT_URL, headers={"User-Agent": "engrave-mcp-bootstrap/1"})
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


def main():
    parser = argparse.ArgumentParser(description="Prepare the pinned upstream checkout for a clean build.")
    parser.add_argument(
        "--soundfont",
        action="store_true",
        help="Download and verify .runtime/soundfonts/FluidR3Mono_GM.sf3",
    )
    arguments = parser.parse_args()
    ensure_source()
    if arguments.soundfont:
        ensure_soundfont()


if __name__ == "__main__":
    main()
