#!/usr/bin/env python3
"""Deterministic acceptance for both doors and the authority/artifact invariants."""

from __future__ import annotations

import json
import hashlib
import os
import pathlib
import shutil
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = pathlib.Path(os.environ.get("SWARM_HARNESS_BIN", ROOT / "build" / "swarm-harness"))
CONFIG = ROOT / "config" / "test.json"


def html_for(label: str) -> str:
    color = "#ff4fb7" if "a" in label.lower() else "#59ddff"
    return f"""<!doctype html>
<html><head><meta charset=\"utf-8\"><title>{label}</title>
<style>html,body,canvas{{margin:0;width:100%;height:100%;overflow:hidden;background:#080615}}</style></head>
<body><canvas id=\"field\"></canvas><script>
const c=document.getElementById('field'),x=c.getContext('2d');
function size(){{c.width=innerWidth;c.height=innerHeight}} size(); addEventListener('resize',size);
const p=Array.from({{length:160}},(_,i)=>({{x:(i*83)%innerWidth,y:(i*47)%innerHeight,vx:Math.sin(i)*.7,vy:Math.cos(i)*.7}}));
function frame(){{x.fillStyle='rgba(8,6,21,.18)';x.fillRect(0,0,c.width,c.height);x.fillStyle='{color}';
for(const q of p){{q.x=(q.x+q.vx+c.width)%c.width;q.y=(q.y+q.vy+c.height)%c.height;x.beginPath();x.arc(q.x,q.y,2.2,0,7);x.fill()}}requestAnimationFrame(frame)}}frame();
</script></body></html>"""


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    lock = threading.Lock()
    worker_calls = 0
    director_calls = 0

    def log_message(self, *_args) -> None:
        return

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        if request.get("chat_template_kwargs", {}).get("enable_thinking") is not False:
            self.reply({"error": "enable_thinking was not forced false"}, status=400)
            return
        if "tools" in request:
            with self.lock:
                type(self).director_calls += 1
            response = self.director_response(request)
        else:
            with self.lock:
                type(self).worker_calls += 1
            response = self.worker_response(request)
        self.reply(response)

    def reply(self, payload: dict, status: int = 200) -> None:
        encoded = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    @staticmethod
    def completion(message: dict, finish_reason: str = "stop") -> dict:
        return {
            "id": "mock",
            "object": "chat.completion",
            "choices": [{"index": 0, "message": message, "finish_reason": finish_reason}],
            "usage": {"prompt_tokens": 100, "completion_tokens": 200},
        }

    def worker_response(self, request: dict) -> dict:
        messages = request["messages"]
        assert all(isinstance(message.get("content"), str) for message in messages)
        prompt = messages[-1]["content"]
        if "FIXTURE_T1_BAD" in prompt:
            content = (ROOT / "tests" / "fixtures" / "t1_bad.html").read_text()
        elif "FIXTURE_T1_CONFORMING" in prompt:
            content = (ROOT / "tests" / "fixtures" / "t1_conforming.html").read_text()
        elif "FIXTURE_T2_PLACEHOLDER" in prompt:
            content = (ROOT / "tests" / "fixtures" / "t2_placeholder.html").read_text()
        elif "FIXTURE_T4_BAD" in prompt:
            content = (ROOT / "tests" / "fixtures" / "t4_bad.html").read_text()
        elif "Adversary" in prompt:
            content = "dir_b is weaker: its cyan field lacks pointer interaction. Repair only pointer attraction."
        elif "Verifier/Integrator" in prompt:
            content = "CONFIRMED: dir_b lacks pointer interaction while dir_a has it. No contradiction."
        elif "FORCE_CLIP" in prompt:
            return self.completion({"role": "assistant", "content": html_for("clipped")}, "length")
        elif "Return the COMPLETE artifact" in prompt:
            content = html_for(prompt.split("TASK ID:", 1)[-1].splitlines()[0].strip())
        else:
            content = "Complete analysis result."
        return self.completion({"role": "assistant", "content": content})

    def director_response(self, request: dict) -> dict:
        messages = request["messages"]
        if "JOB ID: artifact_contract_test" in messages[1]["content"]:
            called = [
                call["function"]["name"]
                for message in messages
                if message.get("role") == "assistant"
                for call in message.get("tool_calls", [])
            ]

            def fixture_call(call_id: str, name: str, arguments: dict) -> dict:
                return {"id": call_id, "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}

            if not called:
                calls = [fixture_call("f1", "list_workers", {"job_id": "artifact_contract_test"})]
            elif called == ["list_workers"]:
                calls = [fixture_call("f2", "dispatch_task", {
                    "job_id": "artifact_contract_test", "task_id": "door_a_t2", "role": "Builder",
                    "seat": "swarm", "prompt": "FIXTURE_T2_PLACEHOLDER", "artifact_name": "candidate_a.html",
                    "artifact_contract_id": "t2_placeholder_contract", "input_artifact_ids": [],
                })]
            else:
                calls = [fixture_call("f3", "get_result", {
                    "job_id": "artifact_contract_test", "task_id": "door_a_t2", "wait_ms": 5000,
                })]
            return self.completion({"role": "assistant", "content": None, "tool_calls": calls})
        called = [
            call["function"]["name"]
            for message in messages
            if message.get("role") == "assistant"
            for call in message.get("tool_calls", [])
        ]
        results = [json.loads(message["content"]) for message in messages if message.get("role") == "tool"]

        def call(call_id: str, name: str, arguments: dict) -> dict:
            return {"id": call_id, "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}

        job = "director_smoke"
        tool_calls = []
        if not called:
            tool_calls = [call("c1", "list_workers", {"job_id": job})]
        elif called == ["list_workers"]:
            tool_calls = [
                call("c2", "dispatch_task", {"job_id": job, "task_id": "dir_a", "role": "Builder", "seat": "swarm", "prompt": "Build particle page A with pointer attraction.", "artifact_name": "dir_a.html", "artifact_contract_id": "dir_a_contract", "input_artifact_ids": []}),
                call("c3", "dispatch_task", {"job_id": job, "task_id": "dir_b", "role": "Builder", "seat": "swarm", "prompt": "Build particle page B with a calm cyan field.", "artifact_name": "dir_b.html", "artifact_contract_id": "dir_b_contract", "input_artifact_ids": []}),
            ]
        elif called == ["list_workers", "dispatch_task", "dispatch_task"]:
            tool_calls = [
                call("c4", "get_result", {"job_id": job, "task_id": "dir_a", "wait_ms": 5000}),
                call("c5", "get_result", {"job_id": job, "task_id": "dir_b", "wait_ms": 5000}),
            ]
        elif called[-2:] == ["get_result", "get_result"] and "run_gate" not in called:
            tool_calls = [
                call("c6", "run_gate", {"job_id": job, "task_id": "dir_a", "gate_name": "static_html"}),
                call("c7", "run_gate", {"job_id": job, "task_id": "dir_b", "gate_name": "static_html"}),
            ]
        elif called[-2:] == ["run_gate", "run_gate"] and "dir_adversary" not in json.dumps(results):
            tool_calls = [call("c8", "dispatch_task", {"job_id": job, "task_id": "dir_adversary", "role": "Adversary", "seat": "solo", "prompt": "Compare both complete pages and identify exactly one worst defect.", "input_artifact_ids": ["dir_a", "dir_b"]})]
        elif called[-1] == "dispatch_task" and results[-1].get("task_id") == "dir_adversary":
            tool_calls = [call("c9", "get_result", {"job_id": job, "task_id": "dir_adversary", "wait_ms": 5000})]
        elif called[-1] == "get_result" and results[-1].get("task_id") == "dir_adversary":
            tool_calls = [{"id": "c10-bad", "type": "function", "function": {"name": "dispatch_task", "arguments": "{\"job_id\":\"director_smoke\",\"task_id\":\"dir_verifier\",BROKEN"}}]
        elif results[-1].get("outcome") == "REFUSED" and results[-1].get("retryable") is True:
            tool_calls = [call("c10", "dispatch_task", {"job_id": job, "task_id": "dir_verifier", "role": "Verifier/Integrator", "seat": "solo", "prompt": "Verify the referenced Adversary finding without flattening disagreement.", "input_artifact_ids": ["dir_a", "dir_b"], "input_result_task_ids": ["dir_adversary"]})]
        elif called[-1] == "dispatch_task" and results[-1].get("task_id") == "dir_verifier":
            tool_calls = [call("c11", "get_result", {"job_id": job, "task_id": "dir_verifier", "wait_ms": 5000})]
        elif called[-1] == "get_result" and results[-1].get("task_id") == "dir_verifier":
            tool_calls = [call("c12", "dispatch_task", {"job_id": job, "task_id": "dir_repair", "role": "Builder", "seat": "solo", "prompt": "Add pointer attraction and preserve everything else.", "artifact_name": "dir_repaired.html", "artifact_contract_id": "dir_repaired_contract", "input_artifact_ids": ["dir_b"], "repair_of": "dir_b", "error_receipt_task_id": "dir_verifier"})]
        elif called[-1] == "dispatch_task" and results[-1].get("task_id") == "dir_repair":
            tool_calls = [call("c13", "get_result", {"job_id": job, "task_id": "dir_repair", "wait_ms": 5000})]
        elif called[-1] == "get_result" and results[-1].get("task_id") == "dir_repair":
            tool_calls = [call("c14", "run_gate", {"job_id": job, "task_id": "dir_repair", "gate_name": "static_html"})]
        elif called[-1] == "run_gate" and results[-1].get("task_id") == "dir_repair":
            tool_calls = [call("c15", "finish_job", {"job_id": job, "summary": "Two pages built and gated; weaker page repaired and re-gated.", "contradictions": []})]
        else:
            raise AssertionError(f"unexpected director history: {called[-5:]} results={results[-3:]}")
        return self.completion({"role": "assistant", "content": None, "tool_calls": tool_calls})


class Server:
    def __enter__(self):
        MockHandler.worker_calls = 0
        MockHandler.director_calls = 0
        self.server = ThreadingHTTPServer(("127.0.0.1", 18972), MockHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


def journal(path: pathlib.Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def call_counts() -> tuple[int, int]:
    with MockHandler.lock:
        return MockHandler.worker_calls, MockHandler.director_calls


def run(mode: str, job: str, output: pathlib.Path, expected: int = 0) -> subprocess.CompletedProcess:
    return run_path(mode, ROOT / "jobs" / job, output, expected)


def run_path(mode: str, job: pathlib.Path, output: pathlib.Path, expected: int = 0) -> subprocess.CompletedProcess:
    command = [str(BIN), mode, "--config", str(CONFIG), "--job", str(job), "--output", str(output)]
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
    if completed.returncode != expected:
        raise AssertionError(f"{command} rc={completed.returncode}\nstdout={completed.stdout}\nstderr={completed.stderr}")
    return completed


class McpClient:
    def __init__(self, job: str | pathlib.Path, output: pathlib.Path):
        job_path = pathlib.Path(job)
        if not job_path.is_absolute():
            job_path = ROOT / "jobs" / job_path
        self.process = subprocess.Popen(
            [str(BIN), "mcp", "--config", str(CONFIG), "--job", str(job_path), "--output", str(output)],
            text=True,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.counter = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self.counter += 1
        payload = {"jsonrpc": "2.0", "id": self.counter, "method": method}
        if params is not None:
            payload["params"] = params
        assert self.process.stdin and self.process.stdout
        self.process.stdin.write(json.dumps(payload) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise AssertionError("MCP server closed early: " + (self.process.stderr.read() if self.process.stderr else ""))
        return json.loads(line)

    def tool(self, name: str, arguments: dict) -> tuple[dict, dict]:
        response = self.request("tools/call", {"name": name, "arguments": arguments})
        return response, response["result"]["structuredContent"]

    def close(self) -> None:
        assert self.process.stdin
        self.process.stdin.close()
        rc = self.process.wait(timeout=20)
        if rc != 0:
            raise AssertionError("MCP rc=" + str(rc) + " stderr=" + (self.process.stderr.read() if self.process.stderr else ""))


def test_scripted(root: pathlib.Path) -> None:
    output = root / "scripted"
    run("scripted", "scripted_smoke.json", output)
    assert (output / "scripted_a.html").is_file()
    assert (output / "scripted_b.html").is_file()
    events = journal(output / "journal.jsonl")
    assert events[-1]["event"] == "job_finished" and events[-1]["outcome"] == "DONE"
    assert sum(event["event"] == "gate_completed" and event["outcome"] == "PASSED" for event in events) == 2


def test_director(root: pathlib.Path) -> None:
    output = root / "director"
    run("director", "director_smoke.json", output)
    assert (output / "dir_a.html").is_file()
    assert (output / "dir_b.html").is_file()
    assert (output / "dir_repaired.html").is_file()
    events = journal(output / "journal.jsonl")
    finished = [event for event in events if event["event"] == "job_finished"][-1]
    assert finished["outcome"] == "GATED"
    assert "cannot produce DONE" in finished["authority_invariant"]
    roles = {event.get("role") for event in events if event["event"] == "task_dispatched"}
    assert {"Builder", "Adversary", "Verifier/Integrator"} <= roles
    for task_id in ("dir_adversary", "dir_verifier"):
        result_path = output / ".results" / f"{task_id}.txt"
        assert result_path.is_file()
        completed = next(event for event in events if event.get("event") == "task_completed" and event.get("task_id") == task_id)
        assert completed["result_path"] == f".results/{task_id}.txt"
        assert hashlib.sha256(result_path.read_bytes()).hexdigest() == completed["result_sha256"]


def test_mcp(root: pathlib.Path) -> None:
    output = root / "mcp"
    client = McpClient("mcp_smoke.json", output)
    initialized = client.request("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "local-smoke", "version": "1"}})
    assert initialized["result"]["serverInfo"]["name"] == "swarm-harness"
    discovered = client.request("server/discover", {})
    assert discovered["result"]["protocolVersion"] == "2026-07-28"
    listed = client.request("tools/list", {})
    assert [tool["name"] for tool in listed["result"]["tools"]] == ["list_workers", "dispatch_task", "get_result", "run_gate", "finish_job"]
    _, workers = client.tool("list_workers", {"job_id": "mcp_smoke"})
    assert workers["ok"] and all(not worker["lifecycle_control"] for worker in workers["workers"])
    _, dispatched = client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "mcp_page", "role": "Builder", "seat": "swarm", "prompt": "Build a complete particle page.", "artifact_name": "mcp_page.html", "artifact_contract_id": "mcp_page_contract", "input_artifact_ids": []})
    assert dispatched["state"] == "QUEUED"
    _, result = client.tool("get_result", {"job_id": "mcp_smoke", "task_id": "mcp_page", "wait_ms": 5000})
    assert result["artifact"]["completeness"] == "FULL"
    assert result["artifact"]["identity_check"]["verified"] is True
    _, gated = client.tool("run_gate", {"job_id": "mcp_smoke", "task_id": "mcp_page", "gate_name": "static_html"})
    assert gated["passed"] is True
    escaped = root / "escape.html"
    outside_response, refused = client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "escape", "role": "Builder", "seat": "swarm", "prompt": "escape", "artifact_name": str(escaped), "input_artifact_ids": []})
    assert outside_response["result"]["isError"] is True and refused["outcome"] == "REFUSED"
    assert not escaped.exists()
    _, finished = client.tool("finish_job", {"job_id": "mcp_smoke", "summary": "Local five-tool smoke passed.", "contradictions": []})
    assert finished["state"] == "GATED" and finished["done_possible_through_this_surface"] is False
    client.close()
    events = journal(output / "journal.jsonl")
    assert any(event["event"] == "tool_refused" and "absolute artifact paths" in event["reason"] for event in events)


def test_clipped_judge_refusal(root: pathlib.Path) -> None:
    output = root / "clipped"
    client = McpClient("clipped_refusal.json", output)
    client.tool("dispatch_task", {"job_id": "clipped_refusal", "task_id": "clipper", "role": "Builder", "seat": "swarm", "prompt": "FORCE_CLIP", "artifact_name": "clipper.html", "artifact_contract_id": "clipper_contract", "input_artifact_ids": []})
    _, clipped = client.tool("get_result", {"job_id": "clipped_refusal", "task_id": "clipper", "wait_ms": 5000})
    assert clipped["artifact"]["completeness"] == "CLIPPED"
    assert clipped["state"] == "ARTIFACT_REFUSED" and clipped["failed_check"] == "completeness"
    response, refused = client.tool("dispatch_task", {"job_id": "clipped_refusal", "task_id": "illegal_judge", "role": "Adversary", "seat": "solo", "prompt": "Judge it.", "input_artifact_ids": ["clipper"]})
    assert response["result"]["isError"] is True
    assert "originating artifact refusal task=clipper" in refused["error"]
    _, finished = client.tool("finish_job", {
        "job_id": "clipped_refusal", "summary": "clipped contract refusal", "contradictions": [],
    })
    assert finished["state"] == "FAILED" and finished["refused_tasks"] == ["clipper"]
    client.close()


def test_gate_and_repair_guards(root: pathlib.Path) -> None:
    output = root / "repair_guards"
    client = McpClient("mcp_smoke.json", output)
    client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "source", "role": "Builder", "seat": "swarm", "prompt": "Build a complete particle page.", "artifact_name": "source.html", "artifact_contract_id": "source_contract", "input_artifact_ids": []})
    client.tool("get_result", {"job_id": "mcp_smoke", "task_id": "source", "wait_ms": 5000})

    response, refused = client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "early_judge", "role": "Adversary", "seat": "solo", "prompt": "Judge it.", "input_artifact_ids": ["source"]})
    assert response["result"]["isError"] is True
    assert "GATE LAW" in refused["error"]

    client.tool("run_gate", {"job_id": "mcp_smoke", "task_id": "source", "gate_name": "static_html"})
    client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "verifier", "role": "Verifier/Integrator", "seat": "solo", "prompt": "Issue one exact repair finding.", "input_artifact_ids": ["source"]})
    client.tool("get_result", {"job_id": "mcp_smoke", "task_id": "verifier", "wait_ms": 5000})

    response, refused = client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "overwrite", "role": "Builder", "seat": "solo", "prompt": "Apply the exact repair.", "artifact_name": "source.html", "artifact_contract_id": "source_contract", "input_artifact_ids": ["source"], "repair_of": "source", "error_receipt_task_id": "verifier"})
    assert response["result"]["isError"] is True
    assert "new immutable artifact_name" in refused["error"]

    response, refused = client.tool("dispatch_task", {"job_id": "mcp_smoke", "task_id": "bad_receipt", "role": "Builder", "seat": "solo", "prompt": "Apply the exact repair.", "artifact_name": "repaired.html", "artifact_contract_id": "repaired_contract", "input_artifact_ids": ["source"], "repair_of": "source", "error_receipt_task_id": "source"})
    assert response["result"]["isError"] is True
    assert "Verifier/Integrator" in refused["error"]
    client.close()


