"""Verify the pinned engine submodule and stage/verify native assets via the engine installer.

No C++ or CMake source transformation is performed by this production path.
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "vendor/engrave"
TOOLS = SOURCE / "tools"


def verify_submodule():
    status = subprocess.check_output(
        ["git", "-C", str(ROOT), "submodule", "status", "--", "vendor/engrave"],
        text=True,
    ).rstrip("\r\n")
    if not status:
        raise RuntimeError("vendor/engrave is not a submodule of this repository")
    marker = status[0]
    if marker == "-":
        raise RuntimeError("vendor/engrave is not initialized; run `git submodule update --init vendor/engrave` first")
    if marker == "+":
        raise RuntimeError("vendor/engrave is at a different commit than the superproject records")
    if marker == "U":
        raise RuntimeError("vendor/engrave has a merge conflict; resolve it before building")
    if marker != " ":
        raise RuntimeError(f"Unexpected vendor/engrave submodule status: {status}")
    dirty = subprocess.check_output(
        ["git", "-C", str(SOURCE), "status", "--porcelain", "--untracked-files=normal"],
        text=True,
    )
    if dirty:
        raise RuntimeError("vendor/engrave has local changes; commit or remove them before building:\n" + dirty)
    return status[1:].split()[0]


def stage_assets(assets: Path) -> bool:
    """Ensure assets directory matches the engine's authoritative manifest.

    Returns True if assets were freshly staged or repaired, False if already verified.
    """
    sys.path.insert(0, str(TOOLS))
    import install_assets

    if assets.is_dir():
        try:
            install_assets.install(assets, verify=True)
            return False
        except Exception:
            # Installed directory is absent, incomplete or outdated; repair below.
            pass

    runtime_dir = assets.parent
    runtime_dir.mkdir(parents=True, exist_ok=True)

    if not assets.exists():
        install_assets.install(assets, verify=False)
        return True

    # Destination exists but failed verification. Build replacement safely in a temp dir
    # and swap into place, leaving no partially published directory.
    temp_parent = Path(tempfile.mkdtemp(prefix=".assets-build-", dir=runtime_dir))
    try:
        replacement = temp_parent / "assets"
        install_assets.install(replacement, verify=False)
        stale_parent = Path(tempfile.mkdtemp(prefix=".assets-stale-", dir=runtime_dir))
        stale_backup = stale_parent / "assets"
        assets.rename(stale_backup)
        try:
            replacement.rename(assets)
        except Exception:
            if stale_backup.exists() and not assets.exists():
                stale_backup.rename(assets)
            raise
        finally:
            shutil.rmtree(stale_parent, ignore_errors=True)
    finally:
        shutil.rmtree(temp_parent, ignore_errors=True)
    return True


def main():
    revision = verify_submodule()
    assets = ROOT / ".runtime/assets"
    staged = stage_assets(assets)

    # Clean up legacy .runtime/assets-manifest.json if present; assets.json inside
    # .runtime/assets is the single authoritative manifest and nothing consumes the external file.
    legacy_manifest = ROOT / ".runtime/assets-manifest.json"
    if legacy_manifest.is_file():
        legacy_manifest.unlink()

    sys.path.insert(0, str(TOOLS))
    import json

    import install_assets

    manifest = json.loads(install_assets.MANIFEST.read_bytes())
    count = len(manifest["files"])

    action = "prepared" if staged else "verified"
    print(f"Verified vendor/engrave at {revision}; {action} {count} native resources")


if __name__ == "__main__":
    main()
