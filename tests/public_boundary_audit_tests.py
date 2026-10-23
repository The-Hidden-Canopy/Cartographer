#!/usr/bin/env python3
"""Adversarial tests for the read-only public boundary audit."""

from __future__ import annotations

import json
import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from unittest.mock import patch
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "scripts" / "public_boundary_audit.py"
AUDIT_SPEC = importlib.util.spec_from_file_location("public_boundary_audit", AUDIT)
assert AUDIT_SPEC is not None and AUDIT_SPEC.loader is not None
AUDIT_MODULE = importlib.util.module_from_spec(AUDIT_SPEC)
sys.modules[AUDIT_SPEC.name] = AUDIT_MODULE
AUDIT_SPEC.loader.exec_module(AUDIT_MODULE)


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
    temporary_root = ROOT / "tmp"
    temporary_root.mkdir(exist_ok=True)
    environment = os.environ.copy()
    environment.update({
        "TMP": str(temporary_root),
        "TEMP": str(temporary_root),
        "TMPDIR": str(temporary_root),
    })
    return subprocess.run(
        [sys.executable, str(AUDIT), "--root", str(repo), *arguments],
        cwd=ROOT,
        env=environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def audit_with_fixture_root(
    repo: Path,
    fixture_root: str,
    root_id: str,
    *arguments: str,
) -> tuple[int, str]:
    fingerprint = hashlib.sha256(fixture_root.encode("utf-8")).hexdigest()
    previous_fingerprints = AUDIT_MODULE.REVIEW_ROOT_FINGERPRINTS
    previous_ids = AUDIT_MODULE.REVIEW_ROOT_IDS
    previous_argv = sys.argv
    AUDIT_MODULE.REVIEW_ROOT_FINGERPRINTS = {fingerprint: root_id}
    AUDIT_MODULE.REVIEW_ROOT_IDS = frozenset({root_id})
    output = StringIO()
    try:
        sys.argv = [str(AUDIT), "--root", str(repo), *arguments]
        with redirect_stdout(output):
            result = AUDIT_MODULE.main()
    finally:
        AUDIT_MODULE.REVIEW_ROOT_FINGERPRINTS = previous_fingerprints
        AUDIT_MODULE.REVIEW_ROOT_IDS = previous_ids
        sys.argv = previous_argv
    return result, output.getvalue()


class PublicBoundaryAuditTests(unittest.TestCase):
    def make_repo(self) -> tempfile.TemporaryDirectory[str]:
        temporary_root = ROOT / "tmp"
        temporary_root.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=temporary_root)
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

    def test_history_inventory_failure_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            original_run_git = AUDIT_MODULE.run_git

            def fail_history_inventory(root: Path, *arguments: str):
                if arguments[:2] == ("rev-list", "--objects"):
                    return subprocess.CompletedProcess(
                        ["git", *arguments], 128, "", "simulated inventory failure"
                    )
                return original_run_git(root, *arguments)

            previous_argv = sys.argv
            output = StringIO()
            errors = StringIO()
            sys.argv = [str(AUDIT), "--root", str(repo)]
            try:
                with patch.object(AUDIT_MODULE, "run_git", side_effect=fail_history_inventory):
                    with redirect_stdout(output), redirect_stderr(errors):
                        result = AUDIT_MODULE.main()
            finally:
                sys.argv = previous_argv

            self.assertEqual(result, 2)
            self.assertIn("history inventory unavailable", errors.getvalue())
            self.assertNotIn("PUBLIC_BOUNDARY_PASS", output.getvalue())

    def test_oversized_current_text_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "oversized.txt"
            path.write_bytes(b"x" * (AUDIT_MODULE.MAX_TEXT_SCAN_BYTES + 1))

            result = audit(repo)

            self.assertEqual(result.returncode, 1)
            self.assertIn(
                "current file oversized.txt: text exceeds scanner size limit",
                result.stdout,
            )

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

    def test_private_package_suffix_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "sample.oaw.carto"
            path.write_text("private package fixture", encoding="utf-8")
            git(repo, "add", "-f", str(path.relative_to(repo)))
            git(repo, "commit", "-q", "-m", "private package suffix fixture")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("private package artifact suffix", result.stdout)

    def test_legacy_private_package_suffix_still_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "sample.owh.carto"
            path.write_text("private package fixture", encoding="utf-8")
            git(repo, "add", "-f", str(path.relative_to(repo)))
            git(repo, "commit", "-q", "-m", "legacy private package suffix fixture")
            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("credential/model artifact suffix", result.stdout)

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

    def test_private_history_path_detected_when_blob_is_shared(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            shared = "same public bytes\n"
            (repo / "README.md").write_text(shared, encoding="utf-8")
            path = repo / "libs" / "botany" / "src" / "generation.cpp"
            path.parent.mkdir(parents=True)
            path.write_text(shared, encoding="utf-8")
            git(repo, "add", "-A")
            git(repo, "commit", "-q", "-m", "shared public/private blob")

            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove private path")

            result = audit(repo, "--ref", "HEAD")

            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("HISTORY_SCOPE selected refs: HEAD", result.stdout)
            self.assertIn("libs/botany/src/generation.cpp", result.stdout)
            self.assertIn("private/quarantined path", result.stdout)

    def test_private_botanical_generator_path_fails_untracked_and_staged(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "botany" / "src" / "generation.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void generate_preview();\n", encoding="utf-8")

            untracked = audit(repo)
            self.assertEqual(untracked.returncode, 1)
            self.assertIn("libs/botany/src/generation.cpp", untracked.stdout)
            self.assertIn("private/quarantined path", untracked.stdout)

            git(repo, "add", "-f", "libs/botany/src/generation.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1)
            self.assertIn(
                "staged path libs/botany/src/generation.cpp: private/quarantined path",
                staged.stdout,
            )

    def test_private_botany_test_paths_are_quarantined(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            for name in ("botany_tests.cpp", "native_botany_ui_tests.cpp"):
                path = repo / "tests" / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("int private_fixture;\n", encoding="utf-8")
                result = audit(repo)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"tests/{name}: private/quarantined path", result.stdout)
                path.unlink()

    def test_private_build_reference_fails_current_and_staged_scans(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            private_module = "bot" + "any"
            private_target = "carto_" + private_module
            path = repo / "CMakeLists.txt"
            path.write_text(
                f"add_subdirectory(libs/{private_module})\n"
                f"target_link_libraries(app PRIVATE {private_target})\n",
                encoding="utf-8",
            )

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file CMakeLists.txt: private botany build reference",
                current.stdout,
            )

            git(repo, "add", "CMakeLists.txt")
            path.write_text("add_executable(app main.cpp)\n", encoding="utf-8")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged file CMakeLists.txt: private botany build reference",
                staged.stdout,
            )

    def test_private_build_reference_in_selected_history_fails(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            private_module = "bot" + "any"
            path = repo / "CMakeLists.txt"
            path.write_text(
                f"add_subdirectory(libs/{private_module})\n",
                encoding="utf-8",
            )
            git(repo, "add", "CMakeLists.txt")
            git(repo, "commit", "-q", "-m", "private module build fixture")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove private module build fixture")

            result = audit(repo, "--ref", "HEAD")

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("history content", result.stdout)
        self.assertIn("private botany build reference", result.stdout)

    def test_private_production_kernel_paths_fail_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "production" / "src" / "cloth_solver.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void compile_private_kernel();\n", encoding="utf-8")

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "untracked path libs/production/src/cloth_solver.cpp: "
                "private/quarantined path",
                current.stdout,
            )

            git(repo, "add", "-f", "libs/production/src/cloth_solver.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged path libs/production/src/cloth_solver.cpp: "
                "private/quarantined path",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private production kernel fixture")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove private kernel fixture")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "selected-ref history path libs/production/src/cloth_solver.cpp: "
                "private/quarantined path",
                history.stdout,
            )

    def test_private_production_build_references_fail_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "production" / "CMakeLists.txt"
            path.parent.mkdir(parents=True)
            path.write_text(
                "add_library(carto_production src/cloth_solver.cpp)\n",
                encoding="utf-8",
            )

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file libs/production/CMakeLists.txt: "
                "private production implementation reference",
                current.stdout,
            )

            git(repo, "add", "libs/production/CMakeLists.txt")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged file libs/production/CMakeLists.txt: "
                "private production implementation reference",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private production build fixture")
            path.write_text(
                "add_library(carto_production src/garment_asset.cpp)\n",
                encoding="utf-8",
            )
            git(repo, "add", "libs/production/CMakeLists.txt")
            git(repo, "commit", "-q", "-m", "remove private build reference")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "private production implementation reference",
                history.stdout,
            )

    def test_private_out_of_tree_integration_cmake_reference_fails_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "CMakeLists.txt"
            private_option = "CARTO_BUILD_" + "PRIVATE_INTEGRATIONS"
            path.write_text(f"option({private_option} OFF)\n", encoding="utf-8")

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file CMakeLists.txt: private out-of-tree integration build reference",
                current.stdout,
            )

            git(repo, "add", "CMakeLists.txt")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged file CMakeLists.txt: private out-of-tree integration build reference",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private integration build fixture")
            path.write_text("project(public_only)\n", encoding="utf-8")
            git(repo, "add", "CMakeLists.txt")
            git(repo, "commit", "-q", "-m", "remove private integration option")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "history content", history.stdout
            )
            self.assertIn(
                "private out-of-tree integration build reference", history.stdout
            )

    def test_private_adaptive_representation_paths_fail_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "representation" / "src" / "selection.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void select_variant();\n", encoding="utf-8")

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "untracked path libs/representation/src/selection.cpp: "
                "private/quarantined path",
                current.stdout,
            )

            git(repo, "add", "-f", "libs/representation/src/selection.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged path libs/representation/src/selection.cpp: "
                "private/quarantined path",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private representation fixture")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove representation fixture")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "selected-ref history path libs/representation/src/selection.cpp: "
                "private/quarantined path",
                history.stdout,
            )

    def test_private_multires_path_and_api_fail_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "production" / "src" / "multires.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void compile_multires_surface_hierarchy();\n", encoding="utf-8")

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn("private/quarantined path", current.stdout)
            git(repo, "add", "-f", "libs/production/src/multires.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            git(repo, "commit", "-q", "-m", "private multires fixture")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove private multires fixture")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn("private/quarantined path", history.stdout)

    def test_private_d3d12_sculpt_preview_path_fails_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "d3d12" / "src" / "sculpt_preview.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void run_d3d12_sculpt_preview();\n", encoding="utf-8")

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn("private/quarantined path", current.stdout)
            git(repo, "add", "-f", "libs/d3d12/src/sculpt_preview.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            git(repo, "commit", "-q", "-m", "private D3D preview fixture")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove private D3D preview fixture")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn("private/quarantined path", history.stdout)

    def test_relocated_multires_api_fails_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "production" / "src" / "surface.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void compile_multires_surface_hierarchy();\n", encoding="utf-8")
            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn("private production API reference", current.stdout)
            git(repo, "add", "libs/production/src/surface.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            git(repo, "commit", "-q", "-m", "relocated private multires API")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove relocated private API")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn("private production API reference", history.stdout)

    def test_private_representation_build_reference_fails_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "biological" / "CMakeLists.txt"
            path.parent.mkdir(parents=True)
            path.write_text(
                "target_link_libraries(carto_biological PUBLIC carto_representation)\n",
                encoding="utf-8",
            )

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file libs/biological/CMakeLists.txt: "
                "private adaptive-representation build reference",
                current.stdout,
            )

            git(repo, "add", "libs/biological/CMakeLists.txt")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged file libs/biological/CMakeLists.txt: "
                "private adaptive-representation build reference",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private representation build fixture")
            path.write_text(
                "target_link_libraries(carto_biological PUBLIC carto_core)\n",
                encoding="utf-8",
            )
            git(repo, "add", "libs/biological/CMakeLists.txt")
            git(repo, "commit", "-q", "-m", "remove representation build reference")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "private adaptive-representation build reference",
                history.stdout,
            )

    def test_private_runtime_cloth_build_reference_fails_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "sim" / "CMakeLists.txt"
            path.parent.mkdir(parents=True)
            path.write_text(
                "add_library(carto_sim src/cloth.cpp)\n",
                encoding="utf-8",
            )

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file libs/sim/CMakeLists.txt: "
                "private runtime-cloth build reference",
                current.stdout,
            )

            git(repo, "add", "libs/sim/CMakeLists.txt")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged file libs/sim/CMakeLists.txt: "
                "private runtime-cloth build reference",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private runtime cloth fixture")
            path.write_text(
                "add_library(carto_sim src/collision_filter.cpp)\n",
                encoding="utf-8",
            )
            git(repo, "add", "libs/sim/CMakeLists.txt")
            git(repo, "commit", "-q", "-m", "remove runtime cloth build reference")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "private runtime-cloth build reference",
                history.stdout,
            )

    def test_relocated_private_kernel_api_fails_current_staged_and_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "production" / "src" / "solver.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("void compile_sculpt_stack();\n", encoding="utf-8")

            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file libs/production/src/solver.cpp: "
                "private production API reference",
                current.stdout,
            )

            git(repo, "add", "libs/production/src/solver.cpp")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn("private production API reference", staged.stdout)

            git(repo, "commit", "-q", "-m", "relocated private algorithm fixture")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove relocated algorithm fixture")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn(
                "private production API reference",
                history.stdout,
            )

    def test_public_production_authoring_contract_remains_allowed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            header = repo / "libs" / "production" / "include" / "garment_asset.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("struct GarmentAsset;\n", encoding="utf-8")
            cmake = repo / "libs" / "production" / "CMakeLists.txt"
            cmake.parent.mkdir(parents=True, exist_ok=True)
            cmake.write_text(
                "add_library(carto_production src/garment_asset.cpp)\n",
                encoding="utf-8",
            )

            result = audit(repo)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_private_short_namespace_is_blocked_but_public_contract_is_allowed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "ui" / "src" / "ui.cpp"
            path.parent.mkdir(parents=True)
            path.write_text(
                "namespace carto::ui {\n"
                "void submit(const botanical::Source&) {}\n"
                "}\n",
                encoding="utf-8",
            )
            public_result = audit(repo)
            self.assertEqual(
                public_result.returncode, 0,
                public_result.stdout + public_result.stderr,
            )

            path.write_text(
                'source.identity = "botanical.native.fixture";\n',
                encoding="utf-8",
            )
            private_identity = audit(repo)
            self.assertEqual(
                private_identity.returncode, 1,
                private_identity.stdout + private_identity.stderr,
            )
            self.assertIn(
                "current file libs/ui/src/ui.cpp: private botany build reference",
                private_identity.stdout,
            )

            path.write_text(
                'source.identity = "botanical.generated.fixture";\n',
                encoding="utf-8",
            )
            generated_receipt = audit(repo)
            self.assertEqual(
                generated_receipt.returncode, 0,
                generated_receipt.stdout + generated_receipt.stderr,
            )

            private_namespace = "bot" + "any"
            path.write_text(
                "namespace carto::ui {\n"
                f"void generate() {{ {private_namespace}::compile_representation(); }}\n"
                "}\n",
                encoding="utf-8",
            )
            current = audit(repo)
            self.assertEqual(current.returncode, 1, current.stdout + current.stderr)
            self.assertIn(
                "current file libs/ui/src/ui.cpp: private botany build reference",
                current.stdout,
            )

            git(repo, "add", "libs/ui/src/ui.cpp")
            path.write_text("namespace carto::ui { void submit() {} }\n", encoding="utf-8")
            staged = audit(repo)
            self.assertEqual(staged.returncode, 1, staged.stdout + staged.stderr)
            self.assertIn(
                "staged file libs/ui/src/ui.cpp: private botany build reference",
                staged.stdout,
            )

            git(repo, "commit", "-q", "-m", "private namespace fixture")
            path.write_text("namespace carto::ui { void submit() {} }\n", encoding="utf-8")
            git(repo, "add", "libs/ui/src/ui.cpp")
            git(repo, "commit", "-q", "-m", "remove private namespace fixture")
            history = audit(repo, "--ref", "HEAD")
            self.assertEqual(history.returncode, 1, history.stdout + history.stderr)
            self.assertIn("history content", history.stdout)
            self.assertIn("private botany build reference", history.stdout)

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

    def test_staged_private_content_fails_when_worktree_copy_is_safe(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "geometry" / "plant.cpp"
            path.parent.mkdir(parents=True)
            marker = "Plant" + "Growth" + "Product"
            path.write_text(f"struct {marker};\n", encoding="utf-8")
            git(repo, "add", "libs/geometry/plant.cpp")
            path.write_text("struct PublicRecipe {};\n", encoding="utf-8")

            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("staged file libs/geometry/plant.cpp", result.stdout)
            self.assertIn("protected field-growth geometry preview", result.stdout)
            self.assertNotIn(marker, result.stdout)

    def test_unmerged_index_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            base_branch = git_value(repo, "branch", "--show-current")
            path = repo / "README.md"

            git(repo, "checkout", "-q", "-b", "other")
            path.write_text("other public fixture\n", encoding="utf-8")
            git(repo, "add", "README.md")
            git(repo, "commit", "-q", "-m", "other change")

            git(repo, "checkout", "-q", base_branch)
            path.write_text("current public fixture\n", encoding="utf-8")
            git(repo, "add", "README.md")
            git(repo, "commit", "-q", "-m", "current change")
            merge = subprocess.run(
                ["git", "merge", "other"],
                cwd=repo,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertNotEqual(merge.returncode, 0)
            self.assertIn("README.md", git_value(repo, "ls-files", "--unmerged"))

            result = audit(repo)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("staged path README.md: unresolved merge stages", result.stdout)

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

            _, _, historical_objects = AUDIT_MODULE.history_paths(repo, False)
            inventory_calls: list[tuple[str, ...]] = []
            original_run_git_with_input = AUDIT_MODULE.run_git_with_input

            def record_inventory(root: Path, input_text: str, *arguments: str):
                inventory_calls.append(arguments)
                return original_run_git_with_input(root, input_text, *arguments)

            with patch.object(
                AUDIT_MODULE,
                "run_git_with_input",
                side_effect=record_inventory,
            ):
                findings = AUDIT_MODULE.history_content_findings(
                    repo, historical_objects
                )

            self.assertEqual(len(inventory_calls), 1)
            self.assertIn("--batch-check=%(objectname) %(objecttype) %(objectsize)", inventory_calls[0])
            self.assertIn("protected field-growth geometry preview", " ".join(findings))

            result = audit(repo)
            self.assertEqual(result.returncode, 1)
            self.assertIn("history content", result.stdout)
            self.assertIn("protected field-growth geometry preview", result.stdout)
            self.assertNotIn(marker, result.stdout)

    def test_selected_push_ref_excludes_private_local_stash(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "libs" / "geometry" / "local-growth.cpp"
            path.parent.mkdir(parents=True)
            marker = "Plant" + "Growth" + "Product"
            path.write_text(f"struct {marker};\n", encoding="utf-8")
            git(repo, "stash", "push", "--include-untracked", "-m", "local moat fixture")

            selected = audit(repo, "--ref", "HEAD")
            self.assertEqual(selected.returncode, 0, selected.stdout + selected.stderr)
            self.assertIn("HISTORY_SCOPE selected refs: HEAD", selected.stdout)

            local_inventory = audit(repo)
            self.assertEqual(local_inventory.returncode, 1)
            self.assertIn("includes local stashes", local_inventory.stdout)
            self.assertIn("protected field-growth geometry preview", local_inventory.stdout)
            self.assertNotIn(marker, local_inventory.stdout)

            reflog_inventory = audit(repo, "--include-reflog")
            self.assertEqual(reflog_inventory.returncode, 1)
            self.assertIn("protected field-growth geometry preview", reflog_inventory.stdout)

    def test_selected_remote_ref_still_scans_its_reachable_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "src" / "legacy-field.cpp"
            path.parent.mkdir(parents=True)
            marker = "build_plant_" + "growth"
            path.write_text(f"void {marker}();\n", encoding="utf-8")
            git(repo, "add", "src/legacy-field.cpp")
            git(repo, "commit", "-q", "-m", "private history fixture")
            git(repo, "update-ref", "refs/remotes/origin/main", "HEAD")
            path.unlink()
            git(repo, "add", "-u")
            git(repo, "commit", "-q", "-m", "remove private fixture from tip")
            git(repo, "update-ref", "refs/remotes/origin/main", "HEAD")

            result = audit(repo, "--ref", "origin/main")
            self.assertEqual(result.returncode, 1)
            self.assertIn("HISTORY_SCOPE selected refs: origin/main", result.stdout)
            self.assertIn("protected field-growth geometry preview", result.stdout)
            self.assertNotIn(marker, result.stdout)

    def test_invalid_or_missing_selected_ref_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            invalid = audit(repo, "--ref", "refs/bad..name")
            self.assertEqual(invalid.returncode, 2)
            self.assertIn("selected history ref is invalid", invalid.stderr)

            missing = audit(repo, "--ref", "refs/remotes/origin/missing")
            self.assertEqual(missing.returncode, 2)
            self.assertIn("selected history ref is unresolved", missing.stderr)

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

    def test_historical_review_root_requires_review(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "legacy" / "fixture-root" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "integration candidate")
            git(repo, "rm", "-q", "legacy/fixture-root/main.cpp")
            git(repo, "commit", "-q", "-m", "remove legacy fixture")
            result, output = audit_with_fixture_root(
                repo, "legacy/fixture-root", "fixture-root", "--fail-on-review"
            )
            self.assertEqual(result, 1, output)
            self.assertIn("REVIEW:", output)
            self.assertIn("legacy integration path requires provenance/licensing review", output)

    def test_owner_reviewed_history_classification_closes_exact_fingerprint(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "legacy" / "fixture-root" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "add legacy fixture")
            candidate_commit = git_value(repo, "rev-parse", "HEAD")
            git(repo, "rm", "-q", "legacy/fixture-root/main.cpp")
            git(repo, "commit", "-q", "-m", "remove legacy fixture")
            manifest = repo / "docs" / "verification" / "public-history-provenance.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({
                "schema": 2,
                "entries": [{
                    "root_id": "fixture-root",
                    "commits": [candidate_commit],
                    "disposition": "first_party_removed_from_current_public_tree",
                    "license_basis": "repository_apache_2_0",
                    "owner_reviewed": True,
                    "reviewed_on": "2026-10-17",
                }],
            }), encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "classify legacy fixture")
            result, output = audit_with_fixture_root(
                repo, "legacy/fixture-root", "fixture-root", "--fail-on-review"
            )
            self.assertEqual(result, 0, output)
            self.assertNotIn("REVIEW:", output)

    def test_history_classification_requires_commits_in_reachable_history(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "legacy" / "fixture-root" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "add legacy fixture")
            git(repo, "rm", "-q", "legacy/fixture-root/main.cpp")
            git(repo, "commit", "-q", "-m", "remove legacy fixture")
            manifest = repo / "docs" / "verification" / "public-history-provenance.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({
                "schema": 2,
                "entries": [{
                    "root_id": "fixture-root",
                    "commits": ["a" * 40],
                    "disposition": "first_party_removed_from_current_public_tree",
                    "license_basis": "repository_apache_2_0",
                    "owner_reviewed": True,
                    "reviewed_on": "2026-10-17",
                }],
            }), encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "classify with missing commit")
            result, output = audit_with_fixture_root(
                repo, "legacy/fixture-root", "fixture-root", "--fail-on-review"
            )
            self.assertEqual(result, 1, output)
            self.assertIn("incomplete classification", output)

    def test_malformed_history_classification_fails_closed(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "legacy" / "fixture-root" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "add legacy fixture")
            git(repo, "rm", "-q", "legacy/fixture-root/main.cpp")
            git(repo, "commit", "-q", "-m", "remove legacy fixture")
            manifest = repo / "docs" / "verification" / "public-history-provenance.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text('{"schema": 2, "entries": "not-a-list"}\n', encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "malformed classification")
            result, output = audit_with_fixture_root(
                repo, "legacy/fixture-root", "fixture-root", "--fail-on-review"
            )
            self.assertEqual(result, 1)
            self.assertIn("entries must be a list", output)

    def test_history_review_cannot_suppress_a_current_match(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            path = repo / "legacy" / "fixture-root" / "main.cpp"
            path.parent.mkdir(parents=True)
            path.write_text("public candidate\n", encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "add legacy fixture")
            candidate_commit = git_value(repo, "rev-parse", "HEAD")
            manifest = repo / "docs" / "verification" / "public-history-provenance.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({
                "schema": 2,
                "entries": [{
                    "root_id": "fixture-root",
                    "commits": [candidate_commit],
                    "disposition": "first_party_removed_from_current_public_tree",
                    "license_basis": "repository_apache_2_0",
                    "owner_reviewed": True,
                    "reviewed_on": "2026-10-17",
                }],
            }), encoding="utf-8")
            git(repo, "add", ".")
            git(repo, "commit", "-q", "-m", "classify history")
            result, output = audit_with_fixture_root(
                repo, "legacy/fixture-root", "fixture-root", "--fail-on-review"
            )
            self.assertEqual(result, 1, output)
            self.assertIn("tracked root", output)

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

    def test_ignore_policy_rejects_missing_local_state_guards(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            ignore_text = (ROOT / ".gitignore").read_text(encoding="utf-8")
            ignore_text = ignore_text.replace("imgui.ini\n", "")
            ignore_text = ignore_text.replace("/libs/botany/\n", "")
            ignore_text = ignore_text.replace("/tests/botany_tests.cpp\n", "")
            ignore_text = ignore_text.replace("/tests/native_botany_ui_tests.cpp\n", "")
            ignore_text = ignore_text.replace(
                "/docs/CARTOGRAPHER_WORK_QUEUE.local.md\n", ""
            )
            (repo / ".gitignore").write_text(ignore_text, encoding="utf-8")
            git(repo, "add", ".gitignore")
            git(repo, "commit", "-q", "-m", "ignore policy without local-state guards")
            result = audit(repo, "--check-ignore-policy")
        self.assertEqual(result.returncode, 1)
        self.assertIn("ignore policy does not quarantine imgui.ini", result.stdout)
        self.assertIn(
            "ignore policy does not quarantine libs/botany/src/generation.cpp",
            result.stdout,
        )
        self.assertIn(
            "ignore policy does not quarantine tests/botany_tests.cpp", result.stdout
        )
        self.assertIn(
            "ignore policy does not quarantine tests/native_botany_ui_tests.cpp",
            result.stdout,
        )
        self.assertIn(
            "ignore policy does not quarantine docs/CARTOGRAPHER_WORK_QUEUE.local.md",
            result.stdout,
        )

    def test_ignore_policy_rejects_stale_index_when_worktree_is_safe(self) -> None:
        with self.make_repo() as temporary:
            repo = Path(temporary)
            safe_text = (ROOT / ".gitignore").read_text(encoding="utf-8")
            stale_text = safe_text.replace("imgui.ini\n", "").replace(
                "/docs/CARTOGRAPHER_WORK_QUEUE.local.md\n", ""
            ).replace("/libs/botany/\n", "").replace(
                "/tests/botany_tests.cpp\n", ""
            ).replace("/tests/native_botany_ui_tests.cpp\n", "")
            ignore_path = repo / ".gitignore"
            ignore_path.write_text(stale_text, encoding="utf-8")
            git(repo, "add", ".gitignore")
            git(repo, "commit", "-q", "-m", "stale staged ignore policy")
            ignore_path.write_text(safe_text, encoding="utf-8")

            result = audit(repo, "--check-ignore-policy")

        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(
            "index ignore policy does not quarantine imgui.ini", result.stdout
        )
        self.assertIn(
            "index ignore policy does not quarantine libs/botany/src/generation.cpp",
            result.stdout,
        )
        self.assertIn(
            "index ignore policy does not quarantine tests/botany_tests.cpp",
            result.stdout,
        )
        self.assertIn(
            "index ignore policy does not quarantine tests/native_botany_ui_tests.cpp",
            result.stdout,
        )
        self.assertIn(
            "index ignore policy does not quarantine docs/CARTOGRAPHER_WORK_QUEUE.local.md",
            result.stdout,
        )
        self.assertNotIn(
            "worktree ignore policy does not quarantine imgui.ini", result.stdout
        )


if __name__ == "__main__":
    unittest.main()
