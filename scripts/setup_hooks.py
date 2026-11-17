#!/usr/bin/env python3
"""Install project git hooks by symlinking hooks/ into .git/hooks/.

Usage:
    pixi run setup-hooks
"""

import argparse
import os
import shutil
import subprocess
import sys


def main():
    repo_root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()

    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", default=os.path.join(repo_root, "vendor", "engrave"))
    engine = os.path.realpath(parser.parse_args().engine_root)
    top = subprocess.check_output(["git", "-C", engine, "rev-parse", "--show-toplevel"], text=True).strip()
    if os.path.normcase(os.path.realpath(top)) != os.path.normcase(engine) or os.path.normcase(
        engine
    ) == os.path.normcase(os.path.realpath(repo_root)):
        print(f"error: not an initialized engine checkout: {engine}", file=sys.stderr)
        return 1

    src_dir = os.path.join(repo_root, "hooks")
    dst_dir = subprocess.check_output(
        ["git", "rev-parse", "--path-format=absolute", "--git-path", "hooks"], text=True
    ).strip()

    if not os.path.isdir(src_dir):
        print(f"error: {src_dir} not found", file=sys.stderr)
        return 1

    hooks = [f for f in os.listdir(src_dir) if not f.startswith(".")]
    if not hooks:
        print("No hooks to install.")
        return 0

    os.makedirs(dst_dir, exist_ok=True)

    for name in hooks:
        src = os.path.join(src_dir, name)
        dst = os.path.join(dst_dir, name)

        # Remove existing hook (file or symlink) so we can replace it.
        if os.path.lexists(dst):
            os.remove(dst)

        # Use a relative symlink so the repo stays relocatable.
        rel = os.path.relpath(src, dst_dir)
        try:
            os.symlink(rel, dst)
            print(f"  {name} -> {rel} (symlink)")
        except OSError:
            # Fallback to copy if symlink fails (e.g. without dev mode permissions)
            shutil.copy2(src, dst)
            print(f"  {name} -> {dst} (copied)")

    engine_hooks = subprocess.check_output(
        ["git", "-C", engine, "rev-parse", "--path-format=absolute", "--git-path", "hooks"], text=True
    ).strip()
    os.makedirs(engine_hooks, exist_ok=True)
    target = os.path.join(engine_hooks, "pre-commit")
    if os.path.lexists(target):
        os.remove(target)
    with open(os.path.join(engine, "tools", "pre-commit"), encoding="utf-8") as source:
        content = source.read()
    with open(target, "w", encoding="utf-8", newline="\n") as destination:
        destination.write(content)
    os.chmod(target, 0o755)
    print(f"Installed engine pre-commit: {target}")
    print(f"\nInstalled {len(hooks)} host hook(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
