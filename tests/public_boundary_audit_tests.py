#!/usr/bin/env python3
"""Adversarial tests for the read-only public boundary audit."""

from __future__ import annotations

import json
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


def git_value(repo: Path, *arguments: str) -> str:
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
    return result.stdout.strip()


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
        temporary = tempfile.TemporaryDirectory(dir=ROOT / "tmp")
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

    def test_protected_algorithm_history_path_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "polygon" / "src" / "polygon.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("protected fixture\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "protected algorithm fixture")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("libs/polygon/src/polygon.cpp", result.stdout)

    def test_protected_field_growth_content_fails_even_under_public_path(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "geometry" / "plant.cpp"
            path.parent.mkdir(parents=True)
            marker = "Plant" + "Growth" + "Product"
            path.write_text(f"struct {marker};\n", encoding="utf-8")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("libs/geometry/plant.cpp", result.stdout)
            self.assertIn("protected field-growth geometry preview", result.stdout)
            self.assertNotIn(marker, result.stdout)

    def test_protected_field_growth_history_is_detected_after_deletion(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "geometry" / "growth.cpp"
            path.parent.mkdir(parents=True)
            marker = "build_plant_" + "growth"
            path.write_text(f"void {marker}();\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "protected field-growth fixture")
            path.unlink()
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("history content", result.stdout)
            self.assertIn("protected field-growth geometry preview", result.stdout)
            self.assertNotIn(marker, result.stdout)

    def test_provider_neutral_vegetation_recipe_remains_public(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "condition" / "vegetation_recipe.hpp"
            path.parent.mkdir(parents=True)
            path.write_text(
                "struct VegetationScatterRecipe { unsigned seed; };\n",
                encoding="utf-8",
            )
            result = audit(repo)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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

    def test_workstation_path_fails_without_printing_value(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            leaked_path = "E:" + r"\HiddenCanopy\.cartographer-docuseries-archive-20261004"
            (repo / "notes.md").write_text(f"archive: `{leaked_path}`\n", encoding="utf-8")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("workstation path", result.stdout)
            self.assertNotIn(leaked_path, result.stdout)

    def test_deleted_workstation_path_is_caught_in_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            leaked_path = "C:" + r"\Users\envyn\private-cache"
            notes = repo / "notes.md"
            notes.write_text(f"cache: {leaked_path}\n", encoding="utf-8")
            git(repo, "add", "notes.md")
            git(repo, "commit", "-q", "-m", "workstation path fixture")
            notes.unlink()
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("workstation path", result.stdout)
            self.assertNotIn(leaked_path, result.stdout)

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

    def test_owner_reviewed_history_classification_closes_exact_root(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "apps" / "gmib" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            existing_commit = git_value(repo, "rev-parse", "HEAD")
            manifest = repo / "docs" / "verification" / "public-history-provenance.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({
                "schema": 1,
                "entries": [{
                    "root": "apps/gmib",
                    "commits": [existing_commit],
                    "disposition": "first_party_removed_from_current_public_tree",
                    "license_basis": "repository_apache_2_0",
                    "owner_reviewed": True,
                }],
            }), encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "classified integration candidate")
            result = audit(repo, "--fail-on-review")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("REVIEW:", result.stdout)

    def test_malformed_history_classification_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "apps" / "gmib" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            manifest = repo / "docs" / "verification" / "public-history-provenance.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text('{"schema": 1, "entries": "not-a-list"}\n', encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "malformed classification")
            result = audit(repo, "--fail-on-review")
            self.assertEqual(result.returncode, 1)
            self.assertIn("entries must be a list", result.stdout)

    def test_ignore_policy_passes_before_history_is_considered(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            (repo / ".gitignore").write_text(
                (ROOT / ".gitignore").read_text(encoding="utf-8"),
                encoding="utf-8",
            )
            git(repo, "add", ".gitignore")
            git(repo, "commit", "-q", "-m", "ignore policy")
            result = audit(repo, "--check-ignore-policy")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
