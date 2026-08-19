#!/usr/bin/env python3
import sys

sys.dont_write_bytecode = True

from hydra_demo import run_cli

raise SystemExit(run_cli("poison"))
