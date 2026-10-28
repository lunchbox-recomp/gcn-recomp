#!/usr/bin/env python3
"""Run the steps CI runs, on a runner or on your own Mac.

Usage: python3 tools/ci.py [JOB ...]

Run it from the repository's root. With no JOB it runs every job. A job's steps run in
order, and the first one that fails stops the run with its exit code.
.github/workflows/ci.yml installs what a runner lacks and then runs each job by name, so a
step changes here and nowhere else.
"""
from __future__ import annotations

import subprocess
import sys

JOBS: dict[str, list[tuple[str, list[str]]]] = {
    "unit": [
        ("No game names in the tree", ["python3", "tools/check_generic.py"]),
        ("Configure without a game", ["cmake", "-S", ".", "-B", "build", "-G", "Ninja"]),
        ("Unit tests", ["ninja", "-C", "build", "unit"]),
    ],
}


def main(jobs: list[str]) -> int:
    unknown = [j for j in jobs if j not in JOBS]
    if unknown:
        print(f"ci.py: no job {', '.join(unknown)}; the jobs are {', '.join(JOBS)}", file=sys.stderr)
        return 2
    for job in jobs or list(JOBS):
        for name, cmd in JOBS[job]:
            print(f"== {job}: {name}", flush=True)
            code = subprocess.run(cmd).returncode
            if code:
                print(f"== {job}: {name} exited {code}", flush=True)
                return code
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
