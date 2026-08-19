#!/usr/bin/env python3
"""Cheap fail-closed HTML structure gate used by deterministic tests."""

import hashlib
import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(json.dumps({"ok": False, "error": "usage: static_html_gate.py ARTIFACT"}))
        return 2
    path = pathlib.Path(sys.argv[1]).resolve()
    try:
        data = path.read_bytes()
    except OSError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 3
    lowered = data.lower()
    checks = {
        "doctype": b"<!doctype html" in lowered,
        "html": b"<html" in lowered and b"</html>" in lowered,
        "script": b"<script" in lowered and b"</script>" in lowered,
        "canvas": b"<canvas" in lowered,
        "minimum_bytes": len(data) >= 256,
    }
    result = {
        "ok": all(checks.values()),
        "checks": checks,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    print(json.dumps(result, sort_keys=True))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

