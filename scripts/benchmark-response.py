#!/usr/bin/env python3
"""Validate a CLI/MCP benchmark response before accepting its timing sample."""
import json
import sys


def response_payload(value):
    if not isinstance(value, dict) or "error" in value or value.get("isError"):
        raise ValueError("benchmark request failed or returned a non-object")
    if "result" in value:
        return response_payload(value["result"])
    if "content" in value:
        content = value["content"]
        if not isinstance(content, list) or len(content) != 1 or content[0].get("type") != "text":
            raise ValueError("expected one JSON text content block")
        return response_payload(json.loads(content[0]["text"]))
    return value


def validate(kind, value):
    data = response_payload(value)
    fields = ("nodes", "edges") if kind == "index" else ("total",)
    for field in fields:
        if type(data.get(field)) is not int or data[field] < 0:
            raise ValueError(f"missing or invalid {field}")
    if kind == "index" and (not isinstance(data.get("project"), str) or not data["project"]):
        raise ValueError("missing project identity")
    return data


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2 or sys.argv[1] not in ("index", "search"):
            raise ValueError("usage: benchmark-response.py index|search")
        result = validate(sys.argv[1], json.load(sys.stdin))
        print(json.dumps(result) if sys.argv[1] == "index" else result["total"])
    except (ValueError, KeyError, TypeError, AttributeError) as exc:
        print(f"Invalid benchmark sample: {exc}", file=sys.stderr)
        sys.exit(1)
