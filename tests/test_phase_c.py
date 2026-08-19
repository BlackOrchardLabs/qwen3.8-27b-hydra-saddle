#!/usr/bin/env python3
"""Process-level Phase C Ledger boundaries and non-vacuous injection canary."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile


def capabilities(role: str) -> list[str]:
    return {
        "builder": ["read_assignment", "submit_result"],
        "verifier": ["read_assignment", "request_gate", "submit_verification"],
    }[role]


def identity(member: str, task: str, mount: str, role: str, run: str | None = None) -> dict:
    return {
        "member_id": member,
        "seat_backend": "local_qwen_swarm",
        "epistemic_role": role,
        "authority_mode": "bot",
        "capability_set": capabilities(role),
        "thread_id": "thread_phase_c",
        "job_id": "phase_a_live",
        "task_id": task,
        "run_id": run or f"run_{task}",
        "mount_id": mount,
        "job_epoch": 1,
    }


def candidate(keys: list[str]) -> dict:
    return {
        "learning_id": "process_canary",
        "question_key": "cirrus.checksum",
        "role_scope": "builder",
        "relevance_keys": keys,
        "claim": "The cirrus checksum is 7F3A.",
        "limits": "Use only for the process canary fixture.",
        "evidence_refs": ["test:independent-process-oracle"],
        "outcome": "unknown",
        "confidence": 0.99,
        "review_after": 100000,
        "injection_budget": 1024,
    }


class Organ:
    def __init__(
        self,
        arguments: argparse.Namespace,
        root: pathlib.Path,
        journal: pathlib.Path,
        *,
        ledger: bool,
        harness: bool = False,
        fault: str | None = None,
    ) -> None:
        command = [
            arguments.organ,
            "--policy", arguments.policy,
            "--workroom-policy", arguments.workroom_policy,
            "--journal", str(journal),
        ]
        if ledger:
            command.extend(["--ledger-policy", arguments.ledger_policy])
        if harness:
            command.extend([
                "--harness-bin", arguments.fake_harness,
                "--harness-config", arguments.policy,
                "--harness-job", arguments.job,
                "--harness-output", str(root / "fake-harness-output"),
            ])
        environment = os.environ.copy()
        if fault:
            environment["DISPATCH_ORGAN_FAULT_POINT"] = fault
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )

    def request(self, value: dict) -> dict:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(json.dumps(value, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise RuntimeError(f"organ exited before response: {stderr}")
        return json.loads(line)

    def close(self) -> None:
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        self.process.wait(timeout=5)
        if self.process.returncode != 0:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise AssertionError(f"organ exited {self.process.returncode}: {stderr}")


def expect(response: dict, ok: bool, code: str | None = None) -> dict:
    assert response["ok"] is ok, response
    if code is not None:
        assert response["code"] == code, response
    return response


def injection_request(version: int, target: dict, mode: str = "normal") -> dict:
    return {
        "op": "ledger_injection",
        "expected_version": version,
        "envelope": target,
        "question_key": "cirrus.checksum",
        "relevance_keys": ["domain:cirrus", "format:hex"],
        "memory_mode": mode,
        "requested_max_bytes": 2048,
        "omit_learning_ids": [],
        "declared_omissions": [],
    }


def phase_fence(arguments: argparse.Namespace, root: pathlib.Path) -> None:
    organ = Organ(arguments, root, root / "phase-b-only.jsonl", ledger=False)
    expect(organ.request({"op": "ledger_status"}), False, "UNKNOWN_OPERATION")
    expect(organ.request({"op": "arm_billed"}), False, "UNKNOWN_OPERATION")
    expect(organ.request({"op": "overseer_cast"}), False, "UNKNOWN_OPERATION")
    organ.close()
    print("PASS process_phase_c_explicit_mount_and_later_phase_fences")


def ledger_path(arguments: argparse.Namespace, root: pathlib.Path) -> None:
    journal = root / "ledger.jsonl"
    organ = Organ(arguments, root, journal, ledger=True, harness=True)
    author = identity("author", "task_author", "mount_author", "verifier", "same_run")
    verifier = identity("independent", "task_verifier", "mount_verifier", "verifier")
    target = identity("target", "task_target", "mount_target", "builder")
    version = 0
    for mounted in (author, verifier, target):
        response = expect(organ.request({
            "op": "mount", "expected_version": version, "envelope": mounted,
        }), True)
        version = response["state_version"]
    response = expect(organ.request({
        "op": "cast", "expected_version": version, "envelope": target,
        "prompt": "Return only the cirrus checksum.", "round_index": 1,
    }), True)
    version = response["state_version"]

    before_refusal = version
    expect(organ.request({
        "op": "ledger_create_candidate", "expected_version": version,
        "envelope": author, "candidate": candidate([]),
    }), False, "MISSING_RETRIEVAL_KEYS")
    assert organ.request({"op": "ledger_status"})["state_version"] == before_refusal

    response = expect(organ.request({
        "op": "ledger_create_candidate", "expected_version": version,
        "envelope": author, "candidate": candidate(["format:hex", "domain:cirrus"]),
    }), True)
    version = response["state_version"]
    assert response["receipt"]["learning"]["maturity"] == "candidate"

    expect(organ.request({
        "op": "ledger_review", "expected_version": version, "envelope": author,
        "learning_id": "process_canary", "outcome": "confirmed",
        "basis": "independent_verification", "conditions": "self review",
        "evidence_refs": ["test:self"],
    }), False, "SELF_PROMOTION")
    response = expect(organ.request({
        "op": "ledger_review", "expected_version": version, "envelope": verifier,
        "learning_id": "process_canary", "outcome": "confirmed",
        "basis": "mechanical_outcome", "conditions": "oracle equals 7F3A",
        "evidence_refs": ["test:independent-process-oracle"],
    }), True)
    version = response["state_version"]

    # Hand-pinned independently of the selector implementation.
    oracle_eligible = ["process_canary"]
    assert oracle_eligible
    response = expect(organ.request(injection_request(version, target)), True)
    version = response["state_version"]
    receipt = response["receipt"]["receipt"]
    assert receipt["selected"] == oracle_eligible, receipt
    assert receipt["exact_packet"], receipt
    assert hashlib.sha256(receipt["exact_packet"].encode()).hexdigest() == receipt["exact_packet_hash"]

    response = expect(organ.request(injection_request(version, target, "no_memory_replay")), True)
    version = response["state_version"]
    assert response["receipt"]["receipt"]["selected"] == []
    assert response["receipt"]["receipt"]["rejected"][0]["named_reason"] == "no-memory replay"

    silent = injection_request(version, target)
    silent["omit_learning_ids"] = ["process_canary"]
    expect(organ.request(silent), False, "SILENT_OMISSION")
    assert organ.request({"op": "ledger_status"})["state_version"] == version

    expect(organ.request({
        "op": "ledger_protected_namespace_write", "learning_id": "process_canary",
        "protected_namespace": "protected/operator/archive",
    }), False, "PROTECTED_NAMESPACE_FORBIDDEN")

    dispatch = injection_request(version, target)
    dispatch["op"] = "dispatch"
    response = expect(organ.request(dispatch), True)
    version = response["state_version"]
    dispatch_receipt = response["injection_receipt"]
    assert dispatch_receipt["considered"] == ["process_canary"]
    assert dispatch_receipt["selected"] == ["process_canary"]
    assert hashlib.sha256(dispatch_receipt["exact_packet"].encode()).hexdigest() == dispatch_receipt["exact_packet_hash"]
    response = expect(organ.request({
        "op": "result", "expected_version": version, "envelope": target,
    }), True)
    version = response["state_version"]
    assert organ.request({"op": "status"})["external_completion"] == "AWAITING_OPERATOR"
    expect(organ.request({"op": "arm_billed"}), False, "UNKNOWN_OPERATION")
    expect(organ.request({"op": "overseer_cast"}), False, "UNKNOWN_OPERATION")
    organ.close()

    restarted = Organ(arguments, root, journal, ledger=True)
    status = expect(restarted.request({"op": "ledger_status"}), True)
    assert status["state_version"] == version
    assert len(status["learnings"]) == 1
    assert len(status["injection_receipts"]) == 3
    restarted.close()
    print("PASS process_candidate_canary_control_silent_omission_protected_namespace_dispatch_restart")


def crash_path(arguments: argparse.Namespace, root: pathlib.Path) -> None:
    journal = root / "ledger-crash.jsonl"
    author = identity("crash_author", "crash_task", "crash_mount", "builder")
    organ = Organ(
        arguments, root, journal, ledger=True,
        fault="after_ledger_append_before_response",
    )
    response = expect(organ.request({
        "op": "mount", "expected_version": 0, "envelope": author,
    }), True)
    crashing_candidate = candidate(["format:hex", "domain:cirrus"])
    crashing_candidate["learning_id"] = "crash_candidate"
    try:
        organ.request({
            "op": "ledger_create_candidate", "expected_version": response["state_version"],
            "envelope": author, "candidate": crashing_candidate,
        })
        raise AssertionError("fault-injected Ledger append unexpectedly returned")
    except RuntimeError as error:
        assert "exited before response" in str(error), error
    organ.process.wait(timeout=5)
    assert organ.process.returncode == 86, organ.process.returncode

    recovered = Organ(arguments, root, journal, ledger=True)
    status = expect(recovered.request({"op": "ledger_status"}), True)
    assert status["state_version"] == 2, status
    assert len(status["learnings"]) == 1, status
    assert status["learnings"][0]["learning_id"] == "crash_candidate", status
    assert status["learnings"][0]["created_seq"] == 2, status
    sequences = [json.loads(line)["seq"] for line in journal.read_text().splitlines() if line]
    assert sequences == [1, 2], sequences
    recovered.close()
    print("PASS process_crash_after_durable_ledger_append_replay_exactly_once")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--organ", required=True)
    parser.add_argument("--fake-harness", required=True)
    parser.add_argument("--policy", required=True)
    parser.add_argument("--job", required=True)
    parser.add_argument("--workroom-policy", required=True)
    parser.add_argument("--ledger-policy", required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dispatch-organ-phase-c-") as temporary:
        root = pathlib.Path(temporary)
        phase_fence(arguments, root)
        ledger_path(arguments, root)
        crash_path(arguments, root)
    print("phase_c_process_boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
