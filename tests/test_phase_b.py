#!/usr/bin/env python3
"""Process-level Phase B Workroom attacks."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import tempfile


def identity(member: str, task: str, mount: str, role: str = "builder") -> dict:
    tools = {
        "builder": ["read_assignment", "submit_result"],
        "scout": ["read_assignment", "submit_observation"],
    }[role]
    return {
        "member_id": member,
        "seat_backend": "local_qwen_swarm",
        "epistemic_role": role,
        "authority_mode": "bot",
        "capability_set": tools,
        "thread_id": "thread_room",
        "job_id": "job_room",
        "task_id": task,
        "run_id": f"run_{task}",
        "mount_id": mount,
        "job_epoch": 1,
    }


def post_request(version: int, envelope: dict, payload: str = "hello", event_type: str = "OBSERVATION") -> dict:
    return {
        "op": "workroom_post",
        "expected_version": version,
        "envelope": envelope,
        "event_type": event_type,
        "reply_to": None,
        "bounded_payload": payload,
        "group_tags": [],
        "chatter_round": 1,
    }


class Organ:
    def __init__(
        self,
        arguments: argparse.Namespace,
        journal: pathlib.Path,
        workroom_policy: pathlib.Path,
        fault: str | None = None,
    ) -> None:
        environment = os.environ.copy()
        if fault:
            environment["DISPATCH_ORGAN_FAULT_POINT"] = fault
        self.process = subprocess.Popen(
            [
                arguments.organ,
                "--policy", arguments.policy,
                "--workroom-policy", str(workroom_policy),
                "--journal", str(journal),
            ],
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
            raise EOFError("organ exited before response")
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


def member(status: dict, member_id: str) -> dict:
    return next(item for item in status["members"] if item["member_id"] == member_id)


def ordinary_path(arguments: argparse.Namespace, root: pathlib.Path, policy: pathlib.Path) -> None:
    journal = root / "ordinary.jsonl"
    organ = Organ(arguments, journal, policy)
    sender = identity("sender", "task_sender", "mount_sender")
    receiver = identity("receiver", "task_receiver", "mount_receiver", "scout")
    version = 0
    for mounted in (sender, receiver):
        response = expect(organ.request({"op": "mount", "expected_version": version, "envelope": mounted}), True)
        version = response["state_version"]

    expect(organ.request({"op": "prove_adapter", "expected_version": version, "adapter_id": "lying_immediate"}),
        False, "ADAPTER_CONFORMANCE_FAILED")
    response = expect(organ.request({
        "op": "prove_adapter", "expected_version": version, "adapter_id": "opaque_local_qwen"}), True)
    version = response["state_version"]
    response = expect(organ.request({
        "op": "set_group_tags", "expected_version": version, "envelope": sender, "group_tags": ["alpha"]}), True)
    version = response["state_version"]
    response = expect(organ.request({
        "op": "set_group_tags", "expected_version": version, "envelope": receiver, "group_tags": ["alpha"]}), True)
    version = response["state_version"]
    response = expect(organ.request({
        "op": "set_member_execution", "expected_version": version,
        "envelope": receiver, "execution_state": "OPAQUE_CALL"}), True)
    version = response["state_version"]

    grouped = post_request(version, sender, "queued safely", "QUESTION")
    grouped["group_tags"] = ["alpha"]
    response = expect(organ.request(grouped), True)
    version = response["state_version"]
    record = response["receipt"]["record"]
    for key in (
        "room_seq", "thread_id", "job_id", "task_id", "run_id", "member_id",
        "mount_id", "job_epoch", "timestamp", "event_type", "reply_to",
        "bounded_payload", "payload_hash",
    ):
        assert key in record, record

    expect(organ.request(post_request(version, sender, "authority claim", "GATE")), False, "ROOM_EVENT_FORBIDDEN")
    expect(organ.request(post_request(version, sender, "x" * 65)), False, "ROOM_PAYLOAD_CAP_EXCEEDED")
    over_round = post_request(version, sender)
    over_round["chatter_round"] = 3
    expect(organ.request(over_round), False, "ROOM_ROUND_CAP_EXCEEDED")
    expect(organ.request(post_request(version - 1, sender)), False, "STALE_PRECONDITION")

    expect(organ.request({
        "op": "workroom_read", "expected_version": version, "envelope": receiver,
        "adapter_id": "opaque_local_qwen", "safe_boundary": "BETWEEN_TURNS", "limit": 4,
    }), False, "NO_SAFE_BOUNDARY")
    queued = organ.request({"op": "workroom_status"})
    assert member(queued, "receiver")["watermark"] == 0, queued
    assert member(queued, "receiver")["unread_count"] == 1, queued

    response = expect(organ.request({
        "op": "set_member_execution", "expected_version": version,
        "envelope": receiver, "execution_state": "SAFE_BOUNDARY"}), True)
    version = response["state_version"]
    response = expect(organ.request({
        "op": "workroom_read", "expected_version": version, "envelope": receiver,
        "adapter_id": "opaque_local_qwen", "safe_boundary": "BETWEEN_TURNS", "limit": 4,
    }), True)
    version = response["state_version"]
    delivery = response["receipt"]
    assert len(delivery["delivered"]) == 1, delivery
    assert delivery["durably_observed"] is True and delivery["seen"] is False and delivery["understood"] is False
    response = expect(organ.request({
        "op": "workroom_read", "expected_version": version, "envelope": receiver,
        "adapter_id": "opaque_local_qwen", "safe_boundary": "BETWEEN_TURNS", "limit": 4,
    }), True)
    version = response["state_version"]
    assert response["receipt"]["delivered"] == [], response

    gate = organ.request({"op": "gate_evidence_view"})
    assert gate["type_filter"] == ["RESULT"] and gate["range_fold"] is False and gate["evidence"] == [], gate
    for forbidden in ("experience_submit", "arm_billed", "overseer_cast"):
        expect(organ.request({"op": forbidden}), False, "UNKNOWN_OPERATION")

    response = expect(organ.request({
        "op": "set_group_tags", "expected_version": version, "envelope": sender, "group_tags": []}), True)
    version = response["state_version"]
    response = expect(organ.request({
        "op": "set_group_tags", "expected_version": version, "envelope": receiver, "group_tags": []}), True)
    version = response["state_version"]
    final_status = organ.request({"op": "workroom_status"})
    assert "alpha" not in final_status["derived_groups"], final_status
    saved_room_seq = final_status["room_seq"]
    saved_watermark = member(final_status, "receiver")["watermark"]
    organ.close()

    restarted = Organ(arguments, journal, policy)
    replayed = restarted.request({"op": "workroom_status"})
    assert replayed["room_seq"] == saved_room_seq, replayed
    assert member(replayed, "receiver")["watermark"] == saved_watermark, replayed
    assert replayed["record_count"] == 1, replayed
    restarted.close()
    print("PASS process_valid_refusals_safe_delivery_groups_restart_phase_fences")


def ordering_path(arguments: argparse.Namespace, root: pathlib.Path, policy: pathlib.Path) -> None:
    organ = Organ(arguments, root / "ordering.jsonl", policy)
    revoked = identity("revoked", "task_revoked", "mount_revoked")
    stale = identity("stale", "task_stale", "mount_stale", "scout")
    version = 0
    for mounted in (revoked, stale):
        response = expect(organ.request({"op": "mount", "expected_version": version, "envelope": mounted}), True)
        version = response["state_version"]
    response = expect(organ.request({"op": "revoke_mount", "expected_version": version, "envelope": revoked}), True)
    version = response["state_version"]
    expect(organ.request(post_request(version, revoked, "must refuse")), False, "MOUNT_REVOKED")
    response = expect(organ.request({"op": "bump_job_epoch", "expected_version": version, "job_id": "job_room"}), True)
    version = response["state_version"]
    expect(organ.request(post_request(version, stale, "stale epoch")), False, "STALE_EPOCH")
    organ.close()
    print("PASS process_cross_surface_revoked_mount_and_stale_epoch")


def nonreader_path(arguments: argparse.Namespace, root: pathlib.Path, policy: pathlib.Path) -> None:
    organ = Organ(arguments, root / "nonreader.jsonl", policy)
    sender = identity("sender", "task_sender", "mount_sender")
    receiver = identity("receiver", "task_receiver", "mount_receiver", "scout")
    version = 0
    for mounted in (sender, receiver):
        response = expect(organ.request({"op": "mount", "expected_version": version, "envelope": mounted}), True)
        version = response["state_version"]
    response = expect(organ.request(post_request(version, sender, "one")), True)
    version = response["state_version"]
    response = expect(organ.request(post_request(version, sender, "two")), True)
    version = response["state_version"]
    assert "receiver" in response["receipt"]["skipped_members"], response
    response = expect(organ.request(post_request(version, sender, "three")), True)
    state = organ.request({"op": "workroom_status"})
    assert member(state, "receiver")["skipped_at_cap"] is True, state
    assert member(state, "receiver")["unread_count"] == 3, state
    organ.close()
    print("PASS process_nonreader_cap_skip_visible_lag_no_wedge")


def crash_path(arguments: argparse.Namespace, root: pathlib.Path, policy: pathlib.Path) -> None:
    journal = root / "crash.jsonl"
    organ = Organ(arguments, journal, policy, fault="after_room_append_before_response")
    sender = identity("sender", "task_sender", "mount_sender")
    receiver = identity("receiver", "task_receiver", "mount_receiver", "scout")
    version = 0
    for mounted in (sender, receiver):
        response = expect(organ.request({"op": "mount", "expected_version": version, "envelope": mounted}), True)
        version = response["state_version"]
    try:
        organ.request(post_request(version, sender, "durable before crash"))
        raise AssertionError("fault-injected Workroom append unexpectedly returned")
    except EOFError:
        pass
    organ.process.wait(timeout=5)
    assert organ.process.returncode == 86, organ.process.returncode

    recovered = Organ(arguments, journal, policy)
    status = recovered.request({"op": "workroom_status"})
    assert status["record_count"] == 1, status
    assert status["room_seq"] == 3, status
    sequences = [json.loads(line)["seq"] for line in journal.read_text().splitlines() if line]
    assert sequences == [1, 2, 3], sequences
    recovered.close()
    print("PASS process_crash_after_durable_room_append_replay_no_duplicate")


def commutative_path(arguments: argparse.Namespace, root: pathlib.Path, policy: pathlib.Path) -> None:
    organ = Organ(arguments, root / "commutative.jsonl", policy)
    for index in range(2):
        response = expect(organ.request({
            "op": "workroom_commutative_observation",
            "thread_id": "thread_room",
            "job_id": "job_room",
            "task_id": f"comm_task_{index}",
            "run_id": f"comm_run_{index}",
            "job_epoch": 1,
            "bounded_payload": f"commutative {index}",
        }), True)
        assert response["receipt"]["read_set"] == [], response
    organ.close()
    print("PASS process_commutative_observation_has_no_version_token")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--organ", required=True)
    parser.add_argument("--policy", required=True)
    parser.add_argument("--workroom-policy", required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dispatch-organ-phase-b-") as temporary:
        root = pathlib.Path(temporary)
        policy = root / "workroom-policy.json"
        configured = json.loads(pathlib.Path(arguments.workroom_policy).read_text())
        configured.update({
            "max_payload_bytes": 64,
            "max_posts_per_member": 8,
            "max_reads_per_member": 4,
            "max_chatter_rounds": 2,
            "max_unread_per_member": 2,
            "max_delivery_batch": 4,
        })
        policy.write_text(json.dumps(configured))
        ordinary_path(arguments, root, policy)
        ordering_path(arguments, root, policy)
        nonreader_path(arguments, root, policy)
        crash_path(arguments, root, policy)
        commutative_path(arguments, root, policy)
    print("phase_b_process_boundary: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
