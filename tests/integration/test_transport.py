import json
import os
import subprocess
import sys
import time
from pathlib import Path

from engrave_mcp_testing.paths import REPO_ROOT
from engrave_mcp_testing.process_client import Client

PROTOCOL_VERSION = "2025-11-25"
# The advertised tool list must match the checked-in schema exactly.
EXPECTED_TOOLS = [
    tool["name"] for tool in json.loads((REPO_ROOT / "engrave_mcp/tools.json").read_text(encoding="utf-8"))
]
EXPECTED_RESPONSE_IDS = [None, None, None, None, None, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
EXPECTED_RESPONSE_COUNT = 15
TIMEOUT_SECONDS = 45
OVERSIZED_LINE_BYTES = 4 * 1024 * 1024 + 10
DEFAULT_VALIDATION_PATH = Path("artifacts") / "transport-validation.json"


def _as_text(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def _compact(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _response_text(response: dict) -> str:
    return json.dumps(response, ensure_ascii=False, sort_keys=True)


def _run_server(exe: str, payload: str) -> dict:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            [exe]
            + (["--resources", os.environ["ENGRAVE_MCP_RESOURCES"]] if os.environ.get("ENGRAVE_MCP_RESOURCES") else []),
            input=payload.encode("utf-8"),
            timeout=TIMEOUT_SECONDS,
            capture_output=True,
        )
        return {
            "returncode": completed.returncode,
            "stdout": _as_text(completed.stdout),
            "stderr": _as_text(completed.stderr),
            "timed_out": False,
            "run_error": None,
            "duration_ms": int((time.monotonic() - started) * 1000),
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "returncode": None,
            "stdout": _as_text(exc.stdout),
            "stderr": _as_text(exc.stderr),
            "timed_out": True,
            "run_error": f"timeout after {TIMEOUT_SECONDS} seconds",
            "duration_ms": int((time.monotonic() - started) * 1000),
        }
    except Exception as exc:
        return {
            "returncode": None,
            "stdout": "",
            "stderr": "",
            "timed_out": False,
            "run_error": f"{type(exc).__name__}: {exc}",
            "duration_ms": int((time.monotonic() - started) * 1000),
        }


def _parse_responses(stdout: str, label: str, failures: list[str]) -> list[dict]:
    responses: list[dict] = []
    for line_number, line in enumerate(stdout.splitlines(), 1):
        if not line.strip():
            failures.append(f"{label}: stdout line {line_number} is blank")
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            failures.append(f"{label}: stdout line {line_number} is not valid JSON: {exc.msg}")
            continue
        if not isinstance(value, dict):
            failures.append(f"{label}: stdout line {line_number} is not a JSON object")
            continue
        if value.get("jsonrpc") != "2.0":
            failures.append(f"{label}: stdout line {line_number} is not a JSON-RPC 2.0 message")
        responses.append(value)
    return responses


def _error_code(response: dict) -> int | None:
    error = response.get("error")
    if not isinstance(error, dict):
        return None
    code = error.get("code")
    if type(code) is int:
        return code
    return None


def _check(failures: list[str], condition: object, message: str) -> None:
    if not condition:
        failures.append(message)


def _check_error(failures: list[str], response: dict | None, expected_code: int, label: str) -> None:
    if not isinstance(response, dict):
        failures.append(f"{label}: missing response, expected error {expected_code}")
        return
    if "error" not in response:
        failures.append(f"{label}: expected error {expected_code}, got {_response_text(response)}")
        return
    if "result" in response:
        failures.append(f"{label}: response contains both error and result")
    actual_code = _error_code(response)
    if actual_code != expected_code:
        failures.append(f"{label}: expected error {expected_code}, got {actual_code!r}")


def _check_ok(failures: list[str], response: dict | None, label: str) -> bool:
    if not isinstance(response, dict):
        failures.append(f"{label}: missing response")
        return False
    if "error" in response:
        failures.append(f"{label}: unexpected error {_response_text(response)}")
        return False
    if "result" not in response:
        failures.append(f"{label}: response has no result")
        return False
    return True


def _ids_match(actual: list, expected: list) -> bool:
    if len(actual) != len(expected):
        return False
    for got, want in zip(actual, expected):
        if want is None:
            if got is not None:
                return False
        elif type(got) is not int or got != want:
            return False
    return True


def _check_main_responses(failures: list[str], responses: list[dict]) -> list[str]:
    tool_names: list[str] = []

    _check(
        failures,
        len(responses) == EXPECTED_RESPONSE_COUNT,
        f"main stream: expected {EXPECTED_RESPONSE_COUNT} responses, got {len(responses)}",
    )

    for position, response in enumerate(responses, 1):
        _check(
            failures,
            "id" in response,
            f"main stream: response {position} has no id field",
        )

    actual_ids = [response.get("id") for response in responses]
    _check(
        failures,
        _ids_match(actual_ids, EXPECTED_RESPONSE_IDS),
        f"main stream: unexpected response ids {actual_ids!r}",
    )

    first_cases = (
        ("malformed JSON", -32700),
        ("batch", -32600),
        ("invalid boolean id", -32600),
        ("null id", -32600),
        ("float id", -32600),
    )
    for index, (label, expected_code) in enumerate(first_cases):
        if index >= len(responses):
            break
        response = responses[index]
        _check(
            failures,
            "id" in response and response.get("id") is None,
            f"main stream: {label} response id must be null",
        )
        _check_error(failures, response, expected_code, f"main stream: {label} case")

    by_id: dict[int, dict] = {}
    for response in responses:
        response_id = response.get("id")
        if type(response_id) is int:
            by_id[response_id] = response

    initialize = by_id.get(1)
    if _check_ok(failures, initialize, "id 1 (initialize)"):
        result = initialize.get("result")
        if not isinstance(result, dict):
            failures.append("id 1 (initialize): result is not an object")
        else:
            _check(
                failures,
                result.get("protocolVersion") == PROTOCOL_VERSION,
                f"id 1 (initialize): expected protocolVersion {PROTOCOL_VERSION!r}, "
                f"got {result.get('protocolVersion')!r}",
            )

    tools_response = by_id.get(3)
    if _check_ok(failures, tools_response, "id 3 (tools/list)"):
        result = tools_response.get("result")
        if not isinstance(result, dict):
            failures.append("id 3 (tools/list): result is not an object")
        else:
            tools = result.get("tools")
            if not isinstance(tools, list):
                failures.append("id 3 (tools/list): tools is not an array")
            else:
                expected_schema = json.loads((REPO_ROOT / "engrave_mcp/tools.json").read_text(encoding="utf-8"))
                _check(
                    failures,
                    tools == expected_schema,
                    "id 3 (tools/list): registry schemas differ",
                )
                for position, tool in enumerate(tools, 1):
                    if not isinstance(tool, dict):
                        failures.append(f"id 3 (tools/list): tool {position} is not an object")
                        continue
                    name = tool.get("name")
                    if not isinstance(name, str) or not name:
                        failures.append(f"id 3 (tools/list): tool {position} has no valid name")
                    else:
                        tool_names.append(name)

                _check(
                    failures,
                    len(tools) == len(EXPECTED_TOOLS),
                    f"id 3 (tools/list): expected {len(EXPECTED_TOOLS)} tools, got {len(tools)}",
                )
                _check(
                    failures,
                    len(tool_names) == len(EXPECTED_TOOLS),
                    f"id 3 (tools/list): expected {len(EXPECTED_TOOLS)} valid names, got {len(tool_names)}",
                )
                _check(
                    failures,
                    set(tool_names) == set(EXPECTED_TOOLS),
                    f"id 3 (tools/list): expected tool names {EXPECTED_TOOLS!r}, got {tool_names!r}",
                )

    expected_errors = (
        (2, -32600, "tools/list before initialized"),
        (5, -32602, "unknown tool"),
        (6, -32602, "tools/call missing name"),
        (7, -32602, "tools/call non-string name"),
        (8, -32601, "unknown method"),
        (9, -32600, "repeated initialize"),
    )
    for request_id, expected_code, label in expected_errors:
        _check_error(
            failures,
            by_id.get(request_id),
            expected_code,
            f"id {request_id} ({label})",
        )

    for request_id, label in ((4, "ping after tools/list"), (10, "final ping")):
        response = by_id.get(request_id)
        if _check_ok(failures, response, f"id {request_id} ({label})"):
            _check(
                failures,
                response.get("result") == {},
                f"id {request_id} ({label}): expected empty result, got {response.get('result')!r}",
            )

    return tool_names


def _check_oversized_responses(failures: list[str], responses: list[dict]) -> None:
    _check(
        failures,
        len(responses) == 2,
        f"oversized stream: expected 2 responses, got {len(responses)}",
    )
    if not responses:
        return

    first = responses[0]
    _check(
        failures,
        "id" in first and first.get("id") is None,
        "oversized stream: oversize response id must be null",
    )
    _check_error(failures, first, -32700, "oversized stream: oversized line")

    if len(responses) < 2:
        return

    second = responses[1]
    second_id = second.get("id")
    _check(
        failures,
        "id" in second and type(second_id) is int and second_id == 99,
        f"oversized stream: second response must have id 99, got {second_id!r}",
    )
    if _check_ok(failures, second, "oversized stream: ping id 99"):
        _check(
            failures,
            second.get("result") == {},
            f"oversized stream: ping id 99 expected empty result, got {second.get('result')!r}",
        )


def _check_edge_cases(exe: str, failures: list[str]) -> dict[str, dict]:
    cases = {
        "jsonrpc-not-string": (
            '{"jsonrpc":2,"id":3,"method":"ping"}\n',
            -32600,
            "invalid or missing jsonrpc version",
        ),
        "trailing-garbage": (
            '{"jsonrpc":"2.0","id":1,"method":"ping"} garbage\n',
            -32700,
            "Parse error",
        ),
        "leading-bom": (
            '\ufeff{"jsonrpc":"2.0","id":1,"method":"ping"}\n',
            None,
            None,
        ),
    }
    records: dict[str, dict] = {}
    for label, (payload, expected_code, expected_message) in cases.items():
        result = _run_server(exe, payload)
        responses = _parse_responses(result["stdout"], label, failures) if not result["run_error"] else []
        _check(
            failures,
            not result["timed_out"] and not result["run_error"],
            f"{label}: server run failed: {result['run_error'] or 'timeout'}",
        )
        _check(failures, len(responses) == 1, f"{label}: expected one response, got {len(responses)}")
        if responses:
            response = responses[0]
            if expected_code is None:
                if _check_ok(failures, response, label):
                    _check(failures, response.get("result") == {}, f"{label}: expected empty result")
                    _check(failures, response.get("id") == 1, f"{label}: expected id 1")
            else:
                _check_error(failures, response, expected_code, label)
                error = response.get("error")
                message = error.get("message") if isinstance(error, dict) else None
                _check(
                    failures,
                    isinstance(message, str) and expected_message in message,
                    f"{label}: expected message containing {expected_message!r}, got {message!r}",
                )
        records[label] = {
            "input": payload,
            "returncode": result["returncode"],
            "responses": responses,
            "stderr": result["stderr"],
        }
    return records


def _check_music_protocol(exe: str, output: Path, version: str) -> None:
    resources = Path(os.environ.get("ENGRAVE_MCP_RESOURCES", REPO_ROOT / ".runtime/assets"))
    client = Client(Path(exe), resources, output, protocol_version=version)
    legacy = version in ("2024-11-05", "2025-03-26")

    def call(name, arguments):
        result = client.rpc("tools/call", {"name": name, "arguments": arguments})["result"]
        assert not result["isError"], result
        text = result["content"][0]["text"]
        if legacy:
            assert "structuredContent" not in result
            return json.loads(text), text
        return result["structuredContent"], text

    try:
        opened, _ = call("score_create", {})
        document = opened["document"]
        call(
            "score_write_phrase",
            {
                "document": document,
                "lanes": [{"measure": 1, "content": "acciac(D5) C###4 3:2(D4:e E4:e F4:e)"}],
            },
        )
        window, text = call("score_read_music", {"document": document, "count": 1, "include_ids": True})
        assert window["score"] == "master" and window["excerpt"] is None
        assert ("summary" in window) == legacy
        events = window["measures"][0]["voices"][0]["events"]
        assert events[0]["notes"][0]["concert"] == "C###4" and events[0]["notes"][0]["id"]
        assert events[0]["grace_before"][0]["duration"] == "1/8" and window["tuplets"][0]["actual"] == 3
        summary = window["summary"] if legacy else text
        tuplet_id = window["tuplets"][0]["id"]
        for token in ("Bar 1#", "acciac(D5#", "C###4#", f"3:2#{tuplet_id}(D4#"):
            assert token in summary, summary
        if legacy:
            types_payload, types_text = call("runtime_info", {"scope": "types"})
            assert len(types_text) > 256 * 1024 and len(types_payload["types"]) > 20
    finally:
        client.close()


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "usage: python tests/integration/test_transport.py <native-exe> [validation-json]",
            file=sys.stderr,
        )
        return 2

    exe = sys.argv[1]
    output_path = Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_VALIDATION_PATH
    output_path.parent.mkdir(parents=True, exist_ok=True)

    main_stderr_path = output_path.parent / f"{output_path.stem}.main.stderr.txt"
    oversized_stderr_path = output_path.parent / f"{output_path.stem}.oversized.stderr.txt"

    initialize_params = {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {},
        "clientInfo": {"name": "test", "version": "1"},
    }

    for requested in ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25", "2099-01-01"):
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {**initialize_params, "protocolVersion": requested},
        }
        reply = json.loads(_run_server(exe, _compact(request) + "\n")["stdout"])
        assert reply["result"]["protocolVersion"] == (PROTOCOL_VERSION if requested == "2099-01-01" else requested)
        assert "score_write_phrase" in reply["result"]["instructions"]
        for term in ("content string", "C###4", "acciac(D5)", "after16", "3:2", "|"):
            assert term in reply["result"]["instructions"], term
        if requested != "2099-01-01":
            _check_music_protocol(exe, output_path.parent / f"{output_path.stem}.{requested}", requested)

    main_lines = [
        '{"jsonrpc":"2.0","id":0,"method":"ping"',
        _compact([{"jsonrpc": "2.0", "id": 100, "method": "ping"}]),
        _compact({"jsonrpc": "2.0", "id": True, "method": "ping"}),
        _compact({"jsonrpc": "2.0", "id": None, "method": "ping"}),
        _compact({"jsonrpc": "2.0", "id": 1.5, "method": "ping"}),
        _compact(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": initialize_params,
            }
        ),
        _compact({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
        _compact(
            {
                "jsonrpc": "2.0",
                "method": "notifications/initialized",
                "params": {},
            }
        ),
        _compact({"jsonrpc": "2.0", "id": 3, "method": "tools/list"}),
        _compact({"jsonrpc": "2.0", "id": 4, "method": "ping"}),
        _compact(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "tools/call",
                "params": {"name": "not_a_real_tool", "arguments": {}},
            }
        ),
        _compact(
            {
                "jsonrpc": "2.0",
                "id": 6,
                "method": "tools/call",
                "params": {"arguments": {}},
            }
        ),
        _compact(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "tools/call",
                "params": {"name": 7, "arguments": {}},
            }
        ),
        _compact({"jsonrpc": "2.0", "id": 8, "method": "transport/unknown"}),
        _compact(
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "initialize",
                "params": initialize_params,
            }
        ),
        _compact(
            {
                "jsonrpc": "2.0",
                "method": "tools/call",
                "params": {
                    "name": "score_open",
                    "arguments": {"path": "artifacts/__transport_missing__/never-open-this-file.mscz"},
                },
            }
        ),
        _compact({"jsonrpc": "2.0", "id": 10, "method": "ping"}),
    ]
    main_input = "\n".join(main_lines) + "\n"

    oversized_line = '"' + ("a" * (OVERSIZED_LINE_BYTES - 2)) + '"'
    oversized_line_bytes = len(oversized_line.encode("utf-8"))
    boundary_prefix = '{"jsonrpc":"2.0","id":99,"method":"ping","params":{"padding":"'
    boundary_suffix = '"}}'
    boundary_line = (
        boundary_prefix + "a" * (4 * 1024 * 1024 - len(boundary_prefix) - len(boundary_suffix)) + boundary_suffix
    )
    assert len(boundary_line.encode("utf-8")) == 4 * 1024 * 1024
    oversized_input = oversized_line + "\n" + boundary_line + "\n"

    failures: list[str] = []
    tool_names: list[str] = []

    main_run = _run_server(exe, main_input)
    main_stderr_path.write_text(main_run["stderr"], encoding="utf-8")

    main_responses: list[dict] = []
    if main_run["timed_out"]:
        failures.append(f"main stream: server timed out after {TIMEOUT_SECONDS} seconds")
    elif main_run["run_error"]:
        failures.append(f"main stream: {main_run['run_error']}")
    else:
        _check(
            failures,
            main_run["returncode"] == 0,
            f"main stream: expected exit code 0, got {main_run['returncode']!r}",
        )
        main_responses = _parse_responses(main_run["stdout"], "main stream", failures)
        tool_names = _check_main_responses(failures, main_responses)

    oversized_run = _run_server(exe, oversized_input)
    oversized_stderr_path.write_text(oversized_run["stderr"], encoding="utf-8")

    oversized_responses: list[dict] = []
    if oversized_run["timed_out"]:
        failures.append(f"oversized stream: server timed out after {TIMEOUT_SECONDS} seconds")
    elif oversized_run["run_error"]:
        failures.append(f"oversized stream: {oversized_run['run_error']}")
    else:
        _check(
            failures,
            oversized_run["returncode"] == 0,
            f"oversized stream: expected exit code 0, got {oversized_run['returncode']!r}",
        )
        oversized_responses = _parse_responses(oversized_run["stdout"], "oversized stream", failures)
        _check_oversized_responses(failures, oversized_responses)

    edge_cases = _check_edge_cases(exe, failures)

    main_record = {
        "input_line_count": len(main_lines),
        "returncode": main_run["returncode"],
        "timed_out": main_run["timed_out"],
        "run_error": main_run["run_error"],
        "duration_ms": main_run["duration_ms"],
        "stdout_line_count": len(main_run["stdout"].splitlines()),
        "response_count": len(main_responses),
        "response_ids": [response.get("id") for response in main_responses],
        "responses": main_responses,
        "tool_names": tool_names,
        "stderr_path": str(main_stderr_path),
        "stderr_chars": len(main_run["stderr"]),
    }

    oversized_record = {
        "oversized_line_bytes": oversized_line_bytes,
        "input_line_count": 2,
        "returncode": oversized_run["returncode"],
        "timed_out": oversized_run["timed_out"],
        "run_error": oversized_run["run_error"],
        "duration_ms": oversized_run["duration_ms"],
        "stdout_line_count": len(oversized_run["stdout"].splitlines()),
        "response_count": len(oversized_responses),
        "response_ids": [response.get("id") for response in oversized_responses],
        "responses": oversized_responses,
        "stderr_path": str(oversized_stderr_path),
        "stderr_chars": len(oversized_run["stderr"]),
    }

    validation = {
        "ok": not failures,
        "executable": exe,
        "protocol_version": PROTOCOL_VERSION,
        "expected_response_count": EXPECTED_RESPONSE_COUNT,
        "expected_response_ids": EXPECTED_RESPONSE_IDS,
        "expected_tool_names": EXPECTED_TOOLS,
        "validation_path": str(output_path),
        "main_stream": main_record,
        "oversized_stream": oversized_record,
        "edge_cases": edge_cases,
        "failures": failures,
    }

    with output_path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(
            validation,
            handle,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        handle.write("\n")

    if failures:
        print(
            f"transport validation failed: {len(failures)} check(s)",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        print(f"validation: {output_path}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
