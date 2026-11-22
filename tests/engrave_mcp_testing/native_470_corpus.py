"""Compare host 4.70 exports with current-format semantics in the pinned reader."""

from __future__ import annotations

import json
import os
from decimal import Decimal
from pathlib import Path

from engrave_mcp_testing.golden import fixture_files, mnx_fixture_files, open_fixture, required, verify_input_provenance
from engrave_mcp_testing.native_470 import (
    compare_semantics,
    compare_xml,
    file_sha,
    require,
    run_dump,
    save_json,
    xml_roots,
)

GOLDEN_COUNT = 26
BLANK_PIANO = "blank_grand_staff_piano"
FERMATA_RESTORE = {
    "layout_elements.mscx": (2, 7680),
    "repeat70.mscx": (1, 3840),
    "repeat71.mscx": (1, 3840),
    "repeat72.mscx": (1, 3840),
    "repeat_with_2_voltas.mscx": (1, 3840),
}


def read_semantics(data):
    def number(token):
        value = Decimal(token)
        return int(value) if value == value.to_integral_value() else float(value)

    return json.loads(data, parse_float=number)


def check_blank_piano(path, actual):
    parts = actual["parts"]
    require(len(parts) == 1 and parts[0]["instrumentId"] == "piano", "wrong blank piano part")
    require(len(parts[0]["staves"]) == len(actual["events"]) == 2, "missing grand staff")
    for staff in actual["events"]:
        require(
            len(staff[0]) == len(actual["measures"]) > 0
            and all(event["kind"] == "rest" and event["duration"] == "measure" for event in staff[0])
            and not any(staff[1:]),
            "blank piano must have one full-measure rest per staff and measure",
        )
    score = xml_roots(path)[0].find("Score")
    staves = score.findall("./Part/Staff")
    require(len(staves) == 2 and staves[1].findtext("defaultClef") == "F", "missing bass staff clef")
    require(staves[0].find("./bracket[@type='1'][@span='2']") is not None, "missing piano brace")


async def corpus_parity(args, session, output, binary, assets, oracle_env):
    fixtures = fixture_files() + mnx_fixture_files()
    sources = {path.name: path for path in fixtures}
    require(len(fixtures) == len(sources) == GOLDEN_COUNT, f"expected {GOLDEN_COUNT} unique golden fixtures")
    provenance_issues = verify_input_provenance(sources=fixtures)
    require(not provenance_issues, f"unreviewed corpus inputs: {provenance_issues}")
    current = args.current_binary or args.host_binary.with_name(
        "semantic_dump.exe" if os.name == "nt" else "semantic_dump"
    )
    current_assets = args.current_assets or args.host_resources
    records = []
    for name, source in [*sources.items(), (BLANK_PIANO, None)]:
        dest = output / f"corpus_{name}"
        dest.mkdir()
        record = {"case": dest.name, "formats": {}, "errors": []}
        if source is not None:
            record["source_sha256"] = file_sha(source)
        doc = None
        try:
            opened = (
                await open_fixture(session, source)
                if source is not None
                else await required(session, "score_create", {"template_id": "Piano"})
            )
            doc = opened["document"]

            async def export(path: Path, version: str):
                result = await required(
                    session,
                    "score_export",
                    {
                        "document": doc,
                        "expected_revision": opened["revision"],
                        "format": path.suffix[1:],
                        "path": str(path),
                        "options": {"native_version": version},
                    },
                )
                require(result["source_revision"] == opened["revision"], "exported the wrong revision")
                require(result["export_warnings"] == [], f"unexpected export warnings: {result['export_warnings']}")
                require(path.is_file() and len(result["artifacts"]) == 1, "missing export artifact")

            reference = dest / "current.mscx"
            await export(reference, "5.00")
            proc = run_dump(current.resolve(), current_assets.resolve(), reference, dest, "current", os.environ.copy())
            require(proc.returncode == 0 and not proc.stderr, "current reader failed or reported diagnostics")
            expected = read_semantics(proc.stdout)
            if name in FERMATA_RESTORE:
                index, tick = FERMATA_RESTORE[name]
                require(expected["tempoMap"][index][0] == tick, "fermata restoration baseline changed")
                expected["tempoMap"][index][0] = tick - 1
            for fmt in ("mscx", "mscz"):
                path = dest / f"score.{fmt}"
                result = record["formats"][fmt] = {}
                try:
                    await export(path, "4.70")
                    compare_xml(path, {})
                    result["output_sha256"] = file_sha(path)
                    proc = run_dump(binary, assets, path, dest, f"oracle-{fmt}", oracle_env)
                    result["oracle_exit"] = proc.returncode
                    diagnostics = proc.stderr.splitlines()
                    result["oracle_diagnostics"] = len(diagnostics)
                    require(proc.returncode == 0, f"oracle exited {proc.returncode}")
                    require(not diagnostics, f"unexpected oracle diagnostics: {diagnostics}")
                    actual = read_semantics(proc.stdout)
                    compare_semantics(expected, actual)
                    if name == BLANK_PIANO:
                        check_blank_piano(path, actual)
                    result["oracle_matched"] = True
                except Exception as error:
                    record["errors"].append(f"{fmt}: {error}")
        except Exception as error:
            record["errors"].append(str(error))
        finally:
            if doc is not None:
                await required(session, "score_close", {"document": doc})
        record["status"] = "failed" if record["errors"] else "passed"
        save_json(dest / "result.json", record)
        print(f"{record['status'].upper()} {dest.name}", flush=True)
        records.append(record)
    return records
