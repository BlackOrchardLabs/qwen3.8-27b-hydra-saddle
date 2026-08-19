#!/usr/bin/env python3
"""Deterministic T1 content contract; emits the harness validator receipt shape."""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(json.dumps({"ok": False, "error": "usage: t1_shape_gate.py ARTIFACT"}))
        return 2
    path = pathlib.Path(sys.argv[1])
    try:
        data = path.read_bytes()
        lowered = data.decode("utf-8").lower()
    except (OSError, UnicodeDecodeError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 3
    checks = {
        "doctype": "<!doctype html" in lowered,
        "canvas": re.search(r"<canvas[^>]+id=[\"']field[\"']", lowered) is not None,
        "width": re.search(r"<canvas[^>]+width=[\"']?960", lowered) is not None,
        "height": re.search(r"<canvas[^>]+height=[\"']?540", lowered) is not None,
        "pause": re.search(r"id=[\"']pause[\"']", lowered) is not None,
        "reset": re.search(r"id=[\"']reset[\"']", lowered) is not None,
        "state": re.search(r"id=[\"']state[\"']", lowered) is not None,
        "seed": re.search(r"id=[\"']seed[\"']", lowered) is not None,
        "particle_count": re.search(r"(?:particle_count|particlecount|count)\s*=\s*120", lowered) is not None,
        "pointer": "pointer" in lowered or "mousemove" in lowered,
        "offline": "http://" not in lowered and "https://" not in lowered,
        "script": "<script" in lowered,
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
