"""Assemble the Windows dependency closure without requiring a MuseScore install."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/windows-package")
    parser.add_argument("--no-qt", action="store_true", help="Package the Qt-free backend and external resources")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise SystemExit("Package output must be empty; choose a new --output directory")
    library = ROOT / ".pixi/envs/default/Library"
    redist = Path(os.environ["VCToolsRedistDir"]) / "x64"
    available = {p.name.lower(): p for p in redist.rglob("*.dll") if "debug" not in str(p).lower()}
    available.update({p.name.lower(): p for p in (library / "bin").glob("*.dll")})
    queue = []
    origins = {}

    def copy(source, destination):
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        origins[str(destination.relative_to(output))] = str(source)
        queue.append(destination)

    build = "noqt-windows" if args.no_qt else "windows"
    copy(ROOT / f"build/{build}/musescore-mcp.exe", output / "musescore-mcp.exe")
    if not args.no_qt:
        copy(library / "lib/qt6/plugins/platforms/qoffscreen.dll", output / "plugins/platforms/qoffscreen.dll")
    system = Path(os.environ["SystemRoot"]) / "System32"
    while queue:
        binary = queue.pop()
        result = subprocess.run(["dumpbin", "/dependents", str(binary)], capture_output=True, check=True)
        for name in re.findall(r"^\s+([A-Za-z0-9_.-]+\.dll)\s*$", result.stdout.decode("utf-8", errors="replace"), re.M):
            key = name.lower()
            if args.no_qt and key.startswith(("qt5", "qt6")):
                raise RuntimeError(f"Qt dependency found in Qt-free package: {name}")
            if key.startswith(("api-ms-", "ext-ms-")):
                continue
            if key in available:
                destination = output / available[key].name
                if not destination.exists():
                    copy(available[key], destination)
            elif not (system / name).exists():
                raise RuntimeError(f"Unresolved dependency: {name} from {binary.name}")
    if args.no_qt:
        shutil.copytree(ROOT / ".runtime/assets", output / "assets")
    else:
        (output / "qt.conf").write_text("[Paths]\nPrefix=.\nLibraries=.\nPlugins=plugins\n", encoding="utf-8")
    soundfont = ROOT / ".runtime/soundfonts/FluidR3Mono_GM.sf3"
    if soundfont.exists():
        copy(soundfont, output / "soundfonts/FluidR3Mono_GM.sf3")
    licenses = output / "licenses"
    licenses.mkdir()
    for source in (ROOT / ".upstream/webmscore").glob("LICENSE*"):
        if source.is_file(): shutil.copy2(source, licenses / source.name)
    for directory in (ROOT / ".pixi/envs/default/conda-meta",):
        # Package metadata contains license/provenance, not user environment credentials.
        for source in directory.glob("qt6-main-*.json"):
            shutil.copy2(source, licenses / source.name)
    manifest = {str(p.relative_to(output)): {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                for p in output.rglob("*") if p.is_file()}
    (output / "manifest.json").write_text(json.dumps({"files": manifest, "total_bytes": sum(p["bytes"] for p in manifest.values()),
         "note": "Local validation bundle. Full third-party source/license distribution audit remains required before publishing."}, indent=2), encoding="utf-8")
    print(json.dumps({"output": str(output), "files": len(manifest), "bytes": sum(p["bytes"] for p in manifest.values())}))


if __name__ == "__main__":
    main()
