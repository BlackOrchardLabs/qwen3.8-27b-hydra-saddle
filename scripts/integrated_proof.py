#!/usr/bin/env python3
"""Live A+B+C proof: discovered learning reaches a fresh stateless worker."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import signal
import statistics
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass


MODEL_SHA256 = "ce66a629d4a3516bba27ca91de29372f086f90f72ddb92fe298de67b8bb88bbc"
SYSTEM = (
    "You are a fresh stateless worker. A hash-identified EXPERIENCE_LEDGER_PACKET is verified "
    "context; copy its exact fixture response, including leading zeroes. Without one, never "
    "guess. Return only the exact requested form."
)


@dataclass(frozen=True)
class Fixture:
    slug: str
    fixture_id: str
    xor_key: int
    trial_one_input: int
    trial_one_observed: int
    trial_two_input: int
    trial_two_observed: int
    target_input: int
    handling: str
    expected_answer: str
    seed_base: int


FIXTURES = {
    "ember21": Fixture(
        slug="ember21",
        fixture_id="EMBER-21",
        xor_key=0x6D,
        trial_one_input=0x10,
        trial_one_observed=0x7D,
        trial_two_input=0x22,
        trial_two_observed=0x4F,
        target_input=0x3A,
        handling="APPLY",
        expected_answer="57|APPLY",
        seed_base=22100,
    ),
    "tidal09": Fixture(
        slug="tidal09",
        fixture_id="TIDAL-09",
        xor_key=0x01,
        trial_one_input=0x24,
        trial_one_observed=0x25,
        trial_two_input=0x21,
        trial_two_observed=0x20,
        target_input=0x00,
        handling="APPLY",
        expected_answer="01|APPLY",
        seed_base=30900,
    ),
}


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_text(value: str) -> str:
    return sha256_bytes(value.encode())


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fingerprint_model(path: pathlib.Path) -> str:
    if not path.is_file():
        raise SystemExit(f"model file does not exist: {path}")
    return sha256_file(path)


def endpoint_metadata_urls(endpoint: str) -> tuple[str, str]:
    parsed = urllib.parse.urlparse(endpoint)
    if parsed.scheme != "http" or parsed.hostname not in {"127.0.0.1", "localhost", "::1"}:
        raise SystemExit("endpoint must be an HTTP loopback URL")
    suffix = "/v1/chat/completions"
    if not parsed.path.endswith(suffix) or parsed.query or parsed.fragment:
        raise SystemExit(
            "endpoint path must end in /v1/chat/completions"
        )
    prefix = parsed.path[:-len(suffix)]
    origin = urllib.parse.urlunparse((parsed.scheme, parsed.netloc, "", "", "", ""))
    return origin + prefix + "/v1/models", origin + prefix + "/health"


def prepare_model(args: argparse.Namespace) -> tuple[str, str]:
    model_sha = fingerprint_model(args.model)
    _, health_url = endpoint_metadata_urls(args.endpoint)
    args.run_model_sha256 = model_sha
    args.reference_file_match = model_sha == MODEL_SHA256
    args.response_model_ids = []
    return model_sha, health_url


def display_model_name(value: object) -> str:
    if not isinstance(value, str) or not value:
        return "unreported-qwen-model"
    normalized = value.replace("\\", "/").rstrip("/")
    return normalized.rsplit("/", 1)[-1] or "unreported-qwen-model"


def capabilities(role: str) -> list[str]:
    return {
        "builder": ["read_assignment", "submit_result"],
        "scout": ["read_assignment", "submit_observation"],
        "adversary": ["read_assignment", "submit_challenge"],
        "verifier": ["read_assignment", "request_gate", "submit_verification"],
    }[role]


def identity(fixture: Fixture, member: str, task: str, run: str, mount: str, role: str) -> dict:
    return {
        "member_id": f"{fixture.slug}_{member}",
        "seat_backend": "local_qwen_swarm",
        "epistemic_role": role,
        "authority_mode": "bot",
        "capability_set": capabilities(role),
        "thread_id": f"integration_{fixture.slug}_thread",
        "job_id": f"integration_{fixture.slug}",
        "task_id": f"{fixture.slug}_{task}",
        "run_id": f"{fixture.slug}_{run}",
        "mount_id": f"{fixture.slug}_{mount}",
        "job_epoch": 1,
    }


class Organ:
    def __init__(
        self,
        args: argparse.Namespace,
        *,
        journal: pathlib.Path,
        harness_output: pathlib.Path | None = None,
        fault: str | None = None,
    ) -> None:
        command = [
            str(args.organ),
            "--policy", str(args.policy),
            "--workroom-policy", str(args.workroom_policy),
            "--ledger-policy", str(args.ledger_policy),
            "--journal", str(journal),
        ]
        if harness_output is not None:
            command.extend([
                "--harness-bin", str(args.harness_bin),
                "--harness-config", str(args.harness_config),
                "--harness-job", str(args.harness_job),
                "--harness-output", str(harness_output),
            ])
        environment = os.environ.copy()
        if fault:
            environment["DISPATCH_ORGAN_FAULT_POINT"] = fault
        self.command = command
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        self.trace: list[dict] = []
        self.version = 0
        if journal.exists() and journal.stat().st_size:
            status = self.request({"op": "status"})
            if not status.get("ok"):
                raise RuntimeError(f"replayed organ refused status: {status}")
            self.version = int(status["state_version"])

    def request(self, value: dict) -> dict:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(json.dumps(value, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise RuntimeError(
                f"organ exited before response (returncode={self.process.poll()}): {stderr}"
            )
        response = json.loads(line)
        self.trace.append({"request": value, "response": response})
        if "state_version" in response:
            self.version = int(response["state_version"])
        return response

    def mutation(self, op: str, envelope: dict, **fields: object) -> dict:
        request = {"op": op, "expected_version": self.version, "envelope": envelope}
        request.update(fields)
        return self.request(request)

    def close(self) -> str:
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()
        self.process.wait(timeout=20)
        stderr = self.process.stderr.read() if self.process.stderr else ""
        if self.process.returncode != 0:
            raise RuntimeError(f"organ exited {self.process.returncode}: {stderr}")
        return stderr


def expect_ok(response: dict, context: str) -> dict:
    if not response.get("ok"):
        raise AssertionError(f"{context} refused: {response}")
    return response


def expect_refusal(response: dict, code: str, context: str) -> dict:
    if response.get("ok") or response.get("code") != code:
        raise AssertionError(f"{context} expected {code}: {response}")
    return response


def event_payload(response: dict) -> dict:
    return response["receipt"]


def result_harness(response: dict) -> dict:
    return event_payload(response)["receipt"]["harness"]


def result_content(response: dict) -> str:
    return str(result_harness(response)["content"]).strip()


def dispatch_request(
    organ: Organ,
    envelope: dict,
    fixture: Fixture,
    *,
    harness_seat: str,
    artifact_name: str | None = None,
    artifact_contract_id: str | None = None,
    input_artifact_ids: list[str] | None = None,
    input_result_task_ids: list[str] | None = None,
) -> dict:
    fields: dict[str, object] = {
        "question_key": f"integration.work.{fixture.slug}",
        "relevance_keys": [f"fixture:{fixture.fixture_id}"],
        "memory_mode": "normal",
        "requested_max_bytes": 2048,
        "omit_learning_ids": [],
        "declared_omissions": [],
        "harness_seat": harness_seat,
        "input_artifact_ids": input_artifact_ids or [],
        "input_result_task_ids": input_result_task_ids or [],
    }
    if artifact_name is not None or artifact_contract_id is not None:
        assert artifact_name and artifact_contract_id
        fields["artifact_name"] = artifact_name
        fields["artifact_contract_id"] = artifact_contract_id
    return organ.mutation("dispatch", envelope, **fields)


def cast_and_dispatch(
    organ: Organ,
    envelope: dict,
    fixture: Fixture,
    prompt: str,
    *,
    round_index: int,
    harness_seat: str,
    artifact_name: str | None = None,
    artifact_contract_id: str | None = None,
    input_artifact_ids: list[str] | None = None,
    input_result_task_ids: list[str] | None = None,
) -> tuple[dict, dict]:
    expect_ok(organ.mutation(
        "cast", envelope, prompt=prompt, round_index=round_index,
    ), f"cast {envelope['task_id']}")
    dispatched = expect_ok(dispatch_request(
        organ,
        envelope,
        fixture,
        harness_seat=harness_seat,
        artifact_name=artifact_name,
        artifact_contract_id=artifact_contract_id,
        input_artifact_ids=input_artifact_ids,
        input_result_task_ids=input_result_task_ids,
    ), f"dispatch {envelope['task_id']}")
    result = expect_ok(organ.mutation("result", envelope), f"result {envelope['task_id']}")
    return dispatched, result


def post_room(
    organ: Organ,
    envelope: dict,
    event_type: str,
    payload: str,
    *,
    reply_to: int | None = None,
    chatter_round: int = 1,
) -> dict:
    return organ.mutation(
        "workroom_post",
        envelope,
        event_type=event_type,
        reply_to=reply_to,
        bounded_payload=payload,
        group_tags=["integration"],
        chatter_round=chatter_round,
    )


def candidate(
    learning_id: str,
    question_key: str,
    role_scope: str,
    relevance_keys: list[str],
    claim: str,
    limits: str,
    evidence_refs: list[str],
    origin_room_seq: int,
) -> dict:
    return {
        "learning_id": learning_id,
        "question_key": question_key,
        "role_scope": role_scope,
        "relevance_keys": relevance_keys,
        "claim": claim,
        "limits": limits,
        "evidence_refs": evidence_refs,
        "outcome": "unknown",
        "confidence": 0.99,
        "review_after": 1000000,
        "injection_budget": 1024,
        "origin_room_seq": origin_room_seq,
    }


def parse_pipe_record(content: str, prefix: str) -> dict[str, str]:
    line = next((row.strip().strip("`") for row in content.splitlines() if prefix + "|" in row), "")
    if not line:
        raise AssertionError(f"missing {prefix} record in model result: {content!r}")
    line = line[line.index(prefix + "|"):]
    fields: dict[str, str] = {"kind": prefix}
    for part in line.split("|")[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        fields[key.strip()] = value.strip().rstrip(". ")
    return fields


def endpoint_json(url: str, timeout: int = 10) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def chat(
    endpoint: str,
    model: pathlib.Path | str,
    seed: int,
    prompt: str,
    timeout: int,
    max_tokens: int,
    *,
    response_model_sha256: str = MODEL_SHA256,
) -> dict:
    body = json.dumps({
        "model": str(model),
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": prompt},
        ],
        "temperature": 0.0,
        "seed": seed,
        "max_tokens": max_tokens,
        "stream": False,
        "chat_template_kwargs": {"enable_thinking": False},
    }).encode()
    request = urllib.request.Request(
        endpoint, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    started = time.monotonic()
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.loads(response.read())
    elapsed_ms = round((time.monotonic() - started) * 1000, 3)
    choice = payload["choices"][0]
    message = choice["message"]
    usage = payload.get("usage", {})
    response_model = display_model_name(payload.get("model"))
    return {
        "content": message.get("content", "").strip(),
        "reasoning_content": message.get("reasoning_content", ""),
        "finish_reason": choice.get("finish_reason"),
        "prompt_tokens": int(usage.get("prompt_tokens", 0)),
        "completion_tokens": int(usage.get("completion_tokens", 0)),
        "total_tokens": int(usage.get("total_tokens", 0)),
        "elapsed_ms": elapsed_ms,
        "response_model": response_model,
        "response_model_sha256": response_model_sha256,
    }


def chat_for_args(
    args: argparse.Namespace,
    seed: int,
    prompt: str,
    timeout: int,
    max_tokens: int,
) -> dict:
    result = chat(
        args.endpoint,
        args.model,
        seed,
        prompt,
        timeout,
        max_tokens,
        response_model_sha256=args.run_model_sha256,
    )
    if result["response_model"] not in args.response_model_ids:
        args.response_model_ids.append(result["response_model"])
    return result


def reference_manual(fixture: Fixture) -> str:
    rows = [
        "INTEGRATION LAB ARCHIVE — independent reread route for clean controls.",
        "Format: fixture | resolved-byte | handling | evidence-status",
    ]
    for index in range(1, 151):
        name = f"ARCHIVE-{index:03d}"
        answer = f"{(index * 73 + 19) % 256:02X}"
        handling = "REVIEW" if index % 2 else "APPLY"
        rows.append(f"{name} | {answer} | {handling} | independently-verified")
        if index == 79:
            byte, handling = fixture.expected_answer.split("|", 1)
            rows.append(
                f"{fixture.fixture_id} | {byte} | {handling} | independently-verified-from-trial-1+trial-2"
            )
    return "\n".join(rows)


def run_pairs(
    args: argparse.Namespace,
    fixture: Fixture,
    with_receipt: dict,
    without_receipt: dict,
) -> tuple[list[dict], dict]:
    task = (
        f"Return opaque fixture {fixture.fixture_id} as exactly <resolved-byte>|<handling>. "
        "If supplied context does not contain both values, return exactly REREAD_EXPERIMENT."
    )
    packet = with_receipt["exact_packet"]
    packet_hash = with_receipt["exact_packet_hash"]
    pairs: list[dict] = []
    for index in range(1, args.repetitions + 1):
        nonce = f"{fixture.slug}-pair-{index:02d}"
        with_prompt = (
            f"[EXPERIENCE_LEDGER_PACKET sha256={packet_hash}]\n{packet}\n"
            f"[/EXPERIENCE_LEDGER_PACKET]\nEvaluation nonce: {nonce}.\n{task}"
        )
        control_prompt = (
            "[NO_MEMORY_CONTROL reason=no-memory replay]\n"
            f"Evaluation nonce: {nonce}.\n{task}"
        )
        seed = fixture.seed_base + index
        order = ["with", "without"] if index % 2 else ["without", "with"]
        prompts = {"with": with_prompt, "without": control_prompt}
        arms = {
            name: chat_for_args(
                args, seed, prompts[name], args.timeout_seconds, args.max_tokens,
            )
            for name in order
        }
        control_initial = arms["without"]
        rereads = 0 if control_initial["content"] == fixture.expected_answer else 1
        control_final = control_initial
        if rereads:
            control_final = chat_for_args(
                args,
                seed,
                f"{reference_manual(fixture)}\n\nEvaluation nonce: {nonce}.\n{task}",
                args.timeout_seconds,
                args.max_tokens,
            )
        with_exact = arms["with"]["content"] == fixture.expected_answer
        control_exact = control_final["content"] == fixture.expected_answer
        control_total_tokens = control_initial["total_tokens"] + (
            control_final["total_tokens"] if rereads else 0
        )
        pair = {
            "pair_index": index,
            "seed": seed,
            "execution_order": order,
            "with": {
                **arms["with"],
                "quality": 1.0 if with_exact else 0.0,
                "rereads": 0,
                "exact": with_exact,
            },
            "without": {
                "initial": control_initial,
                "final": control_final,
                "quality": 1.0 if control_initial["content"] == fixture.expected_answer else 0.0,
                "positive_calibration_after_reference": control_exact,
                "rereads": rereads,
                "exact": control_exact,
                "total_run_tokens": control_total_tokens,
                "total_run_elapsed_ms": control_initial["elapsed_ms"] + (
                    control_final["elapsed_ms"] if rereads else 0
                ),
            },
        }
        pairs.append(pair)

    aggregate = {
        "repetitions": len(pairs),
        "with_quality_mean": statistics.mean(row["with"]["quality"] for row in pairs),
        "without_quality_mean": statistics.mean(row["without"]["quality"] for row in pairs),
        "quality_delta": statistics.mean(row["with"]["quality"] for row in pairs) -
            statistics.mean(row["without"]["quality"] for row in pairs),
        "with_rereads_total": sum(row["with"]["rereads"] for row in pairs),
        "without_rereads_total": sum(row["without"]["rereads"] for row in pairs),
        "with_tokens_total": sum(row["with"]["total_tokens"] for row in pairs),
        "without_tokens_total": sum(row["without"]["total_run_tokens"] for row in pairs),
        "with_exact_first_pass": sum(row["with"]["exact"] for row in pairs),
        "without_exact_first_pass": sum(
            row["without"]["initial"]["content"] == fixture.expected_answer for row in pairs
        ),
        "without_exact_after_reread": sum(row["without"]["exact"] for row in pairs),
    }
    aggregate["reread_delta"] = aggregate["with_rereads_total"] - aggregate["without_rereads_total"]
    aggregate["token_delta"] = aggregate["with_tokens_total"] - aggregate["without_tokens_total"]
    passed = (
        aggregate["quality_delta"] > 0
        and aggregate["with_rereads_total"] < aggregate["without_rereads_total"]
        and aggregate["with_tokens_total"] < aggregate["without_tokens_total"]
        and aggregate["with_exact_first_pass"] == len(pairs)
        and aggregate["without_exact_first_pass"] == 0
        and aggregate["without_exact_after_reread"] == len(pairs)
        and all(
            row["with"]["quality"] > row["without"]["quality"]
            and row["with"]["rereads"] < row["without"]["rereads"]
            and row["with"]["total_tokens"] < row["without"]["total_run_tokens"]
            for row in pairs
        )
    )
    if not passed:
        raise AssertionError(f"matched-pair causal gate failed: {aggregate}")
    return pairs, aggregate


def verify_journal(path: pathlib.Path) -> list[dict]:
    events: list[dict] = []
    for expected_seq, line in enumerate(path.read_text().splitlines(), start=1):
        if not line:
            continue
        event = json.loads(line)
        received = event.pop("record_sha256")
        canonical = json.dumps(event, sort_keys=True, separators=(",", ":"))
        if sha256_text(canonical) != received:
            raise AssertionError(f"journal hash mismatch at sequence {expected_seq}")
        if event["seq"] != expected_seq:
            raise AssertionError(f"journal sequence mismatch: expected {expected_seq}, got {event['seq']}")
        event["record_sha256"] = received
        events.append(event)
    return events


def run_launcher(path: pathlib.Path, operation: str) -> dict:
    result = subprocess.run(
        [str(path), operation], capture_output=True, text=True, timeout=240, check=False
    )
    return {
        "operation": operation,
        "returncode": result.returncode,
        "stdout": result.stdout.strip(),
        "stderr": result.stderr.strip(),
    }


def terminate_managed_worker(
    path: pathlib.Path,
    model: pathlib.Path,
    pid_path: pathlib.Path,
) -> dict:
    if not pid_path.is_file():
        raise AssertionError("managed-worker PID receipt is absent before controlled worker death")
    pid = int(pid_path.read_text().strip())
    command_path = pathlib.Path(f"/proc/{pid}/cmdline")
    if not command_path.exists():
        raise AssertionError(f"managed-worker PID {pid} vanished before controlled death")
    command = command_path.read_bytes().replace(b"\0", b" ").decode("utf-8", "replace")
    if str(model) not in command or "llama-server" not in command:
        raise AssertionError(f"refusing to kill unverified managed-worker PID {pid}: {command}")
    receipt = run_launcher(path, "down")
    forced_kill = False
    deadline = time.monotonic() + 3
    while command_path.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if command_path.exists():
        os.kill(pid, signal.SIGKILL)
        forced_kill = True
    deadline = time.monotonic() + 10
    while command_path.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if command_path.exists():
        raise AssertionError(f"verified managed-worker PID {pid} survived TERM and KILL")
    receipt.update({
        "verified_pid": pid,
        "verified_command": command,
        "forced_sigkill_after_grace": forced_kill,
        "process_gone": True,
    })
    return receipt


def start_managed_worker(path: pathlib.Path) -> dict:
    attempts: list[dict] = []
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        attempt = run_launcher(path, "up")
        attempts.append(attempt)
        if attempt["returncode"] == 0:
            return {"attempts": attempts, **attempt}
        time.sleep(2)
    raise AssertionError(f"managed worker did not restart after verified worker death: {attempts}")


def live_scenario(args: argparse.Namespace, fixture: Fixture) -> dict:
    for fresh in (args.journal, args.harness_output, args.receipt):
        if fresh.exists():
            raise SystemExit(f"refusing to overwrite integration evidence: {fresh}")
    args.receipt.parent.mkdir(parents=True, exist_ok=True)

    model_sha, health_url = prepare_model(args)

    scout = identity(fixture, "scout", "scout_task", "scout_run", "scout_mount", "scout")
    builder = identity(fixture, "builder", "builder_task", "builder_run", "builder_mount", "builder")
    adversary = identity(fixture, "adversary", "adversary_task", "adversary_run", "adversary_mount", "adversary")
    verifier = identity(fixture, "verifier", "verifier_task", "verifier_run", "verifier_mount", "verifier")
    organ = Organ(args, journal=args.journal, harness_output=args.harness_output)
    all_traces: list[dict] = []
    for mounted in (scout, builder, adversary, verifier):
        expect_ok(organ.mutation("mount", mounted), f"mount {mounted['member_id']}")
    adapter_id = "opaque_local_qwen"
    expect_ok(organ.request({
        "op": "prove_adapter", "expected_version": organ.version, "adapter_id": adapter_id,
    }), "prove safe-boundary adapter")
    for mounted in (scout, builder, adversary, verifier):
        expect_ok(organ.mutation(
            "set_group_tags", mounted, group_tags=["integration"],
        ), f"tag {mounted['member_id']}")
    expect_ok(organ.mutation(
        "set_member_execution", builder, execution_state="OPAQUE_CALL",
    ), "mark builder opaque")

    scout_prompt = (
        f"Trial-1 source lab:{fixture.fixture_id}:trial-1 reports input "
        f"{fixture.trial_one_input:02X} -> observed {fixture.trial_one_observed:02X}. "
        f"The ADD-{fixture.xor_key:02X} and XOR-{fixture.xor_key:02X} hypotheses both fit this one trial. "
        f"Act as Scout and propose the simpler provisional ADD hypothesis. Return exactly one line: "
        f"SCOUT|fixture={fixture.fixture_id}|operator=ADD|key={fixture.xor_key:02X}|source=lab:{fixture.fixture_id}:trial-1"
    )
    _, scout_result = cast_and_dispatch(
        organ, scout, fixture, scout_prompt, round_index=1, harness_seat="scout",
    )
    scout_content = result_content(scout_result)
    scout_fields = parse_pipe_record(scout_content, "SCOUT")
    assert scout_fields.get("operator") == "ADD" and scout_fields.get("fixture") == fixture.fixture_id
    scout_hash = sha256_text(scout_content)
    scout_post = expect_ok(post_room(
        organ,
        scout,
        "OBSERVATION",
        f"SOURCED|result_sha256={scout_hash}|{scout_content}",
    ), "Scout sourced Workroom post")
    scout_room_seq = int(event_payload(scout_post)["record"]["room_seq"])
    scout_learning_id = f"{fixture.slug}_scout_hypothesis"
    expect_ok(organ.mutation(
        "ledger_create_candidate",
        scout,
        candidate=candidate(
            scout_learning_id,
            f"integration.hypothesis.{fixture.slug}",
            "*",
            [f"fixture:{fixture.fixture_id}", "hypothesis:transform"],
            scout_content,
            "Provisional: supported only by trial-1 where ADD and XOR collide.",
            [f"swarm-result:{scout_hash}", f"workroom-seq:{scout_room_seq}"],
            scout_room_seq,
        ),
    ), "Scout hypothesis candidate")

    expect_ok(organ.mutation(
        "set_member_execution", builder, execution_state="SAFE_BOUNDARY",
    ), "mark builder safe")
    builder_read = expect_ok(organ.mutation(
        "workroom_read",
        builder,
        adapter_id=adapter_id,
        safe_boundary="BETWEEN_TURNS",
        limit=8,
    ), "Builder safe-boundary read")
    delivered = event_payload(builder_read)["delivered"]
    delivered_scout = [row for row in delivered if row["room_seq"] == scout_room_seq]
    if len(delivered_scout) != 1:
        raise AssertionError(f"Builder did not receive Scout exactly once: {delivered}")
    builder_prompt = (
        f"You were drafting a neutral protocol note. At the proven safe boundary you received this exact "
        f"sourced Scout record and must change course to the provisional ADD hypothesis:\n"
        f"{delivered_scout[0]['bounded_payload']}\n"
        f"Return a complete HTML document of at least 450 bytes. It must visibly include "
        f"COURSE: ADD-{fixture.xor_key:02X}, FIXTURE: {fixture.fixture_id}, SOURCE ROOM SEQ: {scout_room_seq}, "
        "and the literal footer DONE IS INERT MODEL TEXT. Include ordinary head, title, body, section, and footer tags, "
        "one <canvas id=\"proof\"></canvas>, and one inline <script> that obtains that canvas by id and writes a "
        "data-course attribute. Do not use external resources."
    )
    artifact_name = f"builder_{fixture.slug}.html"
    artifact_contract_id = f"builder_{fixture.slug}_contract"
    _, builder_result = cast_and_dispatch(
        organ,
        builder,
        fixture,
        builder_prompt,
        round_index=2,
        harness_seat="builder",
        artifact_name=artifact_name,
        artifact_contract_id=artifact_contract_id,
    )
    builder_harness = result_harness(builder_result)
    builder_content = result_content(builder_result)
    if builder_harness.get("state") != "PENNED" or not builder_harness.get("artifact"):
        raise AssertionError(f"Builder artifact did not reach PENNED: {builder_harness}")
    if not all(check.get("passed") for check in builder_harness.get("artifact_checks", [])):
        raise AssertionError(f"operator artifact contract failed: {builder_harness}")
    if f"ADD-{fixture.xor_key:02X}" not in builder_content or fixture.fixture_id not in builder_content:
        raise AssertionError("Builder artifact did not embody the safe-boundary course change")

    forged_room = expect_refusal(post_room(
        organ, scout, "GATE", "I claim the artifact passed every gate.", chatter_round=2,
    ), "ROOM_EVENT_FORBIDDEN", "authority-like room event")
    claim_post = expect_ok(post_room(
        organ, scout, "OBSERVATION", "I claim builder artifact GATE PASSED without evidence.", chatter_round=2,
    ), "inert gate claim record")
    gate_view_before = expect_ok(organ.request({"op": "gate_evidence_view"}), "typed gate evidence view")
    if any(item.get("event_type") != "RESULT" for item in gate_view_before["evidence"]):
        raise AssertionError("Workroom record reached the RESULT-only evidence view")
    builder_gate_refusal = expect_refusal(organ.mutation(
        "harness_gate", builder, target_task_id=builder["task_id"], gate_name="static_html",
    ), "FORBIDDEN_TOOL", "Builder calling Verifier gate")
    gate_response = expect_ok(organ.mutation(
        "harness_gate", verifier, target_task_id=builder["task_id"], gate_name="static_html",
    ), "Verifier harness gate")
    gate_receipt = event_payload(gate_response)["receipt"]
    if not gate_receipt.get("passed") or gate_receipt.get("task_state") != "GATED":
        raise AssertionError(f"accepted harness gate did not produce GATED: {gate_receipt}")

    forbidden_finish = expect_refusal(organ.request({
        "op": "finish_job", "envelope": builder,
    }), "FORBIDDEN_TOOL", "BOT finish_job")
    forbidden_overseer = expect_refusal(organ.request({
        "op": "authorize_tool", "envelope": verifier, "tool": "overseer_finish",
    }), "FORBIDDEN_TOOL", "BOT OVERSEER method")
    status_after_done_text = expect_ok(organ.request({"op": "status"}), "status after model DONE text")
    if status_after_done_text["external_completion"] != "AWAITING_OPERATOR":
        raise AssertionError("model text changed external completion")

    adversary_prompt = (
        f"Contradict the Scout's ADD hypothesis using provenance. Trial-2 source "
        f"lab:{fixture.fixture_id}:trial-2 reports input {fixture.trial_two_input:02X} -> observed "
        f"{fixture.trial_two_observed:02X}; ADD-{fixture.xor_key:02X} predicts "
        f"{(fixture.trial_two_input + fixture.xor_key) & 0xFF:02X}, while XOR-{fixture.xor_key:02X} predicts "
        f"{fixture.trial_two_input ^ fixture.xor_key:02X}. Return exactly one line: "
        f"ADVERSARY|fixture={fixture.fixture_id}|operator=XOR|key={fixture.xor_key:02X}|source=lab:{fixture.fixture_id}:trial-2"
    )
    _, adversary_result = cast_and_dispatch(
        organ,
        adversary,
        fixture,
        adversary_prompt,
        round_index=3,
        harness_seat="adversary",
        input_artifact_ids=[builder["task_id"]],
        input_result_task_ids=[scout["task_id"]],
    )
    adversary_content = result_content(adversary_result)
    adversary_fields = parse_pipe_record(adversary_content, "ADVERSARY")
    assert adversary_fields.get("operator") == "XOR" and adversary_fields.get("fixture") == fixture.fixture_id
    adversary_hash = sha256_text(adversary_content)
    adversary_post = expect_ok(post_room(
        organ,
        adversary,
        "CHALLENGE",
        f"PROVENANCE|result_sha256={adversary_hash}|{adversary_content}",
        reply_to=scout_room_seq,
        chatter_round=3,
    ), "Adversary challenge post")
    adversary_room_seq = int(event_payload(adversary_post)["record"]["room_seq"])
    adversary_learning_id = f"{fixture.slug}_adversary_hypothesis"
    expect_ok(organ.mutation(
        "ledger_create_candidate",
        adversary,
        candidate=candidate(
            adversary_learning_id,
            f"integration.hypothesis.{fixture.slug}",
            "*",
            [f"fixture:{fixture.fixture_id}", "hypothesis:transform"],
            adversary_content,
            "Contradiction cites trial-2; requires independent experimental resolution.",
            [f"swarm-result:{adversary_hash}", f"workroom-seq:{adversary_room_seq}"],
            adversary_room_seq,
        ),
    ), "Adversary hypothesis candidate")

    pre_discovery_status = expect_ok(organ.request({"op": "ledger_status"}), "pre-discovery ledger status")
    pre_discovery_ids = [row["learning_id"] for row in pre_discovery_status["learnings"]]
    discovery_learning_id = f"{fixture.slug}_discovered_resolution"
    if discovery_learning_id in pre_discovery_ids:
        raise AssertionError("discovered learning was present before Verifier work")
    verifier_prompt = (
        f"Resolve the live disagreement experimentally; do not vote. Competing operators are ADD-{fixture.xor_key:02X} "
        f"and XOR-{fixture.xor_key:02X}. Recompute both against: trial-1 input {fixture.trial_one_input:02X} "
        f"observed {fixture.trial_one_observed:02X}; trial-2 input {fixture.trial_two_input:02X} observed "
        f"{fixture.trial_two_observed:02X}. Apply the operator that matches BOTH trials to target input "
        f"{fixture.target_input:02X}. For XOR, independently compute the high and low hexadecimal nibbles "
        f"before joining them; no target answer is supplied. Handling rule: resolved byte below 80 hex => "
        f"APPLY; otherwise REVIEW. "
        f"Return exactly one line: DISCOVERY|fixture={fixture.fixture_id}|operator=<ADD-or-XOR>|"
        f"key={fixture.xor_key:02X}|answer=<two uppercase hex>|handling=<APPLY-or-REVIEW>|"
        f"evidence=lab:{fixture.fixture_id}:trial-1+trial-2"
    )
    _, verifier_result = cast_and_dispatch(
        organ,
        verifier,
        fixture,
        verifier_prompt,
        round_index=4,
        harness_seat="verifier",
        input_artifact_ids=[builder["task_id"]],
        input_result_task_ids=[scout["task_id"], adversary["task_id"]],
    )
    verifier_content = result_content(verifier_result)
    verifier_fields = parse_pipe_record(verifier_content, "DISCOVERY")
    expected_byte, expected_handling = fixture.expected_answer.split("|", 1)
    required_fields = {
        "fixture": fixture.fixture_id,
        "operator": "XOR",
        "key": f"{fixture.xor_key:02X}",
        "answer": expected_byte,
        "handling": expected_handling,
    }
    for key, expected in required_fields.items():
        if verifier_fields.get(key) != expected:
            raise AssertionError(f"Verifier discovery field {key}={verifier_fields.get(key)!r}, expected {expected!r}: {verifier_content}")
    verifier_hash = sha256_text(verifier_content)
    verifier_post = expect_ok(post_room(
        organ,
        verifier,
        "ANSWER",
        f"EXPERIMENTAL_RESOLUTION|result_sha256={verifier_hash}|{verifier_content}",
        reply_to=adversary_room_seq,
        chatter_round=4,
    ), "Verifier experimental resolution post")
    verifier_room_seq = int(event_payload(verifier_post)["record"]["room_seq"])
    discovered_claim = (
        f"For opaque fixture {fixture.fixture_id}, the experimentally resolved exact response is "
        f"{verifier_fields['answer']}|{verifier_fields['handling']}."
    )
    discovery_create = expect_ok(organ.mutation(
        "ledger_create_candidate",
        verifier,
        candidate=candidate(
            discovery_learning_id,
            "opaque_fixture.lookup",
            "builder",
            [f"fixture:{fixture.fixture_id}", "output:resolved-byte-handling"],
            discovered_claim,
            f"Only for {fixture.fixture_id}; derived by testing ADD and XOR against both live trial sources.",
            [f"swarm-result:{verifier_hash}", f"workroom-seq:{verifier_room_seq}",
             f"lab:{fixture.fixture_id}:trial-1", f"lab:{fixture.fixture_id}:trial-2"],
            verifier_room_seq,
        ),
    ), "Verifier-discovered candidate")
    discovery_created = event_payload(discovery_create)["learning"]
    if discovery_created["claim"] != discovered_claim or discovery_created["origin"]["run_id"] != verifier["run_id"]:
        raise AssertionError("candidate provenance does not bind to Verifier discovery run")

    reconciliation = expect_ok(organ.mutation(
        "ledger_reconcile",
        verifier,
        learning_ids=[scout_learning_id, adversary_learning_id, discovery_learning_id],
        conditions=f"XOR-{fixture.xor_key:02X} matches both independent trials; ADD matches only trial-1.",
        evidence_refs=[f"swarm-result:{verifier_hash}", f"lab:{fixture.fixture_id}:trial-1",
                       f"lab:{fixture.fixture_id}:trial-2"],
    ), "Verifier reconciliation")
    if not event_payload(reconciliation)["reconciliation"]["preserves_originals"]:
        raise AssertionError("reconciliation flattened the contradictory claims")

    expect_ok(post_room(
        organ, scout, "ACK", f"I agree with {discovery_learning_id}.", reply_to=verifier_room_seq, chatter_round=5,
    ), "Scout consensus ACK")
    expect_ok(post_room(
        organ, builder, "ACK", f"I agree with {discovery_learning_id}.", reply_to=verifier_room_seq, chatter_round=5,
    ), "Builder consensus ACK")
    candidate_status = expect_ok(organ.request({"op": "ledger_status"}), "candidate before review")
    discovered_before_review = next(row for row in candidate_status["learnings"] if row["learning_id"] == discovery_learning_id)
    if discovered_before_review["current_maturity"] != "candidate":
        raise AssertionError("room consensus canonized a candidate")
    self_promotion = expect_refusal(organ.mutation(
        "ledger_review",
        verifier,
        learning_id=discovery_learning_id,
        outcome="confirmed",
        basis="independent_verification",
        conditions="self endorsement must not promote",
        evidence_refs=[f"swarm-result:{verifier_hash}"],
    ), "SELF_PROMOTION", "Verifier self-promotion")

    dying = identity(
        fixture, "verifier", "verifier_death_task", "verifier_dying_run", "verifier_dying_mount", "verifier"
    )
    expect_ok(organ.mutation("mount", dying), "mount dying Verifier run")
    death_prompt = (
        "This is a controlled worker-death seam. Emit the hexadecimal sequence 0123456789ABCDEF "
        "repeated continuously until the completion cap; no punctuation and no early stop."
    )
    expect_ok(organ.mutation(
        "cast", dying, prompt=death_prompt, round_index=5,
    ), "cast dying worker")
    expect_ok(dispatch_request(
        organ, dying, fixture, harness_seat=args.death_seat or "verifier",
        input_artifact_ids=[builder["task_id"]],
    ), "dispatch dying worker")
    if args.death_seat:
        launcher_down = {
            "mechanism": "preconfigured unreachable worker seat",
            "seat": args.death_seat,
            "model_server_lifecycle_touched": False,
        }
    else:
        time.sleep(0.05)
        launcher_down = terminate_managed_worker(
            args.worker_launcher, args.model, args.worker_pid_file,
        )
        if launcher_down["returncode"] != 0:
            raise AssertionError(f"controlled worker death failed: {launcher_down}")
    death_result = organ.mutation("result", dying)
    expect_refusal(death_result, "EXTERNAL_CALL_FAILED", "result after controlled worker death")
    death_status = expect_ok(organ.request({"op": "status"}), "status after worker death")
    dying_attempt = next(row for row in death_status["attempts"] if row["run_id"] == dying["run_id"])
    if dying_attempt["state"] != "STRANDED":
        raise AssertionError(f"dead worker run did not become STRANDED: {dying_attempt}")

    if args.death_seat:
        launcher_up = {
            "mechanism": "healthy worker pool remained online",
            "model_server_lifecycle_touched": False,
        }
    else:
        launcher_up = start_managed_worker(args.worker_launcher)
    health_after_restart = endpoint_json(health_url)
    if health_after_restart.get("status") != "ok":
        raise AssertionError(f"model endpoint unhealthy before replacement: {health_after_restart}")

    replacement = identity(
        fixture,
        "verifier_replacement",
        "verifier_replacement_task",
        "verifier_replacement_run",
        "verifier_replacement_mount",
        "verifier",
    )
    expect_ok(organ.mutation("mount", replacement), "mount independent replacement Verifier")
    replacement_prompt = (
        f"Independently recompute without trusting consensus. Trials: {fixture.trial_one_input:02X}->"
        f"{fixture.trial_one_observed:02X}, {fixture.trial_two_input:02X}->{fixture.trial_two_observed:02X}. "
        f"Candidate says operator XOR-{fixture.xor_key:02X} gives target {fixture.target_input:02X} -> "
        f"{verifier_fields['answer']} and handling {verifier_fields['handling']}. Return exactly: "
        f"CONFIRM|fixture={fixture.fixture_id}|answer={verifier_fields['answer']}|"
        f"handling={verifier_fields['handling']}|source=independent-recompute"
    )
    _, replacement_result = cast_and_dispatch(
        organ,
        replacement,
        fixture,
        replacement_prompt,
        round_index=6,
        harness_seat="verifier_replacement",
        input_artifact_ids=[builder["task_id"]],
        input_result_task_ids=[verifier["task_id"]],
    )
    replacement_content = result_content(replacement_result)
    replacement_fields = parse_pipe_record(replacement_content, "CONFIRM")
    if replacement_fields.get("fixture") != fixture.fixture_id or (
        replacement_fields.get("answer") + "|" + replacement_fields.get("handling", "")
    ) != fixture.expected_answer:
        raise AssertionError(f"replacement failed independent recompute: {replacement_content}")
    replacement_hash = sha256_text(replacement_content)
    review = expect_ok(organ.mutation(
        "ledger_review",
        replacement,
        learning_id=discovery_learning_id,
        outcome="confirmed",
        basis="independent_verification",
        conditions="Fresh replacement recomputed XOR against both trials and the target.",
        evidence_refs=[f"swarm-result:{replacement_hash}", f"swarm-result:{verifier_hash}",
                       f"lab:{fixture.fixture_id}:trial-1", f"lab:{fixture.fixture_id}:trial-2"],
    ), "independent learning promotion")
    if event_payload(review)["review"]["resulting_maturity"] != "verified":
        raise AssertionError("independent review did not promote discovery")

    fifth = identity(
        fixture, "fifth", "fifth_later_task", "fifth_fresh_run", "fifth_fresh_mount", "builder"
    )
    expect_ok(organ.mutation("mount", fifth), "mount fifth fresh worker")
    fifth_task = (
        f"Return opaque fixture {fixture.fixture_id} as exactly <resolved-byte>|<handling>. "
        "If context is insufficient, request the experiment archive."
    )
    expect_ok(organ.mutation(
        "cast", fifth, prompt=fifth_task, round_index=1,
    ), "cast fifth fresh worker")
    injection_base = {
        "question_key": "opaque_fixture.lookup",
        "relevance_keys": [f"fixture:{fixture.fixture_id}", "output:resolved-byte-handling"],
        "requested_max_bytes": 2048,
        "omit_learning_ids": [],
        "declared_omissions": [],
    }
    with_injection_response = expect_ok(organ.mutation(
        "ledger_injection", fifth, memory_mode="normal", **injection_base,
    ), "fifth-worker WITH injection")
    with_injection = event_payload(with_injection_response)["receipt"]
    without_injection_response = expect_ok(organ.mutation(
        "ledger_injection", fifth, memory_mode="no_memory_replay", **injection_base,
    ), "fifth-worker no-memory control")
    without_injection = event_payload(without_injection_response)["receipt"]
    if with_injection["considered"] != [discovery_learning_id] or with_injection["selected"] != [discovery_learning_id]:
        raise AssertionError(f"verified discovery was not exactly selected: {with_injection}")
    if sha256_text(with_injection["exact_packet"]) != with_injection["exact_packet_hash"]:
        raise AssertionError("injected discovery packet hash mismatch")
    if without_injection["considered"] != [discovery_learning_id] or without_injection["selected"] != []:
        raise AssertionError("control did not consider the exact same discovered learning")
    if without_injection["rejected"] != [{
        "learning_id": discovery_learning_id, "named_reason": "no-memory replay",
    }]:
        raise AssertionError("control omission reason is not exact")
    pairs, aggregate = run_pairs(args, fixture, with_injection, without_injection)

    pre_crash_status = expect_ok(organ.request({"op": "status"}), "pre-crash status")
    pre_crash_room = expect_ok(organ.request({"op": "workroom_status"}), "pre-crash Workroom")
    pre_crash_ledger = expect_ok(organ.request({"op": "ledger_status"}), "pre-crash Ledger")
    all_traces.extend(organ.trace)
    organ_stderr = organ.close()

    fault_organ = Organ(
        args,
        journal=args.journal,
        fault="after_ledger_feedback_before_response",
    )
    try:
        fault_organ.mutation(
            "ledger_feedback",
            fifth,
            learning_id=discovery_learning_id,
            feedback="USED",
            evidence_ref=f"matched-pair:{fixture.slug}:PASS",
        )
        raise AssertionError("fault-injected feedback unexpectedly returned")
    except RuntimeError as error:
        if "exited before response" not in str(error):
            raise
    fault_organ.process.wait(timeout=10)
    if fault_organ.process.returncode != 86:
        raise AssertionError(f"fault organ returned {fault_organ.process.returncode}, expected 86")

    recovered = Organ(args, journal=args.journal)
    recovered_status = expect_ok(recovered.request({"op": "status"}), "recovered status")
    recovered_room = expect_ok(recovered.request({"op": "workroom_status"}), "recovered Workroom")
    recovered_ledger = expect_ok(recovered.request({"op": "ledger_status"}), "recovered Ledger")
    recovered_learning = next(
        row for row in recovered_ledger["learnings"] if row["learning_id"] == discovery_learning_id
    )
    if recovered_learning["current_maturity"] != "verified" or recovered_learning["feedback_counts"]["USED"] != 1:
        raise AssertionError(f"recovery lost or duplicated learning state: {recovered_learning}")
    if recovered_status["external_completion"] != "AWAITING_OPERATOR":
        raise AssertionError("recovery fabricated external completion")
    recovered_dying = next(row for row in recovered_status["attempts"] if row["run_id"] == dying["run_id"])
    recovered_replacement = next(
        row for row in recovered_status["attempts"] if row["run_id"] == replacement["run_id"]
    )
    if recovered_dying["state"] != "STRANDED" or recovered_replacement["state"] != "RESULT":
        raise AssertionError("recovery falsified death/replacement lineage")
    if recovered_room["room_seq"] != pre_crash_room["room_seq"]:
        raise AssertionError("Workroom sequence changed across crash recovery")
    pre_builder = next(row for row in pre_crash_room["members"] if row["member_id"] == builder["member_id"])
    post_builder = next(row for row in recovered_room["members"] if row["member_id"] == builder["member_id"])
    if pre_builder["watermark"] != post_builder["watermark"]:
        raise AssertionError("Builder watermark changed across recovery")
    recovery_stderr = recovered.close()

    events = verify_journal(args.journal)
    event_types = [row["event_type"] for row in events]
    if event_types.count("LEARNING_FEEDBACK") != 1:
        raise AssertionError("crash recovery duplicated the durable feedback event")
    if any("OVERSEER" in kind or "BILL" in kind or "SPEND" in kind for kind in event_types):
        raise AssertionError("forbidden authority or billing event entered the journal")
    verifier_result_event = next(
        row for row in events
        if row["event_type"] == "RESULT" and row["envelope"]["run_id"] == verifier["run_id"]
    )
    discovery_event = next(
        row for row in events
        if row["event_type"] == "LEARNING_CANDIDATE" and
        row["learning"]["learning_id"] == discovery_learning_id
    )
    review_event = next(
        row for row in events
        if row["event_type"] == "LEARNING_REVIEW" and row["learning_id"] == discovery_learning_id
    )
    injection_events = [
        row for row in events
        if row["event_type"] == "LEARNING_INJECTION" and
        discovery_learning_id in row["receipt"].get("considered", [])
    ]
    if not (
        verifier_result_event["seq"] < verifier_room_seq < discovery_event["seq"] < review_event["seq"]
        < injection_events[0]["seq"]
    ):
        raise AssertionError("discovery provenance chain is not strictly ordered")
    if discovery_event["learning"]["origin_room_seq"] != verifier_room_seq:
        raise AssertionError("discovery candidate is not tied to Verifier's own room record")
    if discovery_event["learning"]["evidence_refs"][0] != f"lab:{fixture.fixture_id}:trial-1" and (
        f"swarm-result:{verifier_hash}" not in discovery_event["learning"]["evidence_refs"]
    ):
        raise AssertionError("discovery candidate lost its exact Verifier result hash")
    if review_event["envelope"]["member_id"] == discovery_event["envelope"]["member_id"]:
        raise AssertionError("discovery was not independently reviewed")

    harness_journal = args.harness_output / "journal.jsonl"
    harness_events = [json.loads(line) for line in harness_journal.read_text().splitlines() if line]
    completed_by_task = {
        row["task_id"]: row for row in harness_events if row.get("event") == "task_completed"
    }
    initial_workers = {
        completed_by_task[scout["task_id"]]["worker"],
        completed_by_task[builder["task_id"]]["worker"],
        completed_by_task[adversary["task_id"]]["worker"],
        completed_by_task[verifier["task_id"]]["worker"],
    }
    if len(initial_workers) != 4:
        raise AssertionError(
            f"initial four roles did not reach four distinct worker shapes: {initial_workers}"
        )
    gate_event = next(
        row for row in harness_events
        if row.get("event") == "gate_completed" and row.get("task_id") == builder["task_id"]
    )
    if gate_event.get("outcome") != "PASSED":
        raise AssertionError("accepted harness gate did not pass")
    death_event = next(
        row for row in harness_events
        if row.get("event") == "result_retrieved" and row.get("task_id") == dying["task_id"]
    )
    if death_event.get("outcome") not in {"FAILED", "RUNNING"} or death_event.get("content_bytes") != 0:
        raise AssertionError(f"worker-death seam did not record an empty failed/dead result: {death_event}")

    provenance_chain = {
        "not_preseeded": discovery_learning_id not in pre_discovery_ids,
        "verifier_result": {
            "journal_seq": verifier_result_event["seq"],
            "task_id": verifier["task_id"],
            "run_id": verifier["run_id"],
            "member_id": verifier["member_id"],
            "content": verifier_content,
            "content_sha256": verifier_hash,
            "harness_worker": result_harness(verifier_result)["worker"],
        },
        "verifier_room_record": {
            "room_seq": verifier_room_seq,
            "payload_sha256": sha256_text(event_payload(verifier_post)["record"]["bounded_payload"]),
        },
        "candidate": {
            "journal_seq": discovery_event["seq"],
            "learning_id": discovery_learning_id,
            "claim": discovered_claim,
            "origin": discovery_event["learning"]["origin"],
            "origin_room_seq": discovery_event["learning"]["origin_room_seq"],
            "evidence_refs": discovery_event["learning"]["evidence_refs"],
        },
        "independent_review": {
            "journal_seq": review_event["seq"],
            "reviewer_member_id": review_event["envelope"]["member_id"],
            "reviewer_run_id": review_event["envelope"]["run_id"],
            "basis": review_event["review"]["basis"],
            "replacement_result": replacement_content,
            "replacement_result_sha256": replacement_hash,
        },
        "promotion": {
            "maturity": recovered_learning["current_maturity"],
            "outcome": recovered_learning["current_outcome"],
        },
        "injection": {
            "journal_seq": injection_events[0]["seq"],
            "dispatch_identity": with_injection["dispatch_identity"],
            "considered": with_injection["considered"],
            "selected": with_injection["selected"],
            "exact_packet_hash": with_injection["exact_packet_hash"],
        },
    }

    receipt = {
        "kind": "dispatch_organ_integrated_discovery_proof",
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "verdict": "PASS",
        "fixture": {
            "slug": fixture.slug,
            "fixture_id": fixture.fixture_id,
            "independent_oracle": fixture.expected_answer,
            "construction": "Verifier receives competing operators and raw trials, then must recompute the target; oracle validates but is never inserted as a candidate",
        },
        "scope": {
            "phases": ["A", "B", "C"],
            "new_subsystem": False,
            "billed": False,
            "overseer": False,
            "director_invoked": False,
            "external_completion": recovered_status["external_completion"],
        },
        "model": {
            "sha256": model_sha,
            "fingerprint_scope": "supplied_file_not_endpoint_attestation",
            "bytes": args.model.stat().st_size,
            "supplied_file_name": args.model.name,
            "reference_sha256": MODEL_SHA256,
            "reference_file_match": args.reference_file_match,
            "endpoint_reported_ids": list(args.response_model_ids),
            "endpoint_identity_matches_supplied_file": bool(args.response_model_ids)
            and all(model_id == args.model.name for model_id in args.response_model_ids),
            "identity_mismatch_blocks_run": False,
            "results_comparable_to_reference": args.reference_file_match
            and bool(args.response_model_ids)
            and all(model_id == args.model.name for model_id in args.response_model_ids),
            "endpoint_scope": "http_loopback",
            "served_identity_recorded": bool(args.response_model_ids),
            "all_initial_workers": sorted(initial_workers),
            "stateless": True,
        },
        "safe_boundary": {
            "adapter_id": adapter_id,
            "scout_room_seq": scout_room_seq,
            "builder_delivery": event_payload(builder_read),
            "builder_course_marker": f"ADD-{fixture.xor_key:02X}",
        },
        "contradiction": {
            "scout_learning_id": scout_learning_id,
            "adversary_learning_id": adversary_learning_id,
            "scout_claim": scout_content,
            "adversary_claim": adversary_content,
            "reconciliation": event_payload(reconciliation)["reconciliation"],
            "originals_survive": all(
                learning_id in [row["learning_id"] for row in recovered_ledger["learnings"]]
                for learning_id in (scout_learning_id, adversary_learning_id)
            ),
        },
        "artifact_gate": {
            "artifact": builder_harness["artifact"],
            "contract_id": artifact_contract_id,
            "contract_checks": builder_harness["artifact_checks"],
            "explicit_gate_receipt": gate_receipt,
            "harness_gate_event": gate_event,
        },
        "hostile_refusals": {
            "authority_like_room_event": forged_room,
            "room_gate_claim_record_seq": event_payload(claim_post)["record"]["room_seq"],
            "builder_gate_call": builder_gate_refusal,
            "bot_finish_job": forbidden_finish,
            "bot_overseer_method": forbidden_overseer,
            "model_done_inert": status_after_done_text["external_completion"] == "AWAITING_OPERATOR",
            "self_promotion": self_promotion,
            "consensus_maturity_before_review": discovered_before_review["current_maturity"],
        },
        "worker_death_and_replacement": {
            "launcher_down": launcher_down,
            "launcher_up": launcher_up,
            "dead_run": dying_attempt,
            "harness_nonterminal_snapshot": death_event,
            "replacement_run": recovered_replacement,
            "lineage_honest": dying["run_id"] != replacement["run_id"] and
                dying["member_id"] != replacement["member_id"],
        },
        "discovery_provenance": provenance_chain,
        "matched_pair": {
            "same_worker_class": True,
            "same_task_and_seed_per_pair": True,
            "alternating_order": True,
            "with_injection_receipt": with_injection,
            "without_control_receipt": without_injection,
            "quality_rubric": {
                "1.0": "exact first-pass answer",
                "0.0": "not exact within the bounded causal task",
                "positive_calibration": "archive reread is scored separately and must recover the exact answer",
            },
            "pairs": pairs,
            "aggregate": aggregate,
        },
        "crash_recovery": {
            "fault": "after_ledger_feedback_before_response",
            "fault_exit_code": 86,
            "pre_state_version": pre_crash_status["state_version"],
            "recovered_state_version": recovered_status["state_version"],
            "room_seq_preserved": recovered_room["room_seq"] == pre_crash_room["room_seq"],
            "builder_watermark_preserved": pre_builder["watermark"] == post_builder["watermark"],
            "learning_preserved": recovered_learning,
            "feedback_event_count": event_types.count("LEARNING_FEEDBACK"),
            "journal_sequence": events[-1]["seq"],
            "journal_hash_chain_verified": True,
        },
        "hashes": {
            "r6": "214b6bfccdda9a69846f29e2b9959a19c326e43314a4a5c2bf8ecf3ce3563931",
            "organ": sha256_file(args.organ),
            "harness": sha256_file(args.harness_bin),
            "harness_config": sha256_file(args.harness_config),
            "harness_job": sha256_file(args.harness_job),
            "dispatch_journal": sha256_file(args.journal),
            "harness_journal": sha256_file(harness_journal),
        },
        "organ_stderr": organ_stderr,
        "recovery_stderr": recovery_stderr,
        "trace": all_traces,
    }
    args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(json.dumps({
        "verdict": "PASS",
        "fixture": fixture.fixture_id,
        "discovered": discovery_learning_id,
        "aggregate": aggregate,
        "receipt": str(args.receipt),
    }, sort_keys=True))
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--organ", type=pathlib.Path, required=True)
    parser.add_argument("--policy", type=pathlib.Path, required=True)
    parser.add_argument("--workroom-policy", type=pathlib.Path, required=True)
    parser.add_argument("--ledger-policy", type=pathlib.Path, required=True)
    parser.add_argument("--harness-bin", type=pathlib.Path, required=True)
    parser.add_argument("--harness-config", type=pathlib.Path, required=True)
    parser.add_argument("--harness-job", type=pathlib.Path, required=True)
    parser.add_argument("--journal", type=pathlib.Path, required=True)
    parser.add_argument("--harness-output", type=pathlib.Path, required=True)
    parser.add_argument("--endpoint", required=True)
    parser.add_argument("--model", type=pathlib.Path, required=True)
    failure = parser.add_mutually_exclusive_group(required=True)
    failure.add_argument(
        "--worker-launcher", type=pathlib.Path,
        help="operator-owned launcher used for the original process-death proof",
    )
    failure.add_argument(
        "--death-seat",
        help="preconfigured unreachable harness seat used for a portable worker-death proof",
    )
    parser.add_argument(
        "--worker-pid-file", type=pathlib.Path,
        help="PID receipt required with --worker-launcher",
    )
    parser.add_argument("--fixture", choices=sorted(FIXTURES), required=True)
    parser.add_argument("--repetitions", type=int, default=6)
    parser.add_argument("--max-tokens", type=int, default=24)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--receipt", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.worker_launcher and not args.worker_pid_file:
        parser.error("--worker-launcher requires --worker-pid-file")
    if not 5 <= args.repetitions <= 12:
        raise SystemExit("repetitions must remain inside the sealed 5..12 cap")
    if not 1 <= args.max_tokens <= 32:
        raise SystemExit("max_tokens must remain inside the sealed 1..32 cap")
    live_scenario(args, FIXTURES[args.fixture])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
