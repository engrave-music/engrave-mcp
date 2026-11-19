#!/usr/bin/env python3
"""Host entry point for the fork-owned engine gate.

By default the gate runs on the host's profile build: engine_tests is built with
scripts/bbuild.py into build/<profile>, which ordinary host work keeps warm, and
the gate gets --skip-build --build-dir. Passing --build-dir, or an --engine-source
other than the submodule, selects the standalone fork build while sharing this
checkout's Bazel repository and action caches unless overridden.

Unless --pixi-manifest is given, the gate uses this project's Pixi environment
(pixi.lock), the cairo and zlib builds the goldens were made with.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

if __name__ == "__main__":
    args = sys.argv[1:]
    project = Path(__file__).resolve().parents[1]
    submodule = (project / "vendor/engrave").resolve()
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--engine-source", type=Path, default=submodule)
    parser.add_argument("--pixi-manifest", type=Path)
    parser.add_argument("--repo-cache", type=Path)
    parser.add_argument("--disk-cache", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--profile", default="release")
    parser.add_argument("--skip-build", action="store_true")
    parsed, _ = parser.parse_known_args(args)
    root = parsed.engine_source.resolve()
    prefix = []
    if parsed.pixi_manifest is None:
        prefix += ["--pixi-manifest", str(project / "pixi.toml")]
    if parsed.build_dir is None and root == submodule:
        if not parsed.skip_build:
            built = subprocess.call(
                [
                    sys.executable,
                    str(project / "scripts/bbuild.py"),
                    "--profile",
                    parsed.profile,
                    "build",
                    "engine_tests",
                ],
                cwd=project,
            )
            if built:
                sys.exit(built)
            prefix.append("--skip-build")
        prefix += ["--build-dir", str(project / "build" / parsed.profile)]
    else:
        if parsed.repo_cache is None:
            prefix += ["--repo-cache", str(project / "build/.bazel_repo_cache")]
        if parsed.disk_cache is None:
            prefix += ["--disk-cache", str(project / "build/.bazel_disk_cache")]
    args = [*prefix, *args]
    os.environ["PYTHONPATH"] = os.pathsep.join(
        (str(root / "tests"), str(root / "tools"), os.environ.get("PYTHONPATH", ""))
    )
    sys.exit(subprocess.call([sys.executable, str(root / "tests/gate.py"), *args]))
