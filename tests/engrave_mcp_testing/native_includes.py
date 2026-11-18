"""Shared native includes helpers."""

from __future__ import annotations

import re
from pathlib import Path, PurePosixPath

DIRECTIVE = re.compile(r"^\s*#\s*include\b(.*)$")


LITERAL = re.compile(r'^\s*([<"])([^">]+)([">])')


SYSTEM_HEADERS = frozenset(
    [
        "algorithm",
        "any",
        "array",
        "atomic",
        "barrier",
        "bit",
        "bitset",
        "cassert",
        "cctype",
        "cerrno",
        "cfenv",
        "cfloat",
        "charconv",
        "chrono",
        "cinttypes",
        "ciso646",
        "climits",
        "clocale",
        "cmath",
        "codecvt",
        "compare",
        "complex",
        "concepts",
        "condition_variable",
        "coroutine",
        "csetjmp",
        "csignal",
        "cstdarg",
        "cstddef",
        "cstdint",
        "cstdio",
        "cstdlib",
        "cstring",
        "ctgmath",
        "ctime",
        "cuchar",
        "cwchar",
        "cwctype",
        "deque",
        "exception",
        "execution",
        "expected",
        "filesystem",
        "format",
        "forward_list",
        "fstream",
        "functional",
        "future",
        "generator",
        "initializer_list",
        "iomanip",
        "ios",
        "iosfwd",
        "iostream",
        "istream",
        "iterator",
        "latch",
        "limits",
        "list",
        "locale",
        "mdspan",
        "memory",
        "memory_resource",
        "mutex",
        "new",
        "numbers",
        "numeric",
        "optional",
        "ostream",
        "print",
        "queue",
        "random",
        "ranges",
        "ratio",
        "regex",
        "scoped_allocator",
        "semaphore",
        "shared_mutex",
        "source_location",
        "span",
        "spanstream",
        "sstream",
        "stack",
        "stacktrace",
        "stdexcept",
        "stdfloat",
        "stop_token",
        "streambuf",
        "string",
        "string_view",
        "strstream",
        "syncstream",
        "system_error",
        "thread",
        "tuple",
        "typeindex",
        "typeinfo",
        "type_traits",
        "utility",
        "valarray",
        "variant",
        "vector",
        "version",
        "Windows.h",
        "windows.h",
        "fcntl.h",
        "io.h",
        "poll.h",
        "signal.h",
        "spawn.h",
        "unistd.h",
        "sys/prctl.h",
        "sys/wait.h",
        "sys/stat.h",
        "aclapi.h",
        "sddl.h",
    ]
)


def audit(native: Path, public: Path) -> list[str]:
    errors = []
    extensions = {
        ".h",
        ".hh",
        ".hpp",
        ".hxx",
        ".inl",
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".m",
        ".mm",
        ".ixx",
        ".cppm",
    }
    for source in sorted(p for p in native.rglob("*") if p.suffix in extensions):
        for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            directive = DIRECTIVE.match(line)
            if not directive:
                continue
            literal = LITERAL.match(directive.group(1))
            if not literal or (literal[1], literal[3]) not in {("<", ">"), ('"', '"')}:
                errors.append(f"{source}:{number}: include must name a literal header")
                continue
            delimiter, name = literal[1], literal[2]
            parts = PurePosixPath(name).parts
            safe = (
                bool(parts)
                and "\\" not in name
                and ":" not in name
                and not name.startswith("/")
                and all(p not in {".", ".."} for p in parts)
            )
            allowed = False
            if safe and name.startswith("engrave/"):
                candidate = (public / name).resolve()
                allowed = candidate.is_relative_to(public.resolve()) and candidate.is_file()
            elif safe and delimiter == '"':
                candidate = (native / name).resolve()
                allowed = candidate.is_relative_to(native.resolve()) and candidate.is_file()
            elif safe:
                allowed = (
                    name in SYSTEM_HEADERS
                    or name == "asio.hpp"
                    or name.startswith("asio/")
                    or name.startswith("glaze/")
                    or name.startswith("proxy/")
                    or name.startswith("ankerl/")
                    or name.startswith("amc/")
                )
            if not allowed:
                errors.append(f"{source}:{number}: forbidden or unknown include {name!r}")
    return errors
