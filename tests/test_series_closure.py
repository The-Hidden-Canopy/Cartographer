#!/usr/bin/env python3
"""Adversarial tests for the local docseries intake boundary."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "scripts" / "check_series_closure.py"


def git(repo: Path, *arguments: str) -> None:
    result = subprocess.run(
        ["git", *arguments],
        cwd=repo,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise AssertionError(f"git {' '.join(arguments)} failed: {result.stderr}")


def check(repo: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECK), "--root", str(repo)],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


class SeriesClosureTests(unittest.TestCase):
    def make_repo(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        repo = Path(temporary.name)
        git(repo, "init", "-q")
        git(repo, "config", "user.email", "series-test@example.invalid")
        git(repo, "config", "user.name", "Series Test")
        (repo / "README.md").write_text("public fixture\n", encoding="utf-8")
        git(repo, "add", "README.md")
        git(repo, "commit", "-q", "-m", "initial")
        return temporary

    def test_clean_checkout_passes(self) -> None:
        with self.make_repo() as temporary:
            result = check(Path(temporary))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("DOCSERIES_CLOSURE_PASS", result.stdout)

    def test_ignored_working_bundle_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            (repo / ".gitignore").write_text("tmp/\n", encoding="utf-8")
            git(repo, "add", ".gitignore")
            git(repo, "commit", "-q", "-m", "ignore temporary work")
            bundle = repo / "tmp" / "plugin-docuseries"
            bundle.mkdir(parents=True)
            (bundle / "00_INDEX.md").write_text("working input\n", encoding="utf-8")
            result = check(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("plugin-docuseries", result.stdout)

    def test_tracked_bundle_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            bundle = repo / "docs" / "doc_series"
            bundle.mkdir(parents=True)
            (bundle / "index.md").write_text("tracked input\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "tracked series")
            result = check(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("docs/doc_series", result.stdout)

    def test_prose_reference_without_bundle_passes(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            (repo / "evidence.md").write_text(
                "The former docuseries is now an archived input.\n",
                encoding="utf-8",
            )
            result = check(repo)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
