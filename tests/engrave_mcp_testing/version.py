"""Read the host version declarations used by the build and CLI checks."""

import ast
import re
import tomllib
from pathlib import Path

_CPP_TOKENS = re.compile(
    r"(?P<comment>//[^\n]*|/\*.*?\*/)"
    r'|(?:u8|[uUL])?R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)"'
    r"""|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\w+|[^\s]""",
    re.DOTALL,
)


def _header_version(text: str) -> str | None:
    tokens = [match[0] for match in _CPP_TOKENS.finditer(text) if match.lastgroup != "comment"]
    for name, assign, value, end in zip(tokens, tokens[1:], tokens[2:], tokens[3:]):
        if (name, assign, end) == ("host_version", "=", ";") and value.startswith('"'):
            return ast.literal_eval(value)
    return None


def _module_version(text: str) -> str | None:
    for statement in ast.parse(text).body:
        if isinstance(statement, ast.Expr) and isinstance(statement.value, ast.Call):
            call = statement.value
            if isinstance(call.func, ast.Name) and call.func.id == "module":
                for keyword in call.keywords:
                    if keyword.arg == "version" and isinstance(keyword.value, ast.Constant):
                        return keyword.value.value
    return None


def _workspace_version(text: str) -> str | None:
    manifest = tomllib.loads(text)
    workspace = manifest.get("workspace") or manifest.get("project") or {}
    return workspace.get("version")


def read_host_version(root: Path) -> str:
    versions = {}
    for name, parse in (
        ("engrave_mcp/version.h", _header_version),
        ("MODULE.bazel", _module_version),
        ("pixi.toml", _workspace_version),
    ):
        try:
            version = parse((root / name).read_text(encoding="utf-8"))
        except (SyntaxError, ValueError) as error:
            raise ValueError(f"could not parse version from {name}: {error}") from error
        if not isinstance(version, str) or not version:
            raise ValueError(f"could not parse version from {name}")
        versions[name] = version

    if len(set(versions.values())) != 1:
        details = ", ".join(f"{name}={value!r}" for name, value in versions.items())
        raise ValueError(f"host version mismatch: {details}")
    return versions["engrave_mcp/version.h"]