def test_planted_failure(root: pathlib.Path) -> None:
    output = root / "planted"
    run("scripted", "planted_failure.json", output, expected=3)
    events = journal(output / "journal.jsonl")
    assert any(event["event"] == "gate_completed" and event["outcome"] == "FAILED" for event in events)
    assert any(event["event"] == "job_failed" and event["reason"] == "fail-closed gate failure" for event in events)
    assert events[-1]["event"] == "job_finished" and events[-1]["outcome"] == "FAILED"


def test_fail_closed_dispatch_freeze(root: pathlib.Path) -> None:
    output = root / "fail_closed"
    client = McpClient("planted_failure.json", output)
    client.tool("dispatch_task", {"job_id": "planted_failure", "task_id": "red_pen", "role": "Builder", "seat": "swarm", "prompt": "Build a complete page.", "artifact_name": "red_pen.html", "artifact_contract_id": "red_pen_contract", "input_artifact_ids": []})
    client.tool("get_result", {"job_id": "planted_failure", "task_id": "red_pen", "wait_ms": 5000})
    _, red = client.tool("run_gate", {"job_id": "planted_failure", "task_id": "red_pen", "gate_name": "planted_failure"})
    assert red["task_state"] == "FAILED"
    response, refused = client.tool("dispatch_task", {"job_id": "planted_failure", "task_id": "after_red", "role": "Scout", "seat": "solo", "prompt": "This must never run.", "input_artifact_ids": []})
    assert response["result"]["isError"] is True
    assert "job is already terminal: FAILED" in refused["error"]
    client.close()
    events = journal(output / "journal.jsonl")
    assert not any(event.get("task_id") == "after_red" and event["event"] == "task_dispatched" for event in events)


