"""Direct ZIP decompression checks plus optional valid MXL/MSCZ/GP MCP imports."""
from __future__ import annotations
import argparse
import asyncio
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def raw_deflate(data):
    encoder = zlib.compressobj(wbits=-15)
    return encoder.compress(data) + encoder.flush()


def archive(data=b"archive fixture\n", *, method=8, descriptor=False, descriptor_signature=True, **changes):
    """Two entries ensure a damaged second local header still reaches fileData()."""
    entries = []
    local_bytes = bytearray()
    for name, payload, target in [(b"prefix", b"", False), (b"payload", data, True)]:
        current_method = method if target else 0
        compressed = raw_deflate(payload) if current_method == 8 else payload
        crc = zlib.crc32(payload)
        flags = 8 if descriptor and target else 0
        values = {
            "signature": 0x04034B50, "version": 20, "flags": flags,
            "method": current_method, "crc": crc, "compressed_size": len(compressed),
            "uncompressed_size": len(payload), "name_length": len(name), "extra_length": 0,
        }
        local = dict(values)
        if flags & 8:
            local.update(crc=0, compressed_size=0, uncompressed_size=0)
        central = dict(values, offset=len(local_bytes))
        if target:
            for field, value in changes.items():
                if field.startswith("local_"):
                    local[field[6:]] = value
                elif field.startswith("central_"):
                    central[field[8:]] = value
            compressed = changes.get("raw_compressed", compressed)
        local_bytes += struct.pack("<IHHHHHIIIHH", local["signature"], local["version"], local["flags"],
                                   local["method"], 0, 0, local["crc"], local["compressed_size"],
                                   local["uncompressed_size"], local["name_length"], local["extra_length"])
        local_bytes += name + compressed
        if flags & 8:
            if descriptor_signature:
                local_bytes += struct.pack("<I", 0x08074B50)
            local_bytes += struct.pack("<III", crc, values["compressed_size"], len(payload))
        entries.append((name, central))
    directory = bytearray()
    for name, central in entries:
        directory += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, central["version"],
                                 central["flags"], central["method"], 0, 0, central["crc"],
                                 central["compressed_size"], central["uncompressed_size"], len(name),
                                 0, 0, 0, 0, 0, central["offset"])
        directory += name
    return bytes(local_bytes + directory + struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 2, 2,
                                                      len(directory), len(local_bytes), 0))


def cases():
    data = b"bounded decompression\n" * 200
    compressed = raw_deflate(data)
    valid = {
        "stored": (archive(data, method=0), data),
        "deflated": (archive(data), data),
        "empty-stored": (archive(b"", method=0), b""),
        "empty-deflated": (archive(b""), b""),
        "descriptor-signature": (archive(data, descriptor=True), data),
        "descriptor-no-signature": (archive(data, descriptor=True, descriptor_signature=False), data),
        "stored-descriptor": (archive(data, method=0, descriptor=True), data),
    }
    invalid = {
        "underreported-output": archive(data, local_uncompressed_size=1, central_uncompressed_size=1),
        "overreported-output": archive(data, local_uncompressed_size=len(data)+1, central_uncompressed_size=len(data)+1),
        "declared-output-cap": archive(data, local_uncompressed_size=64*1024*1024+1, central_uncompressed_size=64*1024*1024+1),
        "uint32-output": archive(data, local_uncompressed_size=0xFFFFFFFF, central_uncompressed_size=0xFFFFFFFF),
        "uint32-input": archive(data, local_compressed_size=0xFFFFFFFF, central_compressed_size=0xFFFFFFFF),
        "bad-crc": archive(data, local_crc=0, central_crc=0),
        "local-crc-mismatch": archive(data, local_crc=0),
        "stored-size-mismatch": archive(data, method=0, local_uncompressed_size=len(data)-1, central_uncompressed_size=len(data)-1),
        "method-mismatch": archive(data, central_method=0),
        "unsupported-method": archive(data, local_method=9, central_method=9),
        "bad-local-signature": archive(data, local_signature=0x02014B50),
        "truncated-deflate": archive(data, raw_compressed=compressed[:-1],
                                     local_compressed_size=len(compressed)-1, central_compressed_size=len(compressed)-1),
        "trailing-deflate": archive(data, raw_compressed=compressed+b"junk",
                                    local_compressed_size=len(compressed)+4, central_compressed_size=len(compressed)+4),
        "bad-local-offset": archive(data, central_offset=0xFFFFFFFF),
        "bad-local-name-length": archive(data, local_name_length=65535),
        "bad-local-extra-length": archive(data, local_extra_length=65535),
        "central-encryption": archive(data, central_flags=1),
        "local-encryption": archive(data, local_flags=1),
        "strong-encryption": archive(data, local_flags=0x40, central_flags=0x40),
        "directory-encryption": archive(data, local_flags=0x2000, central_flags=0x2000),
        "descriptor-flag-mismatch": archive(data, local_flags=8),
        "unsupported-central-version": archive(data, central_version=45),
        "unsupported-local-version": archive(data, local_version=45),
    }
    return valid, invalid


