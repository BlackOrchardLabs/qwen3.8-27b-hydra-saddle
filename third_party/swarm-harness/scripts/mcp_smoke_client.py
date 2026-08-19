#!/usr/bin/env python3
"""Local Door B client smoke; deliberately performs no external-seat wiring."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


EXPECTED_TOOLS = ["list_workers", "dispatch_task", "get_result", "run_gate", "finish_job"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--job", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--receipt", required=True)
    args = parser.parse_args()

    command = [
        args.binary,
        "mcp",
        "--config",
        args.config,
        "--job",
        args.job,
        "--output",
        args.output,
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    transcript: list[dict] = []
    next_id = 1

    def request(method: str, params: dict | None = None) -> dict:
        nonlocal next_id
        message: dict = {"jsonrpc": "2.0", "id": next_id, "method": method}
        if params is not None:
            message["params"] = params
        next_id += 1
        process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        process.stdin.flush()
        line = process.stdout.readline()
        if not line:
            stderr = process.stderr.read() if process.stderr is not None else ""
            raise RuntimeError(f"Door B closed before responding: {stderr}")
        response = json.loads(line)
        transcript.append({"request": message, "response": response})
        if "error" in response:
            raise RuntimeError(f"JSON-RPC error: {response['error']}")
        return response["result"]

    def tool(name: str, arguments: dict) -> dict:
        return request("tools/call", {"name": name, "arguments": arguments})

    try:
        initialized = request(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "swarm-harness-local-smoke", "version": "1"},
            },
        )
        if initialized["serverInfo"]["name"] != "swarm-harness":
            raise RuntimeError("unexpected Door B server identity")

        listed = request("tools/list")
        names = [item["name"] for item in listed["tools"]]
        if names != EXPECTED_TOOLS:
            raise RuntimeError(f"unexpected tool surface: {names}")

        job_id = "mcp_smoke"
        worker_result = tool("list_workers", {"job_id": job_id})
        if worker_result["isError"]:
            raise RuntimeError("list_workers failed")

        dispatched = tool(
            "dispatch_task",
            {
                "job_id": job_id,
                "task_id": "mcp_live_page",
                "role": "Builder",
                "seat": "swarm",
                "prompt": (
                    "Return one complete self-contained HTML document only. Include <!doctype html>, "
                    "a full-viewport canvas, and inline JavaScript that draws a visible particle field. "
                    "No markdown fences and no external assets."
                ),
                "artifact_name": "mcp_live_page.html",
                "artifact_contract_id": "mcp_live_page_contract",
                "input_artifact_ids": [],
            },
        )
        if dispatched["isError"]:
            raise RuntimeError("dispatch_task failed")

        result = None
        for _ in range(4):
            result = tool(
                "get_result",
                {"job_id": job_id, "task_id": "mcp_live_page", "wait_ms": 30000},
            )
            state = result["structuredContent"]["state"]
            if state != "RUNNING" and state != "QUEUED":
                break
        if result is None or result["structuredContent"].get("artifact", {}).get("completeness") != "FULL":
            raise RuntimeError("Door B did not return a FULL artifact")

        gated = tool(
            "run_gate",
            {"job_id": job_id, "task_id": "mcp_live_page", "gate_name": "static_html"},
        )
        if gated["isError"] or not gated["structuredContent"].get("passed"):
            raise RuntimeError("Door B static gate did not pass")

        refused = tool(
            "dispatch_task",
            {
                "job_id": job_id,
                "task_id": "outside_escape",
                "role": "Builder",
                "seat": "swarm",
                "prompt": "This dispatch must be refused before reaching a worker.",
                "artifact_name": "/tmp/swarm-harness-mcp-escape.html",
                "input_artifact_ids": [],
            },
        )
        refusal = refused["structuredContent"]
        if not refused["isError"] or refusal.get("outcome") != "REFUSED":
            raise RuntimeError("absolute outside path was not refused")

        finished = tool(
            "finish_job",
            {"job_id": job_id, "summary": "Door B local-client smoke passed.", "contradictions": []},
        )
        if finished["isError"] or finished["structuredContent"].get("state") != "GATED":
            raise RuntimeError("feel-artifact did not terminate GATED")
    finally:
        if process.stdin is not None:
            process.stdin.close()
        exit_code = process.wait(timeout=10)

    receipt = {
        "ok": True,
        "door": "B",
        "client": "local stdio JSON-RPC only",
        "external_seat_wiring": False,
        "tool_names": EXPECTED_TOOLS,
        "outside_path": "/tmp/swarm-harness-mcp-escape.html",
        "outside_path_created": pathlib.Path("/tmp/swarm-harness-mcp-escape.html").exists(),
        "terminal_state": "GATED",
        "server_exit_code": exit_code,
        "transcript": transcript,
    }
    pathlib.Path(args.receipt).write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({key: value for key, value in receipt.items() if key != "transcript"}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"mcp smoke failed: {error}", file=sys.stderr)
        raise