def test_fixture_pins() -> None:
    expected = {
        "t1_bad.html": (3206, "e3a255cc7371ecd49ef23c74ec5bf996437175e4d05181b355cddf5be6abc951"),
        "t2_placeholder.html": (16, "e065c14826593dd3f9db8a43cc10ec3294d81c461f76125a8b42beaf1cd8a522"),
        "t4_bad.html": (1982, "ad3232d3ec3621e2ce889e85f95d310e215005f1877ed7dff40f8daa23cf25c5"),
        "t1_conforming.html": (4029, "e38d898d28acf12c270542ccacf868d0e0446ce48b31e9d37f7bbded6bb55601"),
    }
    for name, (byte_count, digest) in expected.items():
        data = (ROOT / "tests" / "fixtures" / name).read_bytes()
        assert len(data) == byte_count
        assert hashlib.sha256(data).hexdigest() == digest


def test_contract_dispatch_guards(root: pathlib.Path) -> None:
    output = root / "contract_dispatch_guards"
    client = McpClient("artifact_contract_test.json", output)
    before = call_counts()[0]
    base = {
        "job_id": "artifact_contract_test", "task_id": "guard", "role": "Builder", "seat": "swarm",
        "prompt": "must not run", "artifact_name": "candidate_a.html", "input_artifact_ids": [],
    }
    cases = [
        ({}, "requires exactly one non-empty"),
        ({"artifact_contract_id": ""}, "requires exactly one non-empty"),
        ({"artifact_contract_id": "unknown"}, "unknown artifact_contract_id"),
        ({"artifact_contract_id": "t1_bad_contract"}, "name mismatch"),
        ({"artifact_contract_id": "t2_placeholder_contract", "artifact_contract_ide": "typo"}, "unknown key"),
    ]
    for index, (extra, needle) in enumerate(cases):
        arguments = dict(base, task_id=f"guard_{index}")
        arguments.update(extra)
        response, refused = client.tool("dispatch_task", arguments)
        assert response["result"]["isError"] and needle in refused["error"]
    response, refused = client.tool("dispatch_task", {
        "job_id": "artifact_contract_test", "task_id": "no_artifact", "role": "Scout", "seat": "solo",
        "prompt": "must not run", "artifact_contract_id": "t1_bad_contract", "input_artifact_ids": [],
    })
    assert response["result"]["isError"] and "forbidden without artifact_name" in refused["error"]
    assert call_counts()[0] == before
    client.close()


