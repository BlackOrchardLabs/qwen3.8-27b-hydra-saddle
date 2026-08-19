#!/usr/bin/env python3
"""Acceptance canary: a configured gate that can never pass."""

import hashlib
import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(json.dumps({"ok": False, "error": "usage: planted_fail_gate.py ARTIFACT"}))
        return 2
    path = pathlib.Path(sys.argv[1]).resolve()
    data = path.read_bytes()
    print(json.dumps({
        "ok": False,
        "planted_failure": True,
        "reason": "acceptance canary is intentionally impossible to satisfy",
        "artifact": path.name,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }, sort_keys=True))
    return 23


if __name__ == "__main__":
    raise SystemExit(main())

