"""Verify the pinned source, apply the reviewed patch, and prepare native assets.

No C++ or CMake source transformation is performed by this production path.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / ".upstream/webmscore"


def main():
    pin = json.loads((ROOT / "upstream.lock.json").read_text(encoding="utf-8-sig"))
    if not (SOURCE / ".git").exists():
        raise RuntimeError("Run scripts/bootstrap.py first to fetch the pinned source")
    revision = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip()
    if revision != pin["revision"]:
        raise RuntimeError("Upstream does not match upstream.lock.json")
    expected = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD:thirdparty/brotli"], text=True).strip()
    actual = subprocess.check_output(["git", "-C", str(SOURCE / "thirdparty/brotli"), "rev-parse", "HEAD"], text=True).strip()
    if actual != expected:
        raise RuntimeError("Initialize the pinned brotli submodule with scripts/bootstrap.py")
    patches = [ROOT / "patches/native-platform.patch", ROOT / "patches/native-imports.patch",
               ROOT / "patches/native-edit.patch", ROOT / "patches/native-archive.patch",
               ROOT / "patches/native-fonts.patch"]
    command = ["git", "-C", str(SOURCE), "apply", "--check"]
    for patch in patches:
        if not patch.is_file():
            raise RuntimeError(f"Missing {patch.relative_to(ROOT)}")
        if subprocess.run(command + [str(patch)], capture_output=True).returncode == 0:
            subprocess.run(["git", "-C", str(SOURCE), "apply", str(patch)], check=True)
        elif subprocess.run(command + ["--reverse", str(patch)], capture_output=True).returncode:
            raise RuntimeError(f"Upstream patch conflicts: {patch.name}; refusing to overwrite changes")
    # Validate the whole tracked tree, not just whether individual hunks apply.
    # A temporary index leaves the developer's real index and edits untouched.
    with tempfile.TemporaryDirectory(prefix="musescore-index-") as temporary:
        environment = {**os.environ, "GIT_INDEX_FILE": str(Path(temporary) / "index")}
        git = ["git", "-C", str(SOURCE)]
        subprocess.run(git + ["read-tree", "HEAD"], env=environment, check=True)
        for patch in patches:
            subprocess.run(git + ["apply", "--cached", str(patch)], env=environment, check=True)
        expected_tree = subprocess.check_output(git + ["write-tree"], env=environment, text=True).strip()
        if subprocess.run(git + ["diff", "--quiet", expected_tree, "--"], env=environment).returncode:
            raise RuntimeError("Upstream contains changes beyond the pinned revision plus reviewed patch")
        if subprocess.check_output(git + ["ls-files", "--others", "--exclude-standard"], env=environment, text=True).strip():
            raise RuntimeError("Upstream contains untracked files; inspect them before building")
    files = json.loads((ROOT / "resources.json").read_text(encoding="utf-8-sig"))
    manifest = {}
    for alias, relative in files.items():
        source, target = SOURCE / relative, ROOT / ".runtime/assets" / alias
        if not source.is_file():
            raise RuntimeError(f"Missing asset: {relative}")
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        manifest[alias] = {"source": relative, "sha256": digest}
    (ROOT / ".runtime/assets-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"Verified upstream {revision}; prepared {len(files)} native resources")


if __name__ == "__main__":
    main()
