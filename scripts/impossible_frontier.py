#!/usr/bin/env python3
"""Discovery-only task swap for the accepted integrated A+B+C proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import statistics

import integrated_proof as accepted


RESIDUE_BYTES = 435
MODEL_SHA256 = accepted.MODEL_SHA256
ORIGINAL_CANDIDATE = accepted.candidate
ORIGINAL_MUTATION = accepted.Organ.mutation

FIXTURES = {
    "vanta73": accepted.Fixture(
        slug="vanta73",
        fixture_id="VANTA-73",
        xor_key=0x01,
        trial_one_input=0x20,
        trial_one_observed=0x21,
        trial_two_input=0x01,
        trial_two_observed=0x00,
        target_input=0x10,
        handling="APPLY",
        expected_answer="11|APPLY",
        seed_base=47300,
    ),
    "lumen42": accepted.Fixture(
        slug="lumen42",
        fixture_id="LUMEN-42",
        xor_key=0x10,
        trial_one_input=0x01,
        trial_one_observed=0x11,
        trial_two_input=0x10,
        trial_two_observed=0x00,
        target_input=0x40,
        handling="APPLY",
        expected_answer="50|APPLY",
        seed_base=54200,
    ),
}


def canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def packet_for(candidate: dict) -> str:
    item = {
        "learning_id": candidate["learning_id"],
        "question_key": candidate["question_key"],
        "relevance_keys": candidate["relevance_keys"],
        "claim": candidate["claim"],
        "limits": candidate["limits"],
        "confidence": candidate["confidence"],
        "evidence_refs": candidate["evidence_refs"],
    }
    return canonical({"experience_ledger": [item], "schema": "dispatch-organ.phase-c/1"})


def frontier_candidate(
    learning_id: str,
    question_key: str,
    role_scope: str,
    relevance_keys: list[str],
    claim: str,
    limits: str,
    evidence_refs: list[str],
    origin_room_seq: int,
) -> dict:
    if not learning_id.endswith("_discovered_resolution"):
        return ORIGINAL_CANDIDATE(
            learning_id, question_key, role_scope, relevance_keys, claim,
            limits, evidence_refs, origin_room_seq,
        )
    fixture_id = relevance_keys[0].removeprefix("fixture:")
    oracle = claim.rsplit(" is ", 1)[-1].rstrip(".")
    exact_refs = [
        ref for ref in evidence_refs
        if ref.startswith("swarm-result:") or ref.startswith("workroom-seq:")
    ]
    result = ORIGINAL_CANDIDATE(
        learning_id,
        "f",
        role_scope,
        [f"f:{fixture_id}", "o:r"],
        claim,
        f"{fixture_id} only",
        exact_refs,
        origin_room_seq,
    )
    pad = RESIDUE_BYTES - len(packet_for(result).encode())
    if pad < 0:
        raise AssertionError(
            f"discovery packet base exceeds {RESIDUE_BYTES} bytes by {-pad}"
        )
    semantic_suffix = " no archive route"
    if pad != len(semantic_suffix.encode()):
        raise AssertionError(
            f"435-byte semantic limit sizing drift: need {pad}, suffix has {len(semantic_suffix.encode())}"
        )
    result["limits"] += semantic_suffix
    actual = len(packet_for(result).encode())
    if actual != RESIDUE_BYTES:
        raise AssertionError(f"packet sizing drift: expected {RESIDUE_BYTES}, got {actual}")
    return result


def frontier_mutation(self, op: str, envelope: dict, **kwargs) -> dict:
    if op == "ledger_injection":
        fixture_key = next(
            (key for key in kwargs["relevance_keys"] if key.startswith("fixture:")), None
        )
        if fixture_key is None:
            raise AssertionError("frontier injection lacks exact fixture relevance")
        fixture_id = fixture_key.removeprefix("fixture:")
        kwargs["question_key"] = "f"
        kwargs["relevance_keys"] = [f"f:{fixture_id}", "o:r"]
    return ORIGINAL_MUTATION(self, op, envelope, **kwargs)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def states_token(text: str, value: str) -> bool:
    return re.search(rf"(?<![0-9A-Za-z]){re.escape(value)}(?![0-9A-Za-z])", text) is not None


def run_discovery_only_pairs(
    args: argparse.Namespace,
    fixture: accepted.Fixture,
    with_receipt: dict,
    without_receipt: dict,
) -> tuple[list[dict], dict]:
    packet = with_receipt["exact_packet"]
    packet_hash = with_receipt["exact_packet_hash"]
    if len(packet.encode()) != RESIDUE_BYTES or sha256_text(packet) != packet_hash:
        raise AssertionError("verified discovery residue is not the exact 435-byte packet")
    if without_receipt["exact_packet"] != "":
        raise AssertionError("cold control unexpectedly carries residue bytes")

    task = (
        f"A completed collaborative experiment produced one opaque verified resolution for fixture "
        f"{fixture.fixture_id}. Return it exactly as <resolved-byte>|<handling>. No trials, transform "
        "rule, key, target, archive, tool, or lookup source are supplied in this task. If no supplied "
        "Experience Ledger residue states the resolution, return exactly UNKNOWN."
    )
    packet_block = (
        f"[EXPERIENCE_LEDGER_PACKET sha256={packet_hash}]\n"
        f"{packet}\n[/EXPERIENCE_LEDGER_PACKET]\n"
    )
    if fixture.expected_answer in task or fixture.expected_answer in accepted.SYSTEM:
        raise AssertionError("independent oracle leaked into common task or system prompt")
    if packet.count(fixture.expected_answer) != 1:
        raise AssertionError("verified residue must carry the exact discovered fact once")

    pairs: list[dict] = []
    for index in range(1, args.repetitions + 1):
        nonce = f"{fixture.slug}-frontier-{index:02d}"
        control_prompt = f"Evaluation nonce: {nonce}.\n{task}"
        with_prompt = packet_block + control_prompt
        if with_prompt.removeprefix(packet_block) != control_prompt:
            raise AssertionError("paired prompts differ by more than the residue block")
        if fixture.expected_answer in control_prompt:
            raise AssertionError("oracle leaked into cold control prompt")
        outside_packet = with_prompt[:with_prompt.index(packet)] + with_prompt[
            with_prompt.index(packet) + len(packet):
        ]
        if fixture.expected_answer in outside_packet:
            raise AssertionError("discovered fact appears outside residue in WITH prompt")

        seed = fixture.seed_base + index
        order = ["with", "without"] if index % 2 else ["without", "with"]
        prompts = {"with": with_prompt, "without": control_prompt}
        arms = {
            name: accepted.chat_for_args(
                args, seed, prompts[name], args.timeout_seconds, args.max_tokens,
            )
            for name in order
        }
        with_exact = arms["with"]["content"] == fixture.expected_answer
        without_exact = arms["without"]["content"] == fixture.expected_answer
        pair = {
            "pair_index": index,
            "seed": seed,
            "execution_order": order,
            "common": {
                "task": task,
                "task_sha256": sha256_text(task),
                "system_sha256": sha256_text(accepted.SYSTEM),
                "max_completion_tokens": args.max_tokens,
                "temperature": 0.0,
            },
            "with": {
                **arms["with"],
                "prompt": with_prompt,
                "prompt_sha256": sha256_text(with_prompt),
                "quality": 1.0 if with_exact else 0.0,
                "exact": with_exact,
                "calls": 1,
            },
            "without": {
                **arms["without"],
                "prompt": control_prompt,
                "prompt_sha256": sha256_text(control_prompt),
                "quality": 1.0 if without_exact else 0.0,
                "exact": without_exact,
                "stays_zero": not without_exact,
                "calls": 1,
            },
            "sole_carrier": {
                "prompt_difference_is_exact_residue_block": True,
                "residue_bytes": len(packet.encode()),
                "oracle_occurrences_in_residue": packet.count(fixture.expected_answer),
                "oracle_occurrences_in_common_task": task.count(fixture.expected_answer),
                "oracle_occurrences_in_control_prompt": control_prompt.count(fixture.expected_answer),
                "oracle_occurrences_outside_residue_in_with_prompt": outside_packet.count(
                    fixture.expected_answer
                ),
                "oracle_occurrences_in_with_prompt": with_prompt.count(fixture.expected_answer),
            },
        }
        pairs.append(pair)

    aggregate = {
        "repetitions": len(pairs),
        "with_quality_mean": statistics.mean(row["with"]["quality"] for row in pairs),
        "without_quality_mean": statistics.mean(row["without"]["quality"] for row in pairs),
        "with_exact": sum(row["with"]["exact"] for row in pairs),
        "without_exact": sum(row["without"]["exact"] for row in pairs),
        "without_stays_zero": sum(row["without"]["stays_zero"] for row in pairs),
        "with_tokens_total": sum(row["with"]["total_tokens"] for row in pairs),
        "without_tokens_total": sum(row["without"]["total_tokens"] for row in pairs),
        "reference_calls_total": 0,
        "calls_per_arm_per_pair": 1,
        "max_completion_tokens": args.max_tokens,
    }
    passed = (
        aggregate["with_quality_mean"] == 1.0
        and aggregate["without_quality_mean"] == 0.0
        and aggregate["with_exact"] == len(pairs)
        and aggregate["without_exact"] == 0
        and aggregate["without_stays_zero"] == len(pairs)
        and all(
            row["without"]["content"] == "UNKNOWN"
            and row["with"]["response_model_sha256"] == args.run_model_sha256
            and row["without"]["response_model_sha256"] == args.run_model_sha256
            and row["sole_carrier"]["prompt_difference_is_exact_residue_block"]
            and row["sole_carrier"]["oracle_occurrences_in_with_prompt"] == 1
            and row["sole_carrier"]["oracle_occurrences_outside_residue_in_with_prompt"] == 0
            and row["sole_carrier"]["oracle_occurrences_in_control_prompt"] == 0
            for row in pairs
        )
    )
    if not passed:
        raise AssertionError(f"discovery-only causal frontier failed: {aggregate}")
    return pairs, aggregate


def original_four_prompts(receipt: dict, fixture: accepted.Fixture) -> list[dict]:
    task_ids = {
        f"{fixture.slug}_scout_task",
        f"{fixture.slug}_builder_task",
        f"{fixture.slug}_adversary_task",
        f"{fixture.slug}_verifier_task",
    }
    prompts = []
    for row in receipt["trace"]:
        request = row.get("request", {})
        envelope = request.get("envelope", {})
        if request.get("op") == "cast" and envelope.get("task_id") in task_ids:
            prompts.append({
                "task_id": envelope["task_id"],
                "prompt": request["prompt"],
                "prompt_sha256": sha256_text(request["prompt"]),
            })
    return prompts


def finalize_receipt(
    args: argparse.Namespace,
    fixture: accepted.Fixture,
    receipt: dict,
) -> dict:
    pairs = receipt["matched_pair"]["pairs"]
    four_prompts = original_four_prompts(receipt, fixture)
    resolved_byte = fixture.expected_answer.split("|", 1)[0]
    verifier_fields = accepted.parse_pipe_record(
        receipt["discovery_provenance"]["verifier_result"]["content"], "DISCOVERY"
    )
    verifier_resolution = verifier_fields.get("answer", "") + "|" + verifier_fields.get("handling", "")
    if len(four_prompts) != 4:
        raise AssertionError(f"expected four original discovery prompts, found {len(four_prompts)}")
    if any(fixture.expected_answer in row["prompt"] or states_token(row["prompt"], resolved_byte)
           for row in four_prompts):
        raise AssertionError("resolved value was stated in an original discovery input")

    packet = receipt["matched_pair"]["with_injection_receipt"]["exact_packet"]
    receipt["kind"] = "dispatch_organ_impossible_frontier_proof"
    receipt["fixture"]["construction"] = (
        "Original four-worker run receives partitioned raw trials and computes a value absent from all "
        "four model inputs; hidden oracle validates only. Later cold task receives no derivation materials."
    )
    receipt["matched_pair"]["same_task_and_seed_per_pair"] = True
    receipt["matched_pair"]["only_difference_is_residue"] = all(
        row["sole_carrier"]["prompt_difference_is_exact_residue_block"] for row in pairs
    )
    receipt["matched_pair"]["quality_rubric"] = {
        "1.0": "exact discovery-only answer in the sole bounded call",
        "0.0": "not exact in the sole bounded call; no reread or reference route exists",
    }
    receipt["matched_pair"]["reference_archive_fallback"] = None
    receipt["matched_pair"]["manual_or_reread_route_exists"] = False
    receipt["matched_pair"]["system_prompt"] = accepted.SYSTEM
    receipt["discovery_only"] = {
        "oracle_role": "hidden_validator_only",
        "oracle": fixture.expected_answer,
        "resolved_byte_absent_from_original_four_inputs": True,
        "original_four_prompts": four_prompts,
        "four_worker_solvability": {
            "receipted": True,
            "verifier_result": receipt["discovery_provenance"]["verifier_result"],
            "parsed_resolution": verifier_resolution,
            "matches_hidden_oracle": verifier_resolution == fixture.expected_answer,
        },
        "cold_worker": {
            "materials": ["fixture_id", "output_shape"],
            "trials_supplied": False,
            "transform_rule_supplied": False,
            "key_supplied": False,
            "target_supplied": False,
            "archive_supplied": False,
            "tools_supplied": False,
            "max_completion_tokens": args.max_tokens,
            "attempts": len(pairs),
            "exact": sum(row["without"]["exact"] for row in pairs),
            "quality": statistics.mean(row["without"]["quality"] for row in pairs),
            "stays_zero": all(row["without"]["stays_zero"] for row in pairs),
        },
        "residue": {
            "bytes": len(packet.encode()),
            "sha256": sha256_text(packet),
            "verified_learning_id": receipt["discovery_provenance"]["candidate"]["learning_id"],
            "sole_carrier": all(
                row["sole_carrier"]["oracle_occurrences_in_residue"] == 1
                and row["sole_carrier"]["oracle_occurrences_in_control_prompt"] == 0
                and row["sole_carrier"]["oracle_occurrences_outside_residue_in_with_prompt"] == 0
                for row in pairs
            ),
        },
        "reference_archive_source_present": False,
        "reference_calls_total": receipt["matched_pair"]["aggregate"]["reference_calls_total"],
    }
    if not receipt["discovery_only"]["four_worker_solvability"]["matches_hidden_oracle"]:
        raise AssertionError("four-worker Verifier result does not prove task solvability")
    if receipt["discovery_only"]["residue"]["bytes"] != RESIDUE_BYTES:
        raise AssertionError("final residue is not exactly 435 bytes")
    if not receipt["discovery_only"]["residue"]["sole_carrier"]:
        raise AssertionError("sole-carrier audit failed")
    args.receipt.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
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
    failure.add_argument("--worker-launcher", type=pathlib.Path)
    failure.add_argument("--death-seat")
    parser.add_argument("--worker-pid-file", type=pathlib.Path)
    parser.add_argument("--fixture", choices=sorted(FIXTURES), required=True)
    parser.add_argument("--repetitions", type=int, default=6)
    parser.add_argument("--max-tokens", type=int, default=128)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--receipt", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.worker_launcher and not args.worker_pid_file:
        parser.error("--worker-launcher requires --worker-pid-file")
    if not 5 <= args.repetitions <= 12:
        raise SystemExit("repetitions must remain inside the sealed 5..12 cap")
    if not 64 <= args.max_tokens <= 256:
        raise SystemExit("cold-worker max_tokens must stay inside the generous bounded 64..256 range")
    accepted.candidate = frontier_candidate
    accepted.Organ.mutation = frontier_mutation
    accepted.run_pairs = run_discovery_only_pairs
    fixture = FIXTURES[args.fixture]
    receipt = accepted.live_scenario(args, fixture)
    receipt = finalize_receipt(args, fixture, receipt)
    print(json.dumps({
        "verdict": receipt["verdict"],
        "fixture": fixture.fixture_id,
        "residue_bytes": receipt["discovery_only"]["residue"]["bytes"],
        "with_quality": receipt["matched_pair"]["aggregate"]["with_quality_mean"],
        "cold_quality": receipt["matched_pair"]["aggregate"]["without_quality_mean"],
        "cold_stays_zero": receipt["discovery_only"]["cold_worker"]["stays_zero"],
        "receipt": str(args.receipt),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