def test_contract_load_guards(root: pathlib.Path) -> None:
    specs = root / "invalid_specs"
    specs.mkdir()
    base = {
        "job_id": "invalid_contract", "feel_artifact": False, "mission": "must fail before a model call",
        "artifact_contracts": [
            {"id": "bad", "artifact_name": "bad.html", "format": "html", "minimum_bytes": 256, "validators": ["static_html"]}
        ],
    }
    variants: dict[str, dict] = {}
    unknown_key = json.loads(json.dumps(base)); unknown_key["artifact_contracts"][0]["minimum_bytez"] = 256
    variants["unknown_key"] = unknown_key
    unknown_format = json.loads(json.dumps(base)); unknown_format["artifact_contracts"][0]["format"] = "markup"
    variants["unknown_format"] = unknown_format
    unknown_validator = json.loads(json.dumps(base)); unknown_validator["artifact_contracts"][0]["validators"] = ["typo_gate"]
    variants["unknown_validator"] = unknown_validator
    below_floor = json.loads(json.dumps(base)); below_floor["artifact_contracts"][0]["minimum_bytes"] = 16
    variants["below_floor"] = below_floor
    duplicate = json.loads(json.dumps(base)); duplicate["artifact_contracts"].append(dict(duplicate["artifact_contracts"][0]))
    variants["duplicate"] = duplicate
    before = call_counts()[0]
    for name, spec in variants.items():
        path = specs / f"{name}.json"
        path.write_text(json.dumps(spec))
        completed = run_path("scripted", path, root / f"invalid_output_{name}", expected=1)
        assert completed.stderr.startswith("swarm-harness:")
        assert not (root / f"invalid_output_{name}").exists()
    assert call_counts()[0] == before


