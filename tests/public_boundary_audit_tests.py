#!/usr/bin/env python3
"""Adversarial tests for the read-only public boundary audit."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "scripts" / "public_boundary_audit.py"


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


def audit(repo: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(AUDIT), "--root", str(repo), *arguments],
        cwd=ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


class PublicBoundaryAuditTests(unittest.TestCase):
    def make_repo(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory()
        repo = Path(temporary.name)
        git(repo, "init", "-q")
        git(repo, "config", "user.email", "boundary-test@example.invalid")
        git(repo, "config", "user.name", "Boundary Test")
        (repo / "README.md").write_text("public fixture\n", encoding="utf-8")
        git(repo, "add", "README.md")
        git(repo, "commit", "-q", "-m", "initial")
        return temporary

    def test_public_fixture_passes(self) -> None:
        with self.make_repo() as temporary:
            result = audit(Path(temporary))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_forbidden_history_path_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "docs" / "spec" / "source.pdf"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"private fixture")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "private artifact")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("docs/spec/source.pdf", result.stdout)

    def test_untracked_model_artifact_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            (repo / "weights.safetensors").write_bytes(b"private fixture")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("weights.safetensors", result.stdout)

    def test_tracked_ignored_path_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            (repo / ".gitignore").write_text("private.txt\n", encoding="utf-8")
            git(repo, "add", ".gitignore")
            git(repo, "commit", "-q", "-m", "ignore private fixture")
            (repo / "private.txt").write_text("private fixture\n", encoding="utf-8")
            git(repo, "add", "-f", "private.txt")
            git(repo, "commit", "-q", "-m", "tracked ignored fixture")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("tracked path private.txt: file is ignored by current policy", result.stdout)

    def test_dot_prefixed_quarantine_path_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / ".hub" / "adapter.json"
            path.parent.mkdir(parents=True)
            path.write_text("private fixture\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "private adapter")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn(".hub/adapter.json", result.stdout)

    def test_recognizable_secret_fails_without_printing_value(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            secret = "AKIA" + "ABCDEFGHIJKLMNOP"
            (repo / "notes.txt").write_text(f'credential = "{secret}"\n', encoding="utf-8")
            git(repo, "add", "notes.txt")
            git(repo, "commit", "-q", "-m", "credential fixture")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("recognizable credential pattern", result.stdout)
            self.assertNotIn(secret, result.stdout)

    def test_review_path_is_visible_and_optional(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "apps" / "gmib" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "integration candidate")
            result = audit(repo)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("REVIEW:", result.stdout)
            blocking = audit(repo, "--fail-on-review")
            self.assertEqual(blocking.returncode, 1)

    def test_checkout_ignore_policy_passes(self) -> None:
        result = audit(ROOT, "--check-ignore-policy")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
