#!/usr/bin/env python3
"""Enforce public engine includes in every native host and test source."""
from __future__ import annotations

import argparse
from pathlib import Path, PurePosixPath
import re

ROOT = Path(__file__).resolve().parents[1]
DIRECTIVE = re.compile(r'^\s*#\s*include\b(.*)$')
LITERAL = re.compile(r'^\s*([<"])([^">]+)([">])')

# Standard C/C++ and platform headers. Other dependencies have explicit
# prefixes below, so an unqualified engine header cannot slip through.
SYSTEM_HEADERS = frozenset("""
algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat
charconv chrono cinttypes ciso646 climits clocale cmath codecvt compare complex
concepts condition_variable coroutine csetjmp csignal cstdarg cstddef cstdint
cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype deque exception
execution expected filesystem format forward_list fstream functional future
generator initializer_list iomanip ios iosfwd iostream istream iterator latch
limits list locale map mdspan memory memory_resource mutex new numbers numeric
optional ostream print queue random ranges ratio regex scoped_allocator semaphore
set shared_mutex source_location span spanstream sstream stack stacktrace stdexcept
stdfloat stop_token streambuf string string_view strstream syncstream system_error
thread tuple typeindex typeinfo type_traits unordered_map unordered_set utility
valarray variant vector version
Windows.h windows.h fcntl.h io.h poll.h signal.h spawn.h unistd.h sys/prctl.h
sys/wait.h sys/stat.h aclapi.h sddl.h
""".split())


def audit(native: Path, public: Path) -> list[str]:
    errors = []
    extensions = {".h", ".hh", ".hpp", ".hxx", ".inl", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".ixx", ".cppm"}
    for source in sorted(p for p in native.rglob("*") if p.suffix in extensions):
        for number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            directive = DIRECTIVE.match(line)
            if not directive:
                continue
            literal = LITERAL.match(directive.group(1))
            if not literal or (literal[1], literal[3]) not in {('<', '>'), ('"', '"')}:
                errors.append(f"{source}:{number}: include must name a literal header")
                continue
            delimiter, name = literal[1], literal[2]
            parts = PurePosixPath(name).parts
            safe = bool(parts) and "\\" not in name and ":" not in name and not name.startswith("/") and all(p not in {".", ".."} for p in parts)
            allowed = False
            if safe and name.startswith("engrave/"):
                candidate = (public / name).resolve()
                allowed = candidate.is_relative_to(public.resolve()) and candidate.is_file()
            elif safe and delimiter == '"':
                candidate = (native / name).resolve()
                allowed = candidate.is_relative_to(native.resolve()) and candidate.is_file()
            elif safe:
                allowed = name in SYSTEM_HEADERS or name == "asio.hpp" or name.startswith("asio/") or name == "nlohmann/json.hpp"
            if not allowed:
                errors.append(f"{source}:{number}: forbidden or unknown include {name!r}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine-source", type=Path, default=ROOT / "vendor/engrave")
    args = parser.parse_args()
    native = ROOT / "native"
    public = args.engine_source / "engrave/include"
    if not public.is_dir():
        parser.error(f"engine public include directory does not exist: {public}")
    errors = audit(native, public)
    if errors:
        print("\n".join(errors))
        return 1
    print(f"native includes: public engine headers only ({native})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
