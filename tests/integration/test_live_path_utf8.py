"""Client-visible UTF-8 export paths, symlink resolution and partial publication."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


import argparse
import json
import os
from pathlib import Path

from support.process_client import ROOT, Client, export_args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    a = parser.parse_args()
    a.output = a.output.resolve()
    c = Client(a.binary, a.resources, a.output, None)
    results = {}
    try:
        doc = c.tool("score_open", {"path": str(ROOT / "tests/data/inputs/golden/repeat70.mscx")})["document"]
        measure = c.tool("score_outline", {"document": doc})["outline"]["measures"][0]["id"]
        c.tool(
            "score_edit",
            dict(
                document=doc,
                ops=[
                    dict(
                        op="set_layout_break",
                        measure=dict(kind="id", id=measure),
                        kind="page",
                        on=True,
                    )
                ],
            ),
        )

        def export(path, ok=True, fmt="svg"):
            args = export_args(doc, fmt, path, True)
            # The helper resolves paths: override with the literal UTF-8 alias.
            args["destination"]["path"] = str(path)
            return c.tool("score_export", args, ok)

        if os.name == "posix":
            for partial in (False, True):
                target = a.output / os.fsdecode(b"invalid_\xff_" + str(partial).encode())
                target.mkdir()
                alias = a.output / f"invalid-alias-{partial}"
                alias.symlink_to(target, target_is_directory=True)
                if partial:
                    (target / "page-0002.svg").mkdir()
                before = sorted(os.listdir(os.fsencode(target)))
                # Both directory and single-file destination resolution reject.
                for fmt, dest in [("svg", alias), ("pdf", alias / "out.pdf")]:
                    error = export(dest, False, fmt)["error"]
                    assert error["code"] == "InvalidArgument", error
                    assert str(dest) in error["message"] and "UTF-8" in error["message"], error
                    assert not error.get("published_artifacts"), error
                    assert sorted(os.listdir(os.fsencode(target))) == before
                for fmt, dest in [
                    ("svg", alias / "missing" / "nested"),
                    ("pdf", alias / "missing" / "nested" / "out.pdf"),
                ]:
                    error = export(dest, False, fmt)["error"]
                    assert error["code"] == "InvalidArgument" and not error.get("published_artifacts"), error
                    assert sorted(os.listdir(os.fsencode(target))) == before
                results[f"invalid_target_partial_{partial}"] = "passed (SVG and PDF)"
        elif os.name == "nt":
            # Python's Windows filesystem calls use Win32 wide-character APIs.
            # Extended paths let us create real, reachable unpaired surrogates;
            # the request itself contains only well-formed Unicode.
            for surrogate in ("\ud800", "\udcff"):
                target = Path("\\\\?\\" + str(a.output / ("unpaired_" + surrogate)))
                target.mkdir()
                alias = a.output / f"invalid-alias-音楽-😀-{ord(surrogate):04x}"
                try:
                    alias.symlink_to(target, target_is_directory=True)
                    assert alias.is_dir() and os.path.samefile(alias, target)
                    before = sorted(os.listdir(a.output))
                    errors = {}
                    for fmt, dest in [("svg", alias), ("pdf", alias / "out.pdf")]:
                        error = export(dest, False, fmt)["error"]
                        assert error["code"] == "InvalidArgument", error
                        assert str(dest) in error["message"] and "UTF-8" in error["message"], error
                        assert not error.get("published_artifacts"), error
                        assert os.listdir(target) == [], os.listdir(target)
                        assert sorted(os.listdir(a.output)) == before
                        errors[fmt] = error
                    for fmt, dest in [
                        ("svg", alias / "missing" / "nested"),
                        ("pdf", alias / "missing" / "nested" / "out.pdf"),
                    ]:
                        error = export(dest, False, fmt)["error"]
                        assert error["code"] == "InvalidArgument" and not error.get("published_artifacts"), error
                        assert os.listdir(target) == [], os.listdir(target)
                    results[f"unpaired_{ord(surrogate):04x}"] = dict(
                        result="passed",
                        reachable=True,
                        errors=errors,
                        no_artifacts_or_staging=True,
                    )
                finally:
                    if alias.is_symlink():
                        alias.unlink()
                    target.rmdir()
        else:
            results["invalid_target"] = "skipped: POSIX byte filenames only"

        for partial in (False, True):
            target = a.output / f"valid_音楽_é_😀_{partial}"
            target.mkdir()
            alias = a.output / f"valid-alias-音楽-😀-{partial}"
            try:
                alias.symlink_to(target, target_is_directory=True)
                route = "symlink"
            except OSError:
                if os.name != "nt":
                    raise
                alias = target
                route = "direct (Windows symlink privilege unavailable)"
            if partial:
                (target / "page-0002.svg").mkdir()
            result = export(alias, not partial)
            if partial:
                assert result["error"]["code"] == "Io", result
                paths = result["error"]["published_artifacts"]
                assert paths == [(target / "page-0001.svg").as_posix()], result
            else:
                paths = [item["path"] for item in result["artifacts"]]
                assert paths == [(target / f"page-{n:04}.svg").as_posix() for n in (1, 2)], result
            assert all(Path(path).is_file() and Path(path).stat().st_size > 0 for path in paths)
            assert not list(target.glob(".engrave-stage-*"))
            results[f"valid_target_partial_{partial}"] = dict(result="passed", route=route, paths=paths)
        assert "tools" in c.rpc("tools/list", {})["result"]
    finally:
        c.close()
    (a.output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results, ensure_ascii=True))


if __name__ == "__main__":
    main()
