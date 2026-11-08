"""Shared native 470 helpers."""

from __future__ import annotations

import base64
import hashlib
import json
import shlex
import subprocess
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


class GateError(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_sha(path: Path) -> str:
    require(path.is_file(), f"missing file: {path}")
    return sha(path.read_bytes())


def read_json(path: Path):
    def reject(value):
        raise ValueError(f"non-finite JSON value: {value}")

    return json.loads(path.read_text(encoding="utf-8"), parse_constant=reject)


def save_json(path: Path, value) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def inside(root: Path, name: str) -> Path:
    result = (root / name).resolve()
    require(result.is_relative_to(root.resolve()), f"path escapes fixture directory: {name}")
    return result


def check_file(root: Path, case: dict, field: str) -> Path:
    path = inside(root, case[field])
    actual = file_sha(path)
    require(
        actual == case[field + "_sha256"],
        f"{case['id']}: {field} hash mismatch: {path}",
    )
    return path


def run_dump(
    binary: Path,
    assets: Path,
    source: Path,
    log_dir: Path,
    label: str,
    env: dict[str, str],
):
    if "__ssh" in env:
        code = (
            "import sys,subprocess,tempfile,json,base64,os\n"
            f"with tempfile.NamedTemporaryFile(suffix={source.suffix!r}) as f:\n"
            " f.write(sys.stdin.buffer.read()); f.flush()\n"
            f" p=subprocess.run([{str(binary)!r},{str(assets)!r},f.name],"
            f'env={{"LD_LIBRARY_PATH":{env["LD_LIBRARY_PATH"]!r}}},capture_output=True,timeout=60)\n'
            ' print(json.dumps({"exit":p.returncode,"stdout":base64.b64encode(p.stdout).decode(),'
            '"stderr":base64.b64encode(p.stderr).decode()}))\n'
        )
        response = remote_python(env["__ssh"], code, source.read_bytes())
        value = json.loads(response)
        proc = subprocess.CompletedProcess(
            ["ssh", env["__ssh"], str(binary), str(source)],
            value["exit"],
            base64.b64decode(value["stdout"]),
            base64.b64decode(value["stderr"]),
        )
    else:
        proc = subprocess.run(
            [str(binary), str(assets), str(source)],
            env=env,
            capture_output=True,
            timeout=60,
        )
    (log_dir / f"{label}.stdout").write_bytes(proc.stdout)
    (log_dir / f"{label}.stderr").write_bytes(proc.stderr)
    return proc


def remote_python(host: str, code: str, data: bytes = b"") -> bytes:
    command = [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        host,
        "python3 -c " + shlex.quote(code),
    ]
    proc = subprocess.run(command, input=data, capture_output=True, timeout=90)
    require(
        proc.returncode == 0,
        f"remote oracle command failed ({proc.returncode}): {shlex.join(command)}\n"
        + proc.stderr.decode("utf-8", errors="replace"),
    )
    return proc.stdout


def compare_semantics(expected, actual) -> None:
    def canonical(value):
        return json.dumps(
            value,
            sort_keys=True,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=False,
        )

    require(canonical(expected) == canonical(actual), "normalized semantics differ")


def compare_warnings(expected, actual) -> None:
    require(isinstance(actual, list), "warnings must be an array")

    def canon(obj):
        return json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False)

    require(
        sorted(map(canon, expected)) == sorted(map(canon, actual)),
        "warning identities, objects or messages differ",
    )


def xml_roots(path: Path):
    if path.suffix.lower() == ".mscz":
        with zipfile.ZipFile(path) as archive:
            members = [name for name in archive.namelist() if name.lower().endswith(".mscx")]
            require(members, f"MSCZ has no score XML: {path}")
            return [ET.fromstring(archive.read(name)) for name in members]
    return [ET.parse(path).getroot()]


def eid_associations(path: Path) -> dict:
    """Bind EIDs to owners and score/staff/measure/voice/object positions.

    Same-tag ordinals ignore unrelated serializer additions. Musical fields
    distinguish equal-type owners as well as the chord/note category swap.
    Lists retain duplicate EIDs so duplication cannot hide a moved identity.
    """
    result = {}
    fields = (
        "pitch",
        "tpc",
        "durationType",
        "duration",
        "sigN",
        "sigD",
        "concertClefType",
        "transposingClefType",
    )

    def visit(element, context):
        eid = element.findtext("eid")
        if eid:
            result.setdefault(eid, []).append(
                (
                    context,
                    tuple((key, element.findtext(key)) for key in fields if element.find(key) is not None),
                )
            )
        counts = {}
        for child in element:
            counts[child.tag] = counts.get(child.tag, 0) + 1
            visit(child, context + ((child.tag, counts[child.tag]),))

    for index, root in enumerate(xml_roots(path)):
        visit(root, ((root.tag, index),))
    return result


def compare_xml(path: Path, checks: dict, identity_source: Path | None = None) -> None:
    roots = xml_roots(path)
    require(
        all(root.tag == "museScore" and root.get("version") == "4.70" for root in roots),
        "output is not declared 4.70",
    )
    forbidden = {
        "SharedPart",
        "sharedPart",
        "StaveSharingLabel",
        "PageLocks",
        "pageLock",
        "InstrumentLabel",
        "customNameLongGroup",
        "customNameShortGroup",
        "useCustomGroupName",
    }
    for root in roots:
        for element in root.iter():
            require(
                element.tag not in forbidden,
                f"5.00-only element in output: {element.tag}",
            )
            if element.tag == "BracketItem":
                require(element.findtext("type") != "Group", "5.00 group bracket in output")
    if checks.get("eids"):
        eids = {element.text for root in roots for element in root.iter("eid")}
        require(set(checks["eids"]).issubset(eids), "expected EIDs missing from output XML")
        require(identity_source is not None, "EID association source is required")
        expected, actual = eid_associations(identity_source), eid_associations(path)
        for eid in checks["eids"]:
            require(
                eid in expected and actual.get(eid) == expected[eid],
                f"EID association differs for {eid}: expected {expected.get(eid)!r}, got {actual.get(eid)!r}",
            )
    if checks.get("ordinary_bracket"):
        b = checks["ordinary_bracket"]
        found = False
        for root in roots:
            for part in root.iter("Part"):
                for staff in part.findall("Staff"):
                    if staff.findtext("eid") != b["staff_eid"]:
                        continue
                    for bracket in staff.findall("bracket"):
                        if (
                            bracket.get("type") == "1"
                            and bracket.get("span") == str(b["span"])
                            and bracket.get("col", "0") == str(b["column"])
                        ):
                            found = True
        require(found, "expected ordinary 4.70 brace missing from output XML")
