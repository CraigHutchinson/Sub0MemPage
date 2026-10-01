#!/usr/bin/env python3
"""Drive unbuffered_probe: rotated, cache-evicted, contention-gated runs. See
docs/investigations/unbuffered-read-ceiling.md.

Eviction and the contention gate are Sub0Llm's (scripts/page_cache.py verifies the eviction and
raises rather than report a warm "cold" run; scripts/run_perf_suite.py owns the load gate), so this
driver takes the path to Sub0Llm's scripts/ directory instead of re-implementing either.

    python drive.py <probe.exe> <file> <sub0llm-scripts-dir> [--sweep depth|block|handles] [--rounds 2]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time

BLOCK_EXPERT = 1777664  # the largest real MoE expert (1,766,400 B) rounded up to 4 KiB

SWEEPS = {
    # (handles, mode, depth, block)
    "depth": [(h, m, d, BLOCK_EXPERT) for d in (1, 4, 8, 16) for m in ("buffered", "unbuffered") for h in ("shared",)],
    "handles": [(h, "unbuffered", d, BLOCK_EXPERT) for d in (8, 16) for h in ("shared", "perthread")],
    "block": [("perthread", "unbuffered", 16, b) for b in (256 << 10, 512 << 10, 1 << 20, BLOCK_EXPERT, 4 << 20)]
             + [("shared", "buffered", 16, BLOCK_EXPERT)],
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("probe")
    ap.add_argument("file")
    ap.add_argument("sub0llm_scripts")
    ap.add_argument("--sweep", choices=sorted(SWEEPS), default="block")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--bytes-per-run", type=int, default=900 << 20)
    args = ap.parse_args()
    sys.path.insert(0, args.sub0llm_scripts)
    import page_cache
    import run_perf_suite as gate

    deadline = time.monotonic() + 1800
    while problems := gate.contention_check(None)[1]:
        if time.monotonic() > deadline:
            sys.exit("REFUSING TO MEASURE: " + "; ".join(problems))
        print("waiting: " + "; ".join(problems), flush=True)
        time.sleep(60)

    variants = SWEEPS[args.sweep]
    for rnd in range(args.rounds):
        for handles, mode, depth, block in (variants if rnd % 2 == 0 else variants[::-1]):
            time.sleep(5)
            while gate.contention_count() > 0:  # a sibling build started mid-run: hold, never record
                print("paused: named competing process", flush=True)
                time.sleep(30)
            page_cache.evict_verified([args.file])
            reads = max(4, args.bytes_per_run // block // depth)
            out = subprocess.run([args.probe, args.file, mode, str(depth), str(reads), str(block),
                                  str(rnd * 100 + depth), handles], capture_output=True, text=True, check=True).stdout
            print(f"round {rnd} " + next(l for l in out.splitlines() if l.startswith("RESULT")), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
