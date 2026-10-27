import json
import os
import subprocess
import sys
import time
from pathlib import Path

PROTOCOL_VERSION = "2025-11-25"
# The advertised tool list must match the checked-in schema exactly.
EXPECTED_TOOLS = [
    tool["name"]
    for tool in json.loads(
        (Path(__file__).resolve().parents[1] / "native/tools.json").read_text(encoding="utf-8")
    )
]
EXPECTED_RESPONSE_IDS = [None, None, None, None, None, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
EXPECTED_RESPONSE_COUNT = 15
TIMEOUT_SECONDS = 45
OVERSIZED_LINE_BYTES = 8 * 1024 * 1024 + 10
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
            [exe] + (["--resources", os.environ["MUSIC_MCP_RESOURCES"]] if os.environ.get("MUSIC_MCP_RESOURCES") else []),
            input=payload,
            text=True,
            encoding="utf-8",
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
            failures.append(
                f"{label}: stdout line {line_number} is not valid JSON: {exc.msg}"
            )
            continue
        if not isinstance(value, dict):
            failures.append(f"{label}: stdout line {line_number} is not a JSON object")
            continue
        if value.get("jsonrpc") != "2.0":
            failures.append(
                f"{label}: stdout line {line_number} is not a JSON-RPC 2.0 message"
            )
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


def _check_error(
    failures: list[str], response: dict | None, expected_code: int, label: str
) -> None:
    if not isinstance(response, dict):
        failures.append(f"{label}: missing response, expected error {expected_code}")
        return
    if "error" not in response:
        failures.append(
            f"{label}: expected error {expected_code}, got {_response_text(response)}"
        )
        return
    if "result" in response:
        failures.append(f"{label}: response contains both error and result")
    actual_code = _error_code(response)
    if actual_code != expected_code:
        failures.append(
            f"{label}: expected error {expected_code}, got {actual_code!r}"
        )


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
        _check_error(
            failures, response, expected_code, f"main stream: {label} case"
        )

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
                for position, tool in enumerate(tools, 1):
                    if not isinstance(tool, dict):
                        failures.append(
                            f"id 3 (tools/list): tool {position} is not an object"
                        )
                        continue
                    name = tool.get("name")
                    if not isinstance(name, str) or not name:
                        failures.append(
                            f"id 3 (tools/list): tool {position} has no valid name"
                        )
                    else:
                        tool_names.append(name)

                _check(
                    failures,
                    len(tools) == len(EXPECTED_TOOLS),
                    f"id 3 (tools/list): expected {len(EXPECTED_TOOLS)} tools, "
                    f"got {len(tools)}",
                )
                _check(
                    failures,
                    len(tool_names) == len(EXPECTED_TOOLS),
                    f"id 3 (tools/list): expected {len(EXPECTED_TOOLS)} valid names, "
                    f"got {len(tool_names)}",
                )
                _check(
                    failures,
                    set(tool_names) == set(EXPECTED_TOOLS),
                    f"id 3 (tools/list): expected tool names {EXPECTED_TOOLS!r}, "
                    f"got {tool_names!r}",
                )

    expected_errors = (
        (2, -32600, "tools/list before initialized"),
        (5, -32602, "unknown tool"),
        (6, -32602, "tools/call missing name"),
        (7, -32602, "tools/call arguments array"),
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
                f"id {request_id} ({label}): expected empty result, "
                f"got {response.get('result')!r}",
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
    _check_error(
        failures, first, -32700, "oversized stream: oversized line"
    )

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
            f"oversized stream: ping id 99 expected empty result, "
            f"got {second.get('result')!r}",
        )


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            "usage: python scripts/test_transport.py <native-exe> [validation-json]",
            file=sys.stderr,
        )
        return 2

    exe = sys.argv[1]
    output_path = Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_VALIDATION_PATH
    output_path.parent.mkdir(parents=True, exist_ok=True)

    main_stderr_path = output_path.parent / f"{output_path.stem}.main.stderr.txt"
    oversized_stderr_path = (
        output_path.parent / f"{output_path.stem}.oversized.stderr.txt"
    )

    initialize_params = {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {},
        "clientInfo": {"name": "test", "version": "1"},
    }

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
                "params": {"name": "score_open", "arguments": []},
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
                    "arguments": {
                        "path": "artifacts/__transport_missing__/never-open-this-file.mscz"
                    },
                },
            }
        ),
        _compact({"jsonrpc": "2.0", "id": 10, "method": "ping"}),
    ]
    main_input = "\n".join(main_lines) + "\n"

    oversized_line = '"' + ("a" * (OVERSIZED_LINE_BYTES - 2)) + '"'
    oversized_line_bytes = len(oversized_line.encode("utf-8"))
    oversized_input = (
        oversized_line
        + "\n"
        + _compact({"jsonrpc": "2.0", "id": 99, "method": "ping"})
        + "\n"
    )

    failures: list[str] = []
    tool_names: list[str] = []

    main_run = _run_server(exe, main_input)
    main_stderr_path.write_text(main_run["stderr"], encoding="utf-8")

    main_responses: list[dict] = []
    if main_run["timed_out"]:
        failures.append(
            f"main stream: server timed out after {TIMEOUT_SECONDS} seconds"
        )
    elif main_run["run_error"]:
        failures.append(f"main stream: {main_run['run_error']}")
    else:
        _check(
            failures,
            main_run["returncode"] == 0,
            f"main stream: expected exit code 0, got {main_run['returncode']!r}",
        )
        main_responses = _parse_responses(
            main_run["stdout"], "main stream", failures
        )
        tool_names = _check_main_responses(failures, main_responses)

    oversized_run = _run_server(exe, oversized_input)
    oversized_stderr_path.write_text(oversized_run["stderr"], encoding="utf-8")

    oversized_responses: list[dict] = []
    if oversized_run["timed_out"]:
        failures.append(
            f"oversized stream: server timed out after {TIMEOUT_SECONDS} seconds"
        )
    elif oversized_run["run_error"]:
        failures.append(f"oversized stream: {oversized_run['run_error']}")
    else:
        _check(
            failures,
            oversized_run["returncode"] == 0,
            f"oversized stream: expected exit code 0, "
            f"got {oversized_run['returncode']!r}",
        )
        oversized_responses = _parse_responses(
            oversized_run["stdout"], "oversized stream", failures
        )
        _check_oversized_responses(failures, oversized_responses)

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
        "response_ids": [
            response.get("id") for response in oversized_responses
        ],
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