def run_contract_fixture(
    root: pathlib.Path,
    directory: str,
    task_id: str,
    marker: str,
    artifact_name: str,
    contract_id: str,
    expected_failed_check: str | None,
) -> tuple[dict, list[dict]]:
    output = root / directory
    client = McpClient("artifact_contract_test.json", output)
    before = call_counts()[0]
    client.tool("dispatch_task", {
        "job_id": "artifact_contract_test", "task_id": task_id, "role": "Builder", "seat": "swarm",
        "prompt": marker, "artifact_name": artifact_name, "artifact_contract_id": contract_id,
        "input_artifact_ids": [],
    })
    _, result = client.tool("get_result", {"job_id": "artifact_contract_test", "task_id": task_id, "wait_ms": 5000})
    assert call_counts()[0] == before + 1
    assert result["failed_check"] == expected_failed_check
    if expected_failed_check:
        assert result["state"] == "ARTIFACT_REFUSED" and result["job_state"] == "FAILED"
        after_refusal = call_counts()[0]
        response, blocked = client.tool("dispatch_task", {
            "job_id": "artifact_contract_test", "task_id": task_id + "_blocked", "role": "Adversary",
            "seat": "solo", "prompt": "must not run", "input_artifact_ids": [task_id],
        })
        assert response["result"]["isError"] and f"originating artifact refusal task={task_id}" in blocked["error"]
        gate_response, _ = client.tool("run_gate", {
            "job_id": "artifact_contract_test", "task_id": task_id, "gate_name": "static_html",
        })
        assert gate_response["result"]["isError"]
        _, finished = client.tool("finish_job", {
            "job_id": "artifact_contract_test", "summary": "refusal receipt", "contradictions": [],
        })
        assert finished["state"] == "FAILED" and finished["artifact_refusal_count"] == 1
        assert finished["refused_tasks"] == [task_id]
        assert call_counts()[0] == after_refusal
    else:
        assert result["state"] == "PENNED" and all(check["passed"] for check in result["artifact_checks"])
        _, gate = client.tool("run_gate", {
            "job_id": "artifact_contract_test", "task_id": task_id, "gate_name": "static_html",
        })
        assert gate["passed"]
        _, finished = client.tool("finish_job", {
            "job_id": "artifact_contract_test", "summary": "conforming control", "contradictions": [],
        })
        assert finished["state"] == "DONE"
    client.close()
    events = journal(output / "journal.jsonl")
    return result, events


