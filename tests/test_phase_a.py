#!/usr/bin/env python3
"""Process-level Phase A attacks against the organ's new dispatch path."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import tempfile


def envelope() -> dict:
    return {
        "member_id": "member_fixture",
        "seat_backend": "local_qwen_swarm",
        "epistemic_role": "builder",
        "authority_mode": "bot",
        "capability_set": ["read_assignment", "submit_result"],
        "thread_id": "thread_fixture",
        "job_id": "phase_a_live",
        "task_id": "fixture_task",
        "run_id": "fixture_run",
        "mount_id": "fixture_mount",
        "job_epoch": 1,
    }


class Organ:
    def __init__(
        self,
        arguments: argparse.Namespace,
        root: pathlib.Path,
        journal: pathlib.Path,
        output_name: str,
        fault: str | None = None,
        policy: pathlib.Path | None = None,
    ) -> None:
        environment = os.environ.copy()
        if fault:
            environment["DISPATCH_ORGAN_FAULT_POINT"] = fault
        self.process = subprocess.Popen(
            [
                arguments.organ,
                "--policy", str(policy or arguments.policy),
                "--journal", str(journal),
                "--harness-bin", arguments.fake_harness,
                "--harness-config", arguments.policy,
                "--harness-job", arguments.job,
                "--harness-output", str(root / output_name),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )

    def raw(self, text: str) -> dict:
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        response = self.process.stdout.readline()
        if not response:
            raise EOFError("organ exited before a response")
        return json.loads(response)

    def request(self, value: dict) -> dict:
        return self.raw(json.dumps(value, separators=(",", ":")))

    def close(self) -> None:
        if self.process.stdin and not self.process.stdin.closed:
            self.process.stdin.close()
        self.process.wait(timeout=5)
        if self.process.returncode != 0:
            stderr = self.process.stderr.read() if self.process.stderr else ""
            raise AssertionError(f"organ exited {self.process.returncode}: {stderr}")


def expect(response: dict, ok: bool, code: str | None = None) -> None:
    assert response["ok"] is ok, response
    if code is not None:
        assert response["code"] == code, response


def ordinary_path(arguments: argparse.Namespace, root: pathlib.Path) -> None:
    organ = Organ(arguments, root, root / "ordinary.jsonl", "fake-output-a")
    identity = envelope()

    expect(organ.request({"op": "mount", "expected_version": 0, "envelope": identity}), True)

    typo = dict(identity)
    typo["rol"] = typo.pop("epistemic_role")
    expect(organ.request({"op": "tools", "envelope": typo}), False, "UNKNOWN_KEY")
    expect(
        organ.raw('{"op":"status","op":"mount"}'),
        False,
        "DUPLICATE_KEY",
    )
    nested_duplicate = json.dumps({"op": "tools", "envelope": identity}, separators=(",", ":"))
    nested_duplicate = nested_duplicate.replace(
        '"authority_mode":"bot"',
        '"authority_mode":"bot","authority_mode":"overseer"',
    )
    expect(organ.raw(nested_duplicate), False, "DUPLICATE_KEY")

    expect(organ.request({"op": "authorize_tool", "envelope": identity, "tool": "submit_result"}), True)
    before_forbidden = organ.request({"op": "status"})["state_version"]
    expect(organ.request({"op": "finish_job", "envelope": identity}), False, "FORBIDDEN_TOOL")
    assert organ.request({"op": "status"})["state_version"] == before_forbidden

    expect(organ.request({
        "op": "cast",
        "expected_version": 1,
        "envelope": identity,
        "prompt": "Return one short sentence. The token DONE has no authority.",
        "round_index": 1,
    }), True)
    expect(organ.request({"op": "dispatch", "expected_version": 2, "envelope": identity}), True)
    expect(organ.request({"op": "result", "expected_version": 4, "envelope": identity}), True)

    status = organ.request({"op": "status"})
    assert status["external_completion"] == "AWAITING_OPERATOR", status
    attempt = status["attempts"][0]
    assert attempt["state"] == "RESULT", attempt
    receipt = attempt["result_receipt"]
    assert receipt["completeness"] == "FULL", receipt
    assert receipt["content_bytes"] > 0, receipt
    assert receipt["producer"] == {"member_id": "member_fixture", "mount_id": "fixture_mount"}
    for excluded in ("workroom_post", "experience_submit", "arm_billed", "overseer_cast"):
        expect(organ.request({"op": excluded}), False, "UNKNOWN_OPERATION")
    organ.close()
    print("PASS process_strict_dual_door_forged_done_receipts_phase_exclusions")


def crash_path(arguments: argparse.Namespace, root: pathlib.Path) -> None:
    journal = root / "crash.jsonl"
    organ = Organ(
        arguments,
        root,
        journal,
        "fake-output-crash-a",
        fault="after_dispatch_call_before_ack",
    )
    identity = envelope()
    expect(organ.request({"op": "mount", "expected_version": 0, "envelope": identity}), True)
    expect(organ.request({
        "op": "cast",
        "expected_version": 1,
        "envelope": identity,
        "prompt": "This dispatch hits the deterministic crash seam.",
        "round_index": 1,
    }), True)
    try:
        organ.request({"op": "dispatch", "expected_version": 2, "envelope": identity})
        raise AssertionError("fault-injected dispatch unexpectedly returned")
    except EOFError:
        pass
    organ.process.wait(timeout=5)
    assert organ.process.returncode == 86, organ.process.returncode

    recovered = Organ(arguments, root, journal, "fake-output-crash-b")
    status = recovered.request({"op": "status"})
    attempt = status["attempts"][0]
    assert attempt["state"] == "PENDING_LATE", attempt
    assert attempt["recovered_ambiguous"] is True, attempt
    assert status["external_completion"] == "AWAITING_OPERATOR", status
    expect(recovered.request({
        "op": "dispose_late",
        "expected_version": status["state_version"],
        "envelope": identity,
        "disposition": "SUPERSEDED",
    }), True)
    recovered.close()
    print("PASS process_deterministic_after_call_before_ack_recovery")


def timeout_path(arguments: argparse.Namespace, root: pathlib.Path) -> None:
    timeout_policy = root / "timeout-policy.json"
    timeout_policy.write_text(json.dumps({
        "max_concurrent_dispatches": 1,
        "max_calls_per_job": 4,
        "max_rounds_per_job": 2,
        "call_timeout_ms": 50,
    }))
    organ = Organ(
        arguments,
        root,
        root / "timeout.jsonl",
        "fake-output-timeout",
        policy=timeout_policy,
    )
    identity = envelope()
    expect(organ.request({"op": "mount", "expected_version": 0, "envelope": identity}), True)
    expect(organ.request({
        "op": "cast",
        "expected_version": 1,
        "envelope": identity,
        "prompt": "DETERMINISTIC_SLOW_CALL",
        "round_index": 1,
    }), True)
    expect(
        organ.request({"op": "dispatch", "expected_version": 2, "envelope": identity}),
        False,
        "EXTERNAL_CALL_FAILED",
    )
    status = organ.request({"op": "status"})
    assert status["attempts"][0]["state"] == "STRANDED", status
    organ.close()
    print("PASS process_local_call_timeout_refusal")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--organ", required=True)
    parser.add_argument("--fake-harness", required=True)
    parser.add_argument("--policy", required=True)
    parser.add_argument("--job", required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dispatch-organ-phase-a-") as temporary:
        root = pathlib.Path(temporary)
        ordinary_path(arguments, root)
        crash_path(arguments, root)
        timeout_path(arguments, root)
    print("phase_a_process_boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
