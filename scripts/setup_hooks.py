#!/usr/bin/env python3
"""Install project git hooks by symlinking hooks/ into .git/hooks/.

Usage:
    pixi run setup-hooks
"""

import os
import shutil
import subprocess
import sys


def main():
    repo_root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()

    src_dir = os.path.join(repo_root, "hooks")
    dst_dir = subprocess.check_output(
        ["git", "rev-parse", "--path-format=absolute", "--git-path", "hooks"], text=True
    ).strip()

    if not os.path.isdir(src_dir):
        print(f"error: {src_dir} not found", file=sys.stderr)
        return 1

    hooks = [f for f in os.listdir(src_dir) if not f.startswith(".") and f != "engine-pre-commit"]
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

    engine = os.path.join(repo_root, "vendor", "engrave")
    engine_hooks = subprocess.check_output(
        ["git", "-C", engine, "rev-parse", "--path-format=absolute", "--git-path", "hooks"], text=True
    ).strip()
    os.makedirs(engine_hooks, exist_ok=True)
    target = os.path.join(engine_hooks, "pre-commit")
    if os.path.lexists(target):
        os.remove(target)
    shutil.copy2(os.path.join(src_dir, "engine-pre-commit"), target)
    os.chmod(target, 0o755)
    print(f"Installed engine pre-commit: {target}")
    print(f"\nInstalled {len(hooks)} host hook(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