def test_contract_fixtures_and_state_law(root: pathlib.Path) -> None:
    t2, t2_events = run_contract_fixture(
        root, "contract_t2", "t2", "FIXTURE_T2_PLACEHOLDER", "candidate_a.html",
        "t2_placeholder_contract", "minimum_bytes")
    assert t2["artifact"]["byte_count"] == 16
    assert t2["artifact"]["sha256"] == "e065c14826593dd3f9db8a43cc10ec3294d81c461f76125a8b42beaf1cd8a522"
    refusal = next(event for event in t2_events if event["event"] == "artifact_refused")
    assert refusal["artifact_contract_id"] == "t2_placeholder_contract"
    assert len(refusal["artifact_contract_sha256"]) == 64
    assert [check["name"] for check in refusal["artifact_checks"]] == [
        "completeness", "minimum_bytes", "format:html", "validator:t1_shape"]
    assert refusal["failed_check"] == "minimum_bytes" and refusal["worker"] and refusal["task_id"] == "t2"
    assert refusal["task_state"] == "ARTIFACT_REFUSED" and refusal["job_state"] == "FAILED"
    assert sum(event["event"] == "artifact_refused" for event in t2_events) == refusal["artifact_refusal_count"] == 1

    t1, _ = run_contract_fixture(
        root, "contract_t1_bad", "t1_bad", "FIXTURE_T1_BAD", "t1_bad.html",
        "t1_bad_contract", "validator:t1_shape")
    particle_check = next(check for check in t1["artifact_checks"] if check["name"] == "validator:t1_shape")
    assert '"particle_count": false' in particle_check["output"]
    run_contract_fixture(
        root, "contract_t4_bad", "t4_bad", "FIXTURE_T4_BAD", "t4_bad.html",
        "t4_bad_contract", "validator:t1_shape")
    conforming, _ = run_contract_fixture(
        root, "contract_conforming", "conforming", "FIXTURE_T1_CONFORMING", "t1_conforming.html",
        "t1_conforming_contract", None)
    assert conforming["artifact"]["sha256"] == "e38d898d28acf12c270542ccacf868d0e0446ce48b31e9d37f7bbded6bb55601"


