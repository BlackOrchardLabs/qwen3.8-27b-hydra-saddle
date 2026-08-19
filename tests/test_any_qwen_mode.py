#!/usr/bin/env python3
"""Deterministic tests for the run-on-any-Qwen demo model contract."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import hydra_demo  # noqa: E402
import integrated_proof as proof  # noqa: E402


class AnyQwenModeTests(unittest.TestCase):
    def test_arbitrary_model_file_is_fingerprinted_without_a_pin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            model = pathlib.Path(directory) / "own-qwen.gguf"
            model.write_bytes(b"an-own-qwen-fixture")
            self.assertEqual(
                proof.fingerprint_model(model),
                hashlib.sha256(b"an-own-qwen-fixture").hexdigest(),
            )

    def test_prepare_model_marks_reference_match_without_gating(self) -> None:
        args = argparse.Namespace(
            endpoint="http://localhost/v1/chat/completions",
            model=pathlib.Path("any-qwen.gguf"),
        )
        with mock.patch.object(proof, "fingerprint_model", return_value="b" * 64):
            model_sha, health = proof.prepare_model(args)
        self.assertEqual(model_sha, "b" * 64)
        self.assertFalse(args.reference_file_match)
        self.assertEqual(args.response_model_ids, [])
        self.assertEqual(health, "http://localhost/health")

        with mock.patch.object(proof, "fingerprint_model", return_value=proof.MODEL_SHA256):
            proof.prepare_model(args)
        self.assertTrue(args.reference_file_match)

    def test_response_identity_mismatch_never_blocks_chat(self) -> None:
        payload = {
            "model": "/served/models/another-qwen.gguf",
            "choices": [{"message": {"content": "OK"}, "finish_reason": "stop"}],
            "usage": {},
        }
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = json.dumps(payload).encode()
        with mock.patch.object(proof.urllib.request, "urlopen", return_value=response):
            result = proof.chat(
                "http://localhost/v1/chat/completions",
                pathlib.Path("supplied-qwen.gguf"), 1, "prompt", 10, 16,
                response_model_sha256="c" * 64,
            )
        self.assertEqual(result["response_model"], "another-qwen.gguf")
        self.assertEqual(result["response_model_sha256"], "c" * 64)

    def test_receipt_identity_keeps_file_fingerprint_and_endpoint_name_separate(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            receipt = pathlib.Path(directory) / "receipt.json"
            receipt.write_text(json.dumps({
                "model": {"endpoint_reported_ids": ["served-qwen.gguf"]},
            }))
            identity = hydra_demo.model_identity(
                [receipt], pathlib.Path("supplied-qwen.gguf"), "d" * 64,
            )
        self.assertEqual(identity["supplied_file"]["name"], "supplied-qwen.gguf")
        self.assertEqual(identity["supplied_file"]["fingerprint"]["value"], "d" * 64)
        self.assertEqual(identity["endpoint_reported_ids"], ["served-qwen.gguf"])
        self.assertFalse(identity["endpoint_identity_matches_supplied_file"])
        self.assertFalse(identity["identity_mismatch_blocks_run"])
        self.assertFalse(identity["results_comparable_to_reference"])

    def test_different_model_gets_one_plain_note(self) -> None:
        identity = {
            "results_comparable_to_reference": False,
            "endpoint_reported_ids": ["served-qwen.gguf"],
            "supplied_file": {"name": "supplied-qwen.gguf"},
        }
        self.assertEqual(
            hydra_demo.model_note(identity),
            "Results are from your model: served-qwen.gguf; reference comparison does not apply.",
        )
        identity["results_comparable_to_reference"] = True
        self.assertIsNone(hydra_demo.model_note(identity))

    def test_cli_has_no_opt_in_mode(self) -> None:
        argv = [
            "demo_frontier.py", "--endpoint", "http://localhost/v1/chat/completions",
            "--model", "any-qwen.gguf",
        ]
        with mock.patch.object(hydra_demo, "run_demo") as run_demo, mock.patch.object(
            sys, "argv", argv,
        ):
            self.assertEqual(hydra_demo.run_cli("frontier"), 0)
            self.assertFalse(hasattr(run_demo.call_args.args[1], "uncertified"))

    def test_help_says_any_qwen_and_has_no_uncertified_switch(self) -> None:
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", ["demo_frontier.py", "--help"]), mock.patch(
            "sys.stdout", stdout,
        ), self.assertRaises(SystemExit) as exit_status:
            hydra_demo.run_cli("frontier")
        self.assertEqual(exit_status.exception.code, 0)
        self.assertIn("Qwen model may run", stdout.getvalue())
        self.assertNotIn("--uncertified", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
