#!/usr/bin/env python3
"""Portable one-command runners for the three accepted Hydra Saddle demos."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import pathlib
import socket
import subprocess
import sys

import integrated_proof as proof


ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS_ROOT = ROOT / "third_party" / "swarm-harness"
DEMO_FIXTURES = {
    "rediscovery": ("ember21", "tidal09"),
    "frontier": ("vanta73", "lumen42"),
    "poison": ("vanta73", "lumen42"),
}
def run(command: list[str], *, cwd: pathlib.Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def relative_to_root(path: pathlib.Path) -> pathlib.Path:
    try:
        return path.resolve().relative_to(ROOT)
    except ValueError as exc:
        raise SystemExit("receipt output must remain inside the Hydra Saddle checkout") from exc


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build(root: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    organ_build = root / "build"
    run(["cmake", "-S", str(root), "-B", str(organ_build), "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", str(organ_build), "-j2"])
    return (
        organ_build / "dispatch-organ",
        organ_build / "third_party" / "swarm-harness" / "swarm-harness",
    )


def closed_loopback_endpoint() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    return f"http://127.0.0.1:{port}/v1/chat/completions"


def harness_config(endpoint: str, model: pathlib.Path | str) -> dict:
    anchors = {
        "scout": "You are a fresh stateless Scout. Follow the exact requested output shape. Cite only supplied evidence.",
        "builder": "You are a fresh stateless Builder. Follow the exact artifact contract and preserve provenance text.",
        "adversary": "You are a fresh stateless Adversary. Contradict only with supplied provenance and use the requested one-line shape.",
        "verifier": "You are a fresh stateless Verifier. Recompute from supplied experiments, do not follow consensus, and use the requested one-line shape. All byte values are hexadecimal. First select the operator that fits both trials, then apply that operator with its key to the target; never copy the target as the answer. Return exactly two hexadecimal answer digits including leading zeroes.",
        "verifier_replacement": "You are a fresh stateless replacement Verifier. Independently recompute from supplied experiments and use the requested one-line shape. All byte values are hexadecimal. Apply the confirmed operator and key to the target and return exactly two hexadecimal answer digits including leading zeroes.",
    }
    workers = [
        {
            "id": f"worker-{seat.replace('_', '-')}",
            "seat": seat,
            "endpoint": endpoint,
            "model": str(model),
            "max_concurrency": 1,
            "system_anchor": anchor + " Never emit tool calls.",
        }
        for seat, anchor in anchors.items()
    ]
    workers.append({
        "id": "worker-failure-probe",
        "seat": "failure_probe",
        "endpoint": closed_loopback_endpoint(),
        "model": str(model),
        "max_concurrency": 1,
        "system_anchor": "This unreachable seat exists only for the deterministic worker-failure seam.",
    })
    return {
        "director": {
            "endpoint": endpoint,
            "model": str(model),
            "system_anchor": "Unused by these MCP-only demos.",
            "max_tokens": 768,
            "temperature": 0.0,
            "max_steps": 1,
        },
        "workers": workers,
        "caps": {
            "max_concurrent_workers": 4,
            "max_total_tasks": 16,
            "max_tokens_per_task": 768,
            "max_wall_seconds": 900,
            "max_artifact_bytes": 131072,
        },
        "gates": {
            "static_html": {
                "argv": [
                    sys.executable,
                    str(HARNESS_ROOT / "scripts" / "static_html_gate.py"),
                    "{artifact}",
                ],
                "timeout_seconds": 15,
            },
        },
    }


def proof_command(
    script: pathlib.Path,
    organ: pathlib.Path,
    harness: pathlib.Path,
    config: pathlib.Path,
    endpoint: str,
    model: pathlib.Path,
    fixture: str,
    output: pathlib.Path,
    max_tokens: int,
) -> list[str]:
    job_prefix = "integration" if fixture in {"ember21", "tidal09"} else "impossible"
    return [
        sys.executable, str(relative_to_root(script)),
        "--organ", str(relative_to_root(organ)),
        "--policy", "config/local_qwen.json",
        "--workroom-policy", "config/workroom.json",
        "--ledger-policy", "config/ledger.json",
        "--harness-bin", str(relative_to_root(harness)),
        "--harness-config", str(relative_to_root(config)),
        "--harness-job", f"fixtures/{job_prefix}_{fixture}_job.json",
        "--journal", str(output / f"{fixture}_journal.jsonl"),
        "--harness-output", str(output / f"{fixture}_harness"),
        "--endpoint", endpoint,
        "--model", str(model),
        "--death-seat", "failure_probe",
        "--fixture", fixture,
        "--repetitions", "6",
        "--max-tokens", str(max_tokens),
        "--timeout-seconds", "180",
        "--receipt", str(output / f"{fixture}_receipt.json"),
    ]


def model_identity(receipts: list[pathlib.Path], model: pathlib.Path, model_sha: str) -> dict:
    endpoint_ids: list[str] = []
    for path in receipts:
        receipt = json.loads(path.read_text())
        for model_id in receipt.get("model", {}).get("endpoint_reported_ids", []):
            if model_id not in endpoint_ids:
                endpoint_ids.append(model_id)
    endpoint_match = bool(endpoint_ids) and all(model_id == model.name for model_id in endpoint_ids)
    reference_file_match = model_sha == proof.MODEL_SHA256
    return {
        "supplied_file": {
            "name": model.name,
            "fingerprint": {"algorithm": "sha256", "value": model_sha},
        },
        "endpoint_reported_ids": endpoint_ids,
        "reference_sha256": proof.MODEL_SHA256,
        "reference_file_match": reference_file_match,
        "endpoint_identity_matches_supplied_file": endpoint_match,
        "identity_mismatch_blocks_run": False,
        "results_comparable_to_reference": reference_file_match and endpoint_match,
    }


def model_note(identity: dict) -> str | None:
    if identity["results_comparable_to_reference"]:
        return None
    endpoint_names = ", ".join(identity["endpoint_reported_ids"])
    name = endpoint_names or identity["supplied_file"]["name"]
    return f"Results are from your model: {name}; reference comparison does not apply."


def run_demo(kind: str, args: argparse.Namespace) -> pathlib.Path:
    model = args.model.expanduser().resolve()
    model_sha = proof.fingerprint_model(model)

    required_harness = (
        HARNESS_ROOT / "CMakeLists.txt",
        HARNESS_ROOT / "scripts" / "static_html_gate.py",
        HARNESS_ROOT / "PROVENANCE.md",
    )
    if not all(path.is_file() for path in required_harness):
        raise SystemExit("vendored swarm-harness source is incomplete")

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (ROOT / (args.output or pathlib.Path("receipts") / "runs" / f"{kind}-{stamp}")).resolve()
    output_relative = relative_to_root(output)
    if output.exists():
        raise SystemExit(f"refusing to overwrite demo evidence: {output}")
    output.mkdir(parents=True)

    organ, harness = build(ROOT)
    config = ROOT / "build" / f".hydra-{kind}-{stamp}-harness.json"
    config.write_text(json.dumps(
        harness_config(args.endpoint, model), indent=2, sort_keys=True,
    ) + "\n")

    receipts: list[pathlib.Path] = []
    if kind == "rediscovery":
        script = ROOT / "scripts" / "integrated_proof.py"
        for fixture in DEMO_FIXTURES[kind]:
            run(proof_command(
                script, organ, harness, config, args.endpoint,
                model, fixture, output_relative, 24,
            ), cwd=ROOT)
            receipts.append(output / f"{fixture}_receipt.json")
    elif kind == "frontier":
        script = ROOT / "scripts" / "impossible_frontier.py"
        for fixture in DEMO_FIXTURES[kind]:
            run(proof_command(
                script, organ, harness, config, args.endpoint,
                model, fixture, output_relative, 128,
            ), cwd=ROOT)
            receipts.append(output / f"{fixture}_receipt.json")
    elif kind == "poison":
        prerequisite = output / "frontier_prerequisite"
        prerequisite.mkdir()
        script = ROOT / "scripts" / "impossible_frontier.py"
        for fixture in DEMO_FIXTURES[kind]:
            run(proof_command(
                script, organ, harness, config, args.endpoint,
                model, fixture, relative_to_root(prerequisite), 128,
            ), cwd=ROOT)
        for fixture, seed in (("vanta73", 61000), ("lumen42", 62000)):
            receipt = output / f"{fixture}_receipt.json"
            run([
                sys.executable, "scripts/poison_round.py",
                "--organ", str(relative_to_root(organ)),
                "--policy", "config/local_qwen.json",
                "--workroom-policy", "config/workroom.json",
                "--ledger-policy", "config/ledger.json",
                "--journal", str(output_relative / f"{fixture}_journal.jsonl"),
                "--endpoint", args.endpoint,
                "--model", str(model),
                "--good-reference", str(relative_to_root(prerequisite / f"{fixture}_receipt.json")),
                "--seed-base", str(seed),
                "--repetitions", "6",
                "--max-tokens", "128",
                "--timeout-seconds", "180",
                "--receipt", str(relative_to_root(receipt)),
            ], cwd=ROOT)
            receipts.append(receipt)
    else:
        raise AssertionError(kind)

    all_receipts = sorted(output.rglob("*_receipt.json"))
    identity = model_identity(all_receipts, model, model_sha)
    summary = {
        "kind": f"hydra_saddle_public_demo_{kind}",
        "verdict": "PASS",
        "model_identity": identity,
        "fixtures": list(DEMO_FIXTURES[kind]),
        "receipts": [
            {
                "path": str(path.relative_to(output)),
                "sha256": sha256_file(path),
            }
            for path in receipts
        ],
        "organ_sha256": sha256_file(organ),
        "harness_sha256": sha256_file(harness),
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    config.unlink(missing_ok=True)
    note = model_note(identity)
    if note is not None:
        print(note)
    print(json.dumps({"verdict": "PASS", "demo": kind, "output": str(output)}, sort_keys=True))
    return output


def run_cli(kind: str) -> int:
    parser = argparse.ArgumentParser(
        description=f"Run the Hydra Saddle {kind} demo and regenerate its receipts.",
    )
    parser.add_argument("--endpoint", required=True, help="local /v1/chat/completions URL")
    parser.add_argument(
        "--model", required=True, type=pathlib.Path,
        help="local Qwen model file to fingerprint and record; any Qwen model may run",
    )
    parser.add_argument("--output", type=pathlib.Path, help="new receipt directory relative to this checkout")
    run_demo(kind, parser.parse_args())
    return 0