def test_negative_control_and_door_a(root: pathlib.Path) -> None:
    output = root / "negative_control"
    before = call_counts()[0]
    run("scripted", "artifact_contract_negative.json", output, expected=3)
    assert call_counts()[0] == before + 1
    events = journal(output / "journal.jsonl")
    refusal = next(event for event in events if event["event"] == "artifact_refused")
    assert refusal["failed_check"] == "minimum_bytes"
    finished = next(event for event in events if event["event"] == "job_finished")
    assert finished["outcome"] == "FAILED" and finished["artifact_refusal_count"] == 1

    door_a = root / "contract_door_a"
    worker_before, director_before = call_counts()
    run("director", "artifact_contract_test.json", door_a, expected=4)
    worker_after, director_after = call_counts()
    assert worker_after == worker_before + 1
    assert director_after == director_before + 3
    door_a_events = journal(door_a / "journal.jsonl")
    assert any(event["event"] == "artifact_refused" and event["task_id"] == "door_a_t2" for event in door_a_events)
    assert not any(event["event"] == "task_completed" and event.get("task_id") == "door_a_t2" for event in door_a_events)


def test_shipped_contract_empty_set() -> None:
    jobs = {path.name: json.loads(path.read_text()) for path in (ROOT / "jobs").glob("*.json")}
    assert jobs
    for name, spec in jobs.items():
        contracts = spec.get("artifact_contracts")
        assert isinstance(contracts, list) and contracts, f"{name} has no artifact contracts"
        by_id = {contract["id"]: contract for contract in contracts}
        assert len(by_id) == len(contracts)
        for task in spec.get("tasks", []):
            if "artifact_name" in task:
                contract_id = task.get("artifact_contract_id")
                assert contract_id in by_id, f"{name}:{task.get('task_id')} lacks a contract"
                assert by_id[contract_id]["artifact_name"] == task["artifact_name"]
            else:
                assert "artifact_contract_id" not in task
    assert {"scripted_smoke.json", "director_smoke.json", "mcp_smoke.json"} <= jobs.keys()


