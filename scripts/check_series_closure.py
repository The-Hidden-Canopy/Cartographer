#!/usr/bin/env python3
"""Verify that no docseries working bundle remains inside the checkout.

This is an intake-closure check, not an implementation-completion claim.  It
only rejects path-level working bundles; ordinary documentation may still
describe the deliberately deferred capabilities and their evidence boundary.
The scan includes ignored files because an ignored temporary bundle is still
an open local work item.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


SERIES_PATH = re.compile(r"(?:docuseries|doc[-_]series)", re.IGNORECASE)
SKIPPED_DIRECTORIES = {".git", "build", "out", "node_modules", ".venv", "__pycache__"}


def git_paths(root: Path, *arguments: str) -> list[str]:
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        return []
    return [item.decode("utf-8", errors="replace") for item in result.stdout.split(b"\0") if item]


def filesystem_paths(root: Path) -> list[str]:
    findings: list[str] = []
    for current, directories, files in os.walk(root, followlinks=False):
        directories[:] = [name for name in directories if name not in SKIPPED_DIRECTORIES]
        current_path = Path(current)
        for name in [*directories, *files]:
            relative = (current_path / name).relative_to(root).as_posix()
            if SERIES_PATH.search(name):
                findings.append(relative)
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    arguments = parser.parse_args()
    root = arguments.root.resolve()
    if not (root / ".git").exists():
        print("DOCSERIES_CLOSURE_FAIL: root is not a Git checkout", file=sys.stderr)
        return 2

    findings = set(filesystem_paths(root))
    findings.update(git_paths(root, "ls-files"))
    findings.update(git_paths(root, "ls-files", "--others", "--exclude-standard"))
    findings = sorted(path for path in findings if SERIES_PATH.search(path))

    print(f"Cartographer docseries closure check: {root}")
    if findings:
        print("DOCSERIES_CLOSURE_FAIL")
        for path in findings:
            print(f"  - open path: {path}")
        return 1
    print("DOCSERIES_CLOSURE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
