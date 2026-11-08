"""Check that executable tests are never imported as shared helpers."""

import ast
from pathlib import Path


def audit_test_layout(root: Path) -> list[str]:
    errors = []
    for repo in (root, root / "vendor/engrave"):
        if not (repo / "tests").is_dir():
            continue
        for path in (repo / "tests/integration").rglob("__init__.py"):
            errors.append(f"{path}: integration scripts must not be a package")
        folders = {repo / name for name in ("tests", "scripts", "tools", "engrave/tests")}
        folders.update((repo / "src").rglob("tests"))
        paths = {path for folder in folders for path in folder.rglob("*.py")}
        for path in sorted(paths):
            if "__pycache__" in path.parts:
                continue
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"), filename=str(path))):
                names = []
                if isinstance(node, ast.Import):
                    names = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom):
                    names = [node.module or "", *(alias.name for alias in node.names)]
                if any(part.startswith("test_") for name in names for part in name.split(".")):
                    errors.append(f"{path}:{node.lineno}: import shared helpers from the testing package")
    return errors