def main() -> int:
    assert BIN.is_file(), BIN
    with tempfile.TemporaryDirectory(prefix="swarm-harness-test-") as directory, Server():
        root = pathlib.Path(directory)
        before = set(root.iterdir())
        assert not before
        test_fixture_pins()
        test_shipped_contract_empty_set()
        test_contract_dispatch_guards(root)
        test_contract_load_guards(root)
        test_contract_fixtures_and_state_law(root)
        test_negative_control_and_door_a(root)
        test_scripted(root)
        test_director(root)
        test_mcp(root)
        test_clipped_judge_refusal(root)
        test_gate_and_repair_guards(root)
        test_planted_failure(root)
        test_fail_closed_dispatch_freeze(root)
        expected = {root / name for name in (
            "contract_dispatch_guards", "invalid_specs", "contract_t2", "contract_t1_bad", "contract_t4_bad",
            "contract_conforming", "negative_control", "contract_door_a", "scripted", "director", "mcp",
            "clipped", "repair_guards", "planted", "fail_closed",
        )}
        assert set(root.iterdir()) == expected, "runtime wrote outside declared per-test output directories"
        print(json.dumps({
            "ok": True,
            "tests": ["fixture_pins", "strict_contract_load", "pre_call_dispatch_refusal", "sealed_corpse_refusals", "conforming_control", "negative_control", "artifact_refusal_receipts", "artifact_freeze", "door_a_refusal", "door_b_refusal", "shipped_contract_empty_set", "scripted", "door_a_director", "malformed_tool_recovery", "door_b_mcp", "full_artifact_refusal", "gate_law", "repair_immutability", "verifier_receipt", "planted_failure", "fail_closed_freeze", "write_boundary"],
            "root": str(root),
        }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
