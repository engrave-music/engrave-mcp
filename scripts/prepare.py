"""Verify the pinned engine submodule and prepare native assets.

No C++ or CMake source transformation is performed by this production path.
"""
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/musescore"


def verify_submodule():
    status = subprocess.check_output(
        ["git", "-C", str(ROOT), "submodule", "status", "--", "vendor/musescore"], text=True,
    ).rstrip("\r\n")
    if not status:
        raise RuntimeError("vendor/musescore is not a submodule of this repository")
    marker = status[0]
    if marker == "-":
        raise RuntimeError("vendor/musescore is not initialized; run scripts/bootstrap.py first")
    if marker == "+":
        raise RuntimeError("vendor/musescore is at a different commit than the superproject records")
    if marker == "U":
        raise RuntimeError("vendor/musescore has a merge conflict; resolve it before building")
    if marker != " ":
        raise RuntimeError(f"Unexpected vendor/musescore submodule status: {status}")
    dirty = subprocess.check_output(
        ["git", "-C", str(SOURCE), "status", "--porcelain", "--untracked-files=normal"],
        text=True,
    )
    if dirty:
        raise RuntimeError(
            "vendor/musescore has local changes; commit or remove them before building:\n" + dirty
        )
    return status[1:].split()[0]


def read_asset(relative):
    if relative.startswith("repo:"):
        source = ROOT / relative[len("repo:"):]
        if not source.is_file():
            raise RuntimeError(f"Missing asset: {relative}")
        return source.read_bytes()
    # Read the committed blob so a CRLF worktree checkout cannot change staged text.
    try:
        return subprocess.check_output(
            ["git", "-C", str(SOURCE), "cat-file", "blob", f"HEAD:{relative}"],
        )
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"Missing asset in vendor/musescore: {relative}") from error


def main():
    revision = verify_submodule()
    files = json.loads((ROOT / "resources.json").read_text(encoding="utf-8-sig"))
    manifest = {}
    for alias, relative in files.items():
        data = read_asset(relative)
        digest = hashlib.sha256(data).hexdigest()
        target = ROOT / ".runtime/assets" / alias
        if not target.is_file() or target.read_bytes() != data:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        manifest[alias] = {"source": relative, "sha256": digest}
    # .runtime/assets is generated: drop files from earlier alias layouts so packages match resources.json.
    assets = ROOT / ".runtime/assets"
    for path in sorted(assets.rglob("*"), reverse=True):
        relative = path.relative_to(assets).as_posix()
        if path.is_file() and relative not in manifest:
            path.unlink()
        elif path.is_dir() and not any(path.iterdir()):
            path.rmdir()
    (ROOT / ".runtime/assets-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"Verified vendor/musescore at {revision}; prepared {len(files)} native resources")


if __name__ == "__main__":
    main()