def run_probe(binary, out):
    valid, invalid = cases()
    report = {"valid": [], "rejected": []}
    for name, packed, expected in [(n, p, e) for n, (p, e) in valid.items()] + [(n, p, None) for n,p in invalid.items()]:
        source, result = out/f"{name}.zip", out/f"{name}.bin"
        source.write_bytes(packed)
        started = time.monotonic()
        run = subprocess.run([str(binary), str(source), "payload", str(result)],
                             capture_output=True, text=True, encoding="utf-8", timeout=10)
        (out/f"{name}.stderr.log").write_text(run.stderr, encoding="utf-8")
        assert run.returncode == 0, (name, run.returncode, run.stderr)
        response = json.loads(run.stdout.strip().splitlines()[-1])
        assert response["exists"], (name, response)
        if expected is None:
            assert response["error"] and response["size"] == 0 and result.read_bytes() == b"", (name, response)
            report["rejected"].append({"name":name,"seconds":round(time.monotonic()-started,4)})
        else:
            assert not response["error"] and result.read_bytes() == expected, (name, response)
            with zipfile.ZipFile(io.BytesIO(packed)) as check:
                assert check.read("payload") == expected
            report["valid"].append(name)
    return report


async def valid_score_imports(binary, resources, out):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    xml = (ROOT/"fixtures/evening.musicxml").read_bytes()
    with zipfile.ZipFile(out/"valid.mxl", "w", compression=zipfile.ZIP_DEFLATED) as target:
        target.writestr("META-INF/container.xml", "<container><rootfiles><rootfile full-path='score.xml'/></rootfiles></container>")
        target.writestr("score.xml", xml)
    params = StdioServerParameters(command=str(binary), args=["--resources",str(resources)], env=dict(os.environ))
    result=[]
    with (out/"mcp.stderr.log").open("w",encoding="utf-8") as errors:
        async with stdio_client(params, errlog=errors) as (read, write):
            async with ClientSession(read,write) as session:
                await session.initialize()
                opened=await session.call_tool("score_open", {"path":str(out/"valid.mxl")})
                assert not opened.is_error, opened.content
                document=opened.structured_content["documentId"]
                exported=await session.call_tool("score_export", {"documentId":document,"format":"mscz","path":str(out/"valid.mscz")})
                assert not exported.is_error, exported.content
                await session.call_tool("score_close", {"documentId":document})
                result.append("mxl")
                for path, label in [(out/"valid.mscz","mscz"),(ROOT/".upstream/webmscore/src/importexport/guitarpro/tests/data/sforzato.gp","gp")]:
                    assert path.is_file(), path
                    opened=await session.call_tool("score_open", {"path":str(path)})
                    assert not opened.is_error, (label,opened.content)
                    await session.call_tool("score_close", {"documentId":opened.structured_content["documentId"]})
                    result.append(label)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary",type=Path,required=True,help="archive_test native probe")
    parser.add_argument("--server",type=Path,help="Optional MCP executable for valid score formats")
    parser.add_argument("--resources",type=Path,default=ROOT/".runtime/assets")
    parser.add_argument("--output",type=Path,default=ROOT/"artifacts/archive-bounds")
    args=parser.parse_args()
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=True)
    report=run_probe(args.binary.resolve(),out)
    if args.server:
        report["valid_score_imports"]=asyncio.run(valid_score_imports(args.server.resolve(),args.resources.resolve(),out))
    (out/"evidence.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
