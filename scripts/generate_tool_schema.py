#!/usr/bin/env python3
"""Validate native/tools.json and optionally generate C++ raw string include."""

import argparse
import json
from pathlib import Path
import sys
from typing import Any


def check_subschemas(node: Any, path: str) -> None:
    if isinstance(node, dict):
        node_type = node.get("type")
        is_obj = node_type == "object" or (isinstance(node_type, list) and "object" in node_type)
        if is_obj and node.get("additionalProperties") is not False:
            raise ValueError(f"{path}: explicit object schema must set additionalProperties to false")
        for key, val in node.items():
            check_subschemas(val, f"{path}.{key}")
    elif isinstance(node, list):
        for idx, item in enumerate(node):
            check_subschemas(item, f"{path}[{idx}]")


def validate_tools(tools: Any) -> None:
    if not isinstance(tools, list) or len(tools) == 0:
        raise ValueError("Schema must be a non-empty list of tools")

    names: set[str] = set()
    for tool in tools:
        if not isinstance(tool, dict):
            raise ValueError("Each tool definition must be an object")
        name = tool.get("name")
        if not isinstance(name, str) or not name.strip():
            raise ValueError("Tool name must be a non-empty string")
        if name in names:
            raise ValueError(f"Duplicate tool name: {name}")
        names.add(name)

        if not isinstance(tool.get("description"), str):
            raise ValueError(f"Tool '{name}' description must be a string")

        input_schema = tool.get("inputSchema")
        if not isinstance(input_schema, dict):
            raise ValueError(f"Tool '{name}' inputSchema must be an object")
        if input_schema.get("type") != "object":
            raise ValueError(f"Tool '{name}' inputSchema type must be 'object'")
        if input_schema.get("additionalProperties") is not False:
            raise ValueError(f"Tool '{name}' inputSchema additionalProperties must be false")

        check_subschemas(input_schema, f"Tool '{name}' inputSchema")


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    default_schema = repo_root / "native" / "tools.json"

    parser = argparse.ArgumentParser(description="Validate native/tools.json and optionally generate C++ raw string include.")
    parser.add_argument("--schema", type=Path, default=default_schema, help="Path to schema JSON file")
    parser.add_argument("--output", type=Path, default=None, help="Output path for C++ raw string include")
    args = parser.parse_args()

    if not args.schema.is_file():
        sys.exit(f"Error: Schema file not found: {args.schema}")
    if args.output is not None and args.schema.resolve() == args.output.resolve():
        sys.exit("Error: Output path cannot overwrite schema path")

    try:
        with args.schema.open("r", encoding="utf-8") as f:
            data = json.load(f)
        validate_tools(data)
    except Exception as err:
        sys.exit(f"Error: {err}")

    if args.output is not None:
        compact_json = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
        if ')schema"' in compact_json:
            sys.exit('Error: JSON content contains raw string terminator ")schema""')
        rendered = f'R"schema({compact_json})schema"\n'.encode("utf-8")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if not args.output.exists() or args.output.read_bytes() != rendered:
            args.output.write_bytes(rendered)
        print(f"Validated {len(data)} tools -> {args.output}")
    else:
        print(f"Validated {len(data)} tools.")


if __name__ == "__main__":
    main()
