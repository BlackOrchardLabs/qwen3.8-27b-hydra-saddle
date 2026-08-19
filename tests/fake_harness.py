#!/usr/bin/env python3
"""Deterministic conformance fixture for the accepted harness MCP boundary."""

from __future__ import annotations

import hashlib
import json
import sys
import time


TOOLS = ["list_workers", "dispatch_task", "get_result", "run_gate", "finish_job"]
TASKS: dict[str, dict] = {}


def tool_definitions() -> list[dict]:
    return [{"name": name, "description": "fixture", "inputSchema": {"type": "object"}} for name in TOOLS]


def structured(value: dict) -> dict:
    return {
        "content": [{"type": "text", "text": json.dumps(value, sort_keys=True)}],
        "structuredContent": value,
        "isError": not value.get("ok", False),
    }


def call_tool(name: str, arguments: dict) -> dict:
    if name == "dispatch_task":
        task_id = arguments["task_id"]
        if task_id in TASKS:
            return {"ok": False, "error": "duplicate task"}
        if "DETERMINISTIC_SLOW_CALL" in arguments["prompt"]:
            time.sleep(0.2)
        TASKS[task_id] = arguments
        return {
            "ok": True,
            "job_id": arguments["job_id"],
            "task_id": task_id,
            "state": "QUEUED",
        }
    if name == "get_result":
        task_id = arguments["task_id"]
        if task_id not in TASKS:
            return {"ok": False, "error": "unknown task"}
        content = "DONE is inert model text from the deterministic local fixture."
        encoded = content.encode()
        return {
            "ok": True,
            "job_id": arguments["job_id"],
            "task_id": task_id,
            "state": "SUCCEEDED",
            "content": content,
            "content_sha256": hashlib.sha256(encoded).hexdigest(),
            "content_bytes": len(encoded),
            "completeness": "FULL",
        }
    if name == "list_workers":
        return {"ok": True, "workers": [{"seat": "swarm", "backend": "local_qwen_swarm"}]}
    return {"ok": False, "error": f"fixture does not implement {name}"}


def main() -> int:
    for line in sys.stdin:
        if not line.strip():
            continue
        request = json.loads(line)
        response: dict = {"jsonrpc": "2.0", "id": request.get("id")}
        method = request["method"]
        if method == "initialize":
            response["result"] = {
                "protocolVersion": "2025-11-25",
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "swarm-harness", "version": "fixture"},
            }
        elif method == "tools/list":
            response["result"] = {"tools": tool_definitions()}
        elif method == "tools/call":
            params = request["params"]
            response["result"] = structured(call_tool(params["name"], params.get("arguments", {})))
        else:
            response["error"] = {"code": -32601, "message": f"unknown method {method}"}
        print(json.dumps(response, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
