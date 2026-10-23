#!/usr/bin/env python3
"""Audit the Cartographer public/private publication boundary.

The audit is intentionally read-only.  It checks the index, non-ignored
working-tree files, reachable Git history, and optionally local reflogs.  It
does not rewrite history, expire reflogs, delete files, or contact a remote.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


FORBIDDEN_PREFIXES = (
    ".hub/",
    "hub-local/",
    ".moat/",
    "moat/",
    ".private-runtime/",
    "private-runtime/",
    "models-private/",
    "weights-private/",
    "prompts-private/",
    "telemetry-private/",
    "tuning-private/",
    "private/",
    ".private/",
    "docs/anatomy/",
    "docs/spec/",
    # Protected invention-bearing algorithm surfaces are out-of-tree even
    # when their names do not look like credentials or private integrations.
    "libs/botany/",
    "tests/botany_tests.cpp",
    "tests/native_botany_ui_tests.cpp",
    "libs/web_geometry/certified/",
    "libs/polygon/",
    "tests/certified_geometry_tests.cpp",
    "tests/polygon_tests.cpp",
    "benchmarks/certified_geometry.cpp",
    "benchmarks/polygon.cpp",
    "docs/architecture/consequence-certified-adaptive-geometry.md",
    "docs/verification/consequence-certified-adaptive-geometry.md",
    "docs/architecture/persistent-polygons.md",
    "docs/verification/persistent-polygons.md",
    # Public authoring schemas remain reviewable; implementation kernels and
    # algorithm-specific conformance suites are moat material.
    "libs/production/src/cloth_solver.cpp",
    "libs/production/src/sculpt_stack.cpp",
    "libs/production/src/local_remesh.cpp",
    "libs/production/include/carto/production/cloth_solver.hpp",
    "libs/production/include/carto/production/local_remesh.hpp",
    "tests/cloth_solver_tests.cpp",
    "tests/sculpt_stack_tests.cpp",
    "tests/local_remesh_tests.cpp",
    "libs/production/src/multires.cpp",
    "libs/production/include/carto/production/multires.hpp",
    "tests/multires_tests.cpp",
    "libs/d3d12/src/sculpt_preview.cpp",
    "libs/d3d12/include/carto/d3d12/sculpt_preview.hpp",
    "docs/architecture/representation-federation.md",
    "docs/verification/perceptual-optimization.md",
    # Adaptive representation/continuity policy and runtime cloth are private.
    "libs/representation/",
    "tests/representation_tests.cpp",
    "tests/representation_continuity_tests.cpp",
    "libs/sim/src/cloth.cpp",
    "libs/sim/include/carto/sim/cloth.hpp",
)

FORBIDDEN_SUFFIXES = (
    ".private.carto",
    ".owh.carto",
    ".pem",
    ".key",
    ".p12",
    ".pfx",
    ".crt",
    ".der",
    ".jks",
    ".keystore",
    ".secret",
    ".credentials",
    ".gguf",
    ".safetensors",
    ".onnx",
    ".pt",
    ".pth",
    ".ckpt",
)

REVIEW_ROOT_FINGERPRINTS = {
    "5ad075416334b9a86ceb3c16b1d939dee2334be29cb15d553ebaa565ecd687fa": "legacy-01",
    "511aee5461e1fc43211d240cc3344317b4438514b114119e7c6d234a0d537288": "legacy-02",
    "159d845d9e5934a66f90270de5db63eec82b34ab72a494eb8d8e973b5badaa2a": "legacy-03",
    "e589c73e9bab504c168aaab53da6eac80fb9ac33ea965494426ccd28ac5f488a": "legacy-04",
    "e9799722d674f3c359665212cfe3f89204523eb894b35842a987124edf4c1843": "legacy-05",
}
REVIEW_ROOT_IDS = frozenset(REVIEW_ROOT_FINGERPRINTS.values())

HISTORY_PROVENANCE_MANIFEST = "docs/verification/public-history-provenance.json"
HISTORY_PROVENANCE_SCHEMA = 2

REQUIRED_IGNORE_PROBES = (
    ".env",
    "local.env",
    "secret.pfx",
    "certificate.crt",
    "key.der",
    "private.credentials",
    "model.gguf",
    "model.safetensors",
    "model.onnx",
    "model.pt",
    "model.pth",
    "model.ckpt",
    ".hub/adapter.json",
    "models-private/model.bin",
    "private-runtime/snapshot.db",
    "docs/spec/source.pdf",
    "libs/polygon/src/polygon.cpp",
    "libs/botany/src/generation.cpp",
    "tests/botany_tests.cpp",
    "tests/native_botany_ui_tests.cpp",
    "libs/web_geometry/certified/src/certified.cpp",
    "sample.private.carto",
    "sample.oaw.carto",
    "sample.owh.carto",
    "sample.carto.journal",
    "sample.carto.lock",
    "sample.journal.lock",
    "imgui.ini",
    "docs/CARTOGRAPHER_WORK_QUEUE.local.md",
)

SECRET_PATTERNS = (
    re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----"),
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"\bghp_[A-Za-z0-9]{30,}\b"),
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}\b"),
    re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b"),
    re.compile(r"\bsk-[A-Za-z0-9]{20,}\b"),
    re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{20,}\b"),
    re.compile(
        r"\b(?:api[_-]?key|access[_-]?token|client_secret|password)"
        r"\s*[:=]\s*['\"][^'\"]{12,}['\"]",
        re.IGNORECASE,
    ),
)

# A public source tree must not preserve workstation-specific roots. Besides
# leaking local layout, these paths make release instructions non-portable and
# can disclose where private material was staged. The check is deliberately
# narrow: generic tool-install examples such as `C:\\Program Files` remain
# valid, while user/workspace roots and home directories fail closed.
WORKSTATION_PATH_PATTERNS = (
    re.compile(
        r"(?i)(?<![A-Za-z0-9])(?:[A-Z]:[\\/](?:Users[\\/][^\\/\s]+|HiddenCanopy)(?:[\\/]|$))"
    ),
    re.compile(r"(?<![A-Za-z0-9_])/home/[A-Za-z0-9._-]+(?:/|$)"),
)

GIT_SECRET_PATTERN = (
    r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----|"
    r"AKIA[0-9A-Z]{16}|"
    r"ghp_[A-Za-z0-9]{30,}|"
    r"github_pat_[A-Za-z0-9_]{20,}|"
    r"xox[baprs]-[A-Za-z0-9-]{20,}|"
    r"sk-[A-Za-z0-9]{20,}|"
    r"Bearer[[:space:]]+[A-Za-z0-9._~+/=-]{20,}|"
    r"(api[_-]?key|access[_-]?token|client_secret|password)[[:space:]]*[:=]"
    r"[[:space:]]*[\"'][^\"']{12,}[\"']"
)

GIT_WORKSTATION_PATH_PATTERN = (
    r"[A-Za-z]:[\\/]((Users)[\\/][^[:space:]]+|HiddenCanopy[\\/])|"
    r"/home/[A-Za-z0-9._-]+/"
)

# These names identify the protected procedural field-growth implementation,
# even when it is staged under an otherwise ordinary public geometry path.
# The audit reports only the path and category; it never prints matching code.
GIT_PROTECTED_CONTENT_PATTERN = (
    r"PlantGrowthRecipe|PlantSpeciesSource|PlantGrowthProduct|PlantBranch|"
    r"PlantRepresentationKind|build_plant_growth"
)
PROTECTED_CONTENT_PATTERN = re.compile(
    r"\b(?:PlantGrowthRecipe|PlantSpeciesSource|PlantGrowthProduct|PlantBranch|"
    r"PlantRepresentationKind|build_plant_growth)\b"
)
PROTECTED_PRODUCTION_CONTENT_PATTERN = re.compile(
    r"\b(?:compile_cloth_cpu|compile_sculpt_stack|stage_local_remesh_patch|"
    r"compile_multires_surface_hierarchy|MultiresSurfaceHierarchy|MultiresSettings|"
    r"run_d3d12_sculpt_preview|carto_d3d12_sculpt_preview)\b",
    re.IGNORECASE,
)
HISTORY_CONTENT_MARKER_PATTERN = re.compile(
    r"PRIVATE KEY|AKIA|ghp_|github_pat_|xox[baprs]-|sk-|bearer|"
    r"api[_-]?key|access[_-]?token|client_secret|password|"
    r"libs[/\\]botany|carto[/\\]botany|carto::botany|carto_botany|"
    r"botanical\.native\.|"
    r"\bbotany\s*::|\bnamespace\s+botany\b|"
    r"\bnamespace\s+[A-Za-z_]\w*\s*=\s*(?:carto::)?botany\b|"
    r"[A-Z]:[\\/](?:Users[\\/]|HiddenCanopy[\\/])|"
    r"/home/[A-Za-z0-9._-]+/|PlantGrowthRecipe|PlantSpeciesSource|"
    r"PlantGrowthProduct|PlantBranch|PlantRepresentationKind|build_plant_growth|"
    r"compile_cloth_cpu|compile_sculpt_stack|stage_local_remesh_patch|"
    r"compile_multires_surface_hierarchy|MultiresSurfaceHierarchy|MultiresSettings|"
    r"run_d3d12_sculpt_preview|carto_d3d12_sculpt_preview|carto_multires_tests|"
    r"carto[/\\]representation|carto::representation|carto_representation|"
    r"cartographer_representation(?:_continuity)?_tests|"
    r"carto[/\\]sim[/\\]cloth\.hpp|src[/\\]cloth\.cpp|"
    r"src[/\\](?:cloth_solver|sculpt_stack|local_remesh)\.cpp|"
    r"cartographer_(?:cloth_solver|sculpt_stack|local_remesh)_tests|"
    r"CARTO_BUILD_PRIVATE_INTEGRATIONS|CARTO_PRIVATE_SOURCE_ROOT|"
    r"CARTO_BUILD_GMIB_(?:GRAPHICS|GAME)|CARTO_GMIB_ROOT|"
    r"private_extensions\.cmake|"
    r"libs[/\\](?:anatomy|owh|gmib|simulation)[/\\]|"
    r"carto_(?:anatomy|owh|gmib)|"
    r"cartographer_(?:anatomy_catalog|owh_model)_tests",
    re.IGNORECASE,
)
HISTORY_CONTENT_PATTERN = (
    f"({GIT_SECRET_PATTERN})|({GIT_WORKSTATION_PATH_PATTERN})|"
    f"({GIT_PROTECTED_CONTENT_PATTERN})"
)
AUDIT_IMPLEMENTATION_PATH = "scripts/public_boundary_audit.py"
PRIVATE_BUILD_REFERENCE_PATTERN = re.compile(
    r"(?:libs[/\\]botany|carto[/\\]botany|carto::botany|carto_botany|"
    r"botanical\.native\.|"
    r"\bbotany\s*::|\bnamespace\s+botany\b|"
    r"\bnamespace\s+[A-Za-z_]\w*\s*=\s*(?:carto::)?botany\b)",
    re.IGNORECASE,
)
PRIVATE_PRODUCTION_BUILD_REFERENCE_PATTERN = re.compile(
    r"(?:src[/\\](?:cloth_solver|sculpt_stack|local_remesh|multires)\.cpp|"
    r"carto[/\\]production[/\\](?:cloth_solver|local_remesh|multires)\.hpp|"
    r"cartographer_(?:cloth_solver|sculpt_stack|local_remesh|multires)_tests|"
    r"carto_d3d12_sculpt_preview|run_d3d12_sculpt_preview|"
    r"d3d12[/\\]sculpt_preview\.hpp|d3d12[/\\]src[/\\]sculpt_preview\.cpp)",
    re.IGNORECASE,
)
PRIVATE_REPRESENTATION_BUILD_REFERENCE_PATTERN = re.compile(
    r"(?:carto[/\\]representation|carto::representation|carto_representation|"
    r"cartographer_representation(?:_continuity)?_tests)",
    re.IGNORECASE,
)
PRIVATE_SIM_BUILD_REFERENCE_PATTERN = re.compile(
    r"(?:src[/\\]cloth\.cpp|carto[/\\]sim[/\\]cloth\.hpp)",
    re.IGNORECASE,
)
PRIVATE_INTEGRATION_CMAKE_REFERENCE_PATTERN = re.compile(
    r"(?:\bCARTO_BUILD_PRIVATE_INTEGRATIONS\b|\bCARTO_PRIVATE_SOURCE_ROOT\b|"
    r"\bCARTO_BUILD_GMIB_(?:GRAPHICS|GAME)\b|\bCARTO_GMIB_ROOT\b|"
    r"private_extensions\.cmake|libs[/\\](?:anatomy|owh|gmib|simulation)[/\\]|"
    r"\bcarto_(?:anatomy|owh|gmib)\b|"
    r"\bcartographer_(?:anatomy_catalog|owh_model)_tests\b)",
    re.IGNORECASE,
)
PRIVATE_BUILD_FILE_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx",
    ".cmake", ".bzl", ".bat", ".cmd", ".ps1", ".sh", ".json", ".xml",
    ".yaml", ".yml", ".toml",
})
PRIVATE_BUILD_FILE_NAMES = frozenset({
    "cmakelists.txt", "makefile", "build", "build.bazel", "meson.build",
})

MAX_TEXT_SCAN_BYTES = 4 * 1024 * 1024


def run_git(root: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def run_git_with_input(
    root: Path, input_text: str, *arguments: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=False,
        input=input_text,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


class GitInventoryError(RuntimeError):
    """Raised when a required repository inventory could not be completed."""


def run_git_checked(
    root: Path, label: str, *arguments: str
) -> subprocess.CompletedProcess[str]:
    result = run_git(root, *arguments)
    if result.returncode != 0:
        raise GitInventoryError(label)
    return result


def normalize_path(path: str) -> str:
    normalized = path.replace("\\", "/")
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized.lower()


def path_reason(path: str) -> str | None:
    normalized = normalize_path(path)
    if any(
        normalized == prefix.rstrip("/") or normalized.startswith(prefix)
        for prefix in FORBIDDEN_PREFIXES
    ):
        return "private/quarantined path"
    if ".codex-remote-attachments/" in normalized:
        return "remote attachment path"
    if ".private." in normalized.rsplit("/", 1)[-1]:
        return "private filename marker"
    name = normalized.rsplit("/", 1)[-1]
    if name == ".env" or (name.startswith(".env.") and name != ".env.example"):
        return "environment/credential file"
    if name.endswith(".env") and name != ".env.example":
        return "environment/credential file"
    if any(normalized.endswith(suffix) for suffix in FORBIDDEN_SUFFIXES):
        return "credential/model artifact suffix"
    if re.search(r"\.o.w\.carto$", normalized):
        return "private package artifact suffix"
    return None


def review_root(
    path: str,
    fingerprints: dict[str, str] | None = None,
) -> str | None:
    normalized = normalize_path(path)
    known_fingerprints = fingerprints or REVIEW_ROOT_FINGERPRINTS
    components = normalized.split("/")
    for count in range(len(components), 0, -1):
        prefix = "/".join(components[:count])
        fingerprint = hashlib.sha256(prefix.encode("utf-8")).hexdigest()
        root_id = known_fingerprints.get(fingerprint)
        if root_id is not None:
            return root_id
    return None


def load_classified_history_roots(
    root: Path,
    candidates: set[str],
    findings: list[str],
    strict: bool,
    history_commits: set[str],
    allowed_roots: frozenset[str] | set[str] | None = None,
) -> set[str]:
    """Load explicit owner classifications for historical review roots.

    The manifest is deliberately narrow. It can classify only a known review
    root, and every entry must carry an owner-review marker, a disposition,
    and a repository-license basis. Missing or malformed metadata fails closed
    when the blocking release mode is requested.
    """
    if not candidates:
        return set()
    manifest_path = root / HISTORY_PROVENANCE_MANIFEST
    try:
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        if strict:
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: unavailable"
            )
        return set()
    except (OSError, json.JSONDecodeError) as error:
        findings.append(
            f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: unavailable or invalid ({error})"
        )
        return set()
    if not isinstance(payload, dict) or payload.get("schema") != HISTORY_PROVENANCE_SCHEMA:
        findings.append(
            f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: unsupported schema"
        )
        return set()
    entries = payload.get("entries")
    if not isinstance(entries, list):
        findings.append(
            f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: entries must be a list"
        )
        return set()

    classified: set[str] = set()
    known_roots = REVIEW_ROOT_IDS if allowed_roots is None else allowed_roots
    for entry in entries:
        if not isinstance(entry, dict):
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: entry is not an object"
            )
            continue
        root_id = entry.get("root_id")
        normalized_root_id = root_id if isinstance(root_id, str) else ""
        if normalized_root_id not in known_roots:
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: unknown review root id {normalized_root_id!r}"
            )
            continue
        if normalized_root_id not in candidates:
            continue
        commits = entry.get("commits")
        if (
            entry.get("owner_reviewed") is not True
            or entry.get("disposition") != "first_party_removed_from_current_public_tree"
            or entry.get("license_basis") != "repository_apache_2_0"
            or not isinstance(commits, list)
            or not commits
            or any(
                not isinstance(commit, str)
                or re.fullmatch(r"[0-9a-f]{40}", commit) is None
                or commit not in history_commits
                for commit in commits
            )
        ):
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: incomplete classification for {normalized_root_id}"
            )
            continue
        if normalized_root_id in classified:
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: duplicate classification for {normalized_root_id}"
            )
            continue
        classified.add(normalized_root_id)
    return classified


def nul_paths(result: subprocess.CompletedProcess[str]) -> list[str]:
    return [item for item in result.stdout.split("\0") if item]


def current_paths(root: Path) -> tuple[list[str], list[str]]:
    tracked = nul_paths(run_git_checked(
        root, "tracked path inventory unavailable", "ls-files", "-z"))
    untracked = nul_paths(
        run_git_checked(root, "untracked path inventory unavailable",
            "ls-files", "--others", "--exclude-standard", "-z")
    )
    return tracked, untracked


def staged_index_paths(root: Path) -> tuple[list[str], list[str]]:
    """Return staged paths and unresolved paths without reading worktree copies."""
    staged = nul_paths(
        run_git_checked(root, "staged path inventory unavailable",
            "diff", "--cached", "--name-only", "--diff-filter=ACMRT", "-z")
    )
    unmerged_result = run_git_checked(root, "unmerged path inventory unavailable",
        "ls-files", "--unmerged", "-z")
    unmerged = [
        entry.split("\t", 1)[-1]
        for entry in unmerged_result.stdout.split("\0")
        if entry
    ]
    return staged, unmerged


def read_index_text(root: Path, relative_path: str) -> tuple[str | None, str | None]:
    """Read a staged blob, returning a fail-closed reason when it cannot be scanned."""
    result = run_git(root, "show", f":{relative_path}")
    if result.returncode != 0:
        return None, "index blob unavailable"
    data = result.stdout.encode("utf-8", errors="replace")
    if b"\0" in data[:4096]:
        return None, None
    if len(data) > MAX_TEXT_SCAN_BYTES:
        return None, "text exceeds scanner size limit"
    return result.stdout, None


def tracked_ignored_paths(root: Path) -> list[str]:
    """Return tracked files hidden by the current ignore policy.

    `.gitignore` is not retroactive. A private file can remain tracked after a
    rule is added, so the publication audit must inspect the index separately
    from the ordinary non-ignored working-tree scan.
    """
    return nul_paths(run_git_checked(root, "tracked ignored path inventory unavailable",
        "ls-files", "--cached", "--ignored", "--exclude-standard", "-z"))


def deleted_worktree_paths(root: Path) -> set[str]:
    """Return tracked paths explicitly deleted in the working tree."""
    result = run_git_checked(root, "deleted path inventory unavailable",
        "diff", "--name-only", "--diff-filter=D", "-z")
    return {normalize_path(path) for path in nul_paths(result)}


def history_paths(
    root: Path,
    include_reflog: bool,
    selected_refs: list[str] | None = None,
) -> tuple[list[str], list[str], dict[str, str]]:
    arguments = ["rev-list", "--objects"]
    if selected_refs:
        arguments.extend(["--end-of-options", *selected_refs])
    else:
        arguments.append("--all")
        if include_reflog:
            arguments.append("--reflog")
    result = run_git_checked(root, "history inventory unavailable", *arguments)
    path_arguments = ["log", "--root", "--format=", "--name-only", "--no-renames", "-z"]
    if selected_refs:
        path_arguments.extend(["--end-of-options", *selected_refs])
    else:
        path_arguments.append("--all")
        if include_reflog:
            path_arguments.append("--reflog")
    path_result = run_git_checked(
        root, "history path inventory unavailable", *path_arguments
    )
    paths: set[str] = set()
    paths.update(nul_paths(path_result))
    commits: set[str] = set()
    object_paths: dict[str, str] = {}
    for line in result.stdout.splitlines():
        parts = line.split(" ", 1)
        if len(parts) != 2:
            commits.add(parts[0])
            object_paths.setdefault(parts[0], "")
            continue
        object_paths.setdefault(parts[0], parts[1])
        paths.add(parts[1])
    return sorted(paths), sorted(commits), object_paths


def history_content_findings(
    root: Path, object_paths: dict[str, str]
) -> list[str]:
    """Scan each unique historical text blob once for protected content."""
    findings: list[str] = []
    if not object_paths:
        return findings

    object_ids = sorted(object_paths)
    inventory = run_git_with_input(
        root,
        "\n".join(object_ids) + "\n",
        "cat-file",
        "--batch-check=%(objectname) %(objecttype) %(objectsize)",
    )
    if inventory.returncode != 0:
        return ["history content inventory unavailable"]

    blobs: list[tuple[str, str, int]] = []
    for line in inventory.stdout.splitlines():
        fields = line.split(" ")
        if len(fields) != 3 or not fields[2].isdigit():
            findings.append("history content inventory malformed")
            continue
        object_id, object_type, size_text = fields
        if object_type == "blob":
            path = object_paths.get(object_id)
            if path is None:
                findings.append(
                    f"history content {object_id[:12]}: path unavailable"
                )
                continue
            blobs.append((object_id, path, int(size_text)))

    def scan_text_blob(object_id: str, path: str, content: bytes) -> None:
        if not content or b"\0" in content[:4096]:
            return
        text = content.decode("utf-8", errors="replace")
        for line in text.splitlines():
            if not HISTORY_CONTENT_MARKER_PATTERN.search(line):
                continue
            if has_secret_pattern(line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "recognizable credential pattern"
                )
            if has_workstation_path(line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: workstation path"
                )
            if (
                normalize_path(path) != AUDIT_IMPLEMENTATION_PATH
                and PROTECTED_CONTENT_PATTERN.search(line)
            ):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "protected field-growth geometry preview"
                )
            if has_private_build_reference(path, line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "private botany build reference"
                )
            if has_private_production_build_reference(path, line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "private production implementation reference"
                )
            if has_private_representation_build_reference(path, line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "private adaptive-representation build reference"
                )
            if has_private_sim_build_reference(path, line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "private runtime-cloth build reference"
                )
            if has_private_integration_cmake_reference(path, line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "private out-of-tree integration build reference"
                )
            if path_reason(path) is None and has_protected_production_content(path, line):
                findings.append(
                    f"history content {object_id[:12]}:{path}: "
                    "private production API reference"
                )

    small_batches: list[list[tuple[str, str, int]]] = []
    pending: list[tuple[str, str, int]] = []
    pending_bytes = 0
    max_batch_count = 1024
    max_batch_bytes = 16 * 1024 * 1024
    large_blobs: list[tuple[str, str, int]] = []
    for blob in blobs:
        object_id, _, size = blob
        if size > MAX_TEXT_SCAN_BYTES:
            large_blobs.append(blob)
            continue
        if pending and (
            len(pending) >= max_batch_count
            or pending_bytes + size > max_batch_bytes
        ):
            small_batches.append(pending)
            pending = []
            pending_bytes = 0
        pending.append(blob)
        pending_bytes += size
    if pending:
        small_batches.append(pending)

    for batch in small_batches:
        request = "".join(f"{object_id}\n" for object_id, _, _ in batch)
        try:
            result = subprocess.run(
                ["git", "cat-file", "--batch"],
                cwd=root,
                check=False,
                input=request.encode("ascii"),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError:
            findings.append("history content scan unavailable")
            break
        if result.returncode != 0:
            findings.extend(
                f"history content scan {object_id[:12]}: unavailable"
                for object_id, _, _ in batch
            )
            continue

        output = io.BytesIO(result.stdout)
        for object_id, path, size in batch:
            header = output.readline().decode("ascii", errors="replace")
            fields = header.rstrip("\n").split(" ")
            if (
                len(fields) != 3
                or fields[0] != object_id
                or fields[1] != "blob"
                or not fields[2].isdigit()
                or int(fields[2]) != size
            ):
                findings.append(
                    f"history content scan {object_id[:12]}: malformed blob header"
                )
                break
            content = output.read(size)
            if len(content) != size or output.read(1) != b"\n":
                findings.append(
                    f"history content scan {object_id[:12]}: truncated blob"
                )
                break
            scan_text_blob(object_id, path, content)
        if output.read(1):
            findings.append("history content scan malformed batch output")

    for object_id, path, size in large_blobs:
        try:
            process = subprocess.Popen(
                ["git", "cat-file", "blob", object_id],
                cwd=root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError:
            findings.append(
                f"history content scan {object_id[:12]}: unavailable"
            )
            continue
        assert process.stdout is not None
        prefix = process.stdout.read(min(size, 4096))
        if len(prefix) != min(size, 4096):
            findings.append(
                f"history content scan {object_id[:12]}: truncated blob"
            )
        elif b"\0" not in prefix:
            findings.append(
                f"history content {object_id[:12]}:{path}: "
                "text exceeds scanner size limit"
            )
        remaining = size - len(prefix)
        while remaining:
            chunk = process.stdout.read(min(remaining, 1024 * 1024))
            if not chunk:
                findings.append(
                    f"history content scan {object_id[:12]}: truncated blob"
                )
                break
            remaining -= len(chunk)
        process.stdout.close()
        if process.stderr is not None:
            process.stderr.read()
            process.stderr.close()
        if process.wait() != 0:
            findings.append(
                f"history content scan {object_id[:12]}: unavailable"
            )
    return findings


def read_current_text(
    root: Path, relative_path: str
) -> tuple[str | None, str | None]:
    candidate = (root / Path(relative_path)).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        return None, None
    try:
        if not candidate.is_file():
            return None, None
        if candidate.stat().st_size > MAX_TEXT_SCAN_BYTES:
            return None, "text exceeds scanner size limit"
        data = candidate.read_bytes()
    except OSError:
        return None, "file could not be read"
    if b"\0" in data[:4096]:
        return None, None
    return data.decode("utf-8", errors="replace"), None


def has_secret_pattern(text: str) -> bool:
    return any(pattern.search(text) for pattern in SECRET_PATTERNS)


def has_workstation_path(text: str) -> bool:
    return any(pattern.search(text) for pattern in WORKSTATION_PATH_PATTERNS)


def has_private_build_reference(path: str, text: str) -> bool:
    """Reject build/source references that pull the private botany module in."""
    normalized = normalize_path(path)
    if normalized == AUDIT_IMPLEMENTATION_PATH:
        return False
    name = normalized.rsplit("/", 1)[-1]
    suffix = Path(name).suffix
    if name not in PRIVATE_BUILD_FILE_NAMES and suffix not in PRIVATE_BUILD_FILE_SUFFIXES:
        return False
    return PRIVATE_BUILD_REFERENCE_PATTERN.search(text) is not None


def has_private_production_build_reference(path: str, text: str) -> bool:
    """Reject build references to private production kernel implementations."""
    normalized = normalize_path(path)
    if normalized == AUDIT_IMPLEMENTATION_PATH:
        return False
    name = normalized.rsplit("/", 1)[-1]
    suffix = Path(name).suffix
    if name not in PRIVATE_BUILD_FILE_NAMES and suffix not in PRIVATE_BUILD_FILE_SUFFIXES:
        return False
    return PRIVATE_PRODUCTION_BUILD_REFERENCE_PATTERN.search(text) is not None


def has_private_representation_build_reference(path: str, text: str) -> bool:
    """Reject public dependency references to adaptive representation policy."""
    normalized = normalize_path(path)
    if normalized == AUDIT_IMPLEMENTATION_PATH:
        return False
    name = normalized.rsplit("/", 1)[-1]
    suffix = Path(name).suffix
    if name not in PRIVATE_BUILD_FILE_NAMES and suffix not in PRIVATE_BUILD_FILE_SUFFIXES:
        return False
    return PRIVATE_REPRESENTATION_BUILD_REFERENCE_PATTERN.search(text) is not None


def has_private_sim_build_reference(path: str, text: str) -> bool:
    """Reject build and source references to the private runtime cloth solver."""
    normalized = normalize_path(path)
    name = normalized.rsplit("/", 1)[-1]
    suffix = Path(name).suffix
    if name not in PRIVATE_BUILD_FILE_NAMES and suffix not in PRIVATE_BUILD_FILE_SUFFIXES:
        return False
    return PRIVATE_SIM_BUILD_REFERENCE_PATTERN.search(text) is not None


def has_private_integration_cmake_reference(path: str, text: str) -> bool:
    """Reject private out-of-tree sources or targets in public CMake inputs."""
    normalized = normalize_path(path)
    name = normalized.rsplit("/", 1)[-1]
    if name not in {"cmakelists.txt", "cmakepresets.json"} and Path(name).suffix != ".cmake":
        return False
    return PRIVATE_INTEGRATION_CMAKE_REFERENCE_PATTERN.search(text) is not None


def has_protected_production_content(path: str, text: str) -> bool:
    """Detect public declarations or calls to private production kernel APIs."""
    suffix = Path(normalize_path(path)).suffix
    if suffix not in {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}:
        return False
    return PROTECTED_PRODUCTION_CONTENT_PATTERN.search(text) is not None


def check_ignore_policy(root: Path, findings: list[str]) -> None:
    def inspect_policy(ignore_text: str, scope: str) -> None:
        # Evaluate Git's actual ignore semantics against the policy under review,
        # independent of the repository's worktree copy or local Git excludes.
        with tempfile.TemporaryDirectory(
            prefix="cartographer-ignore-audit-"
        ) as temporary:
            policy_root = Path(temporary)
            (policy_root / ".gitignore").write_bytes(ignore_text.encode("utf-8"))
            empty_config = policy_root / "empty.gitconfig"
            empty_config.write_bytes(b"")
            environment = os.environ.copy()
            environment["GIT_CONFIG_GLOBAL"] = str(empty_config)
            environment["GIT_CONFIG_NOSYSTEM"] = "1"
            for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
                environment.pop(name, None)
            initialized = subprocess.run(
                ["git", "init", "-q"], cwd=policy_root, env=environment,
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", errors="replace",
            )
            if initialized.returncode != 0:
                findings.append(f"{scope} ignore policy could not be evaluated")
                return

            probes = [*REQUIRED_IGNORE_PROBES, ".env.example"]
            result = subprocess.run(
                ["git", "check-ignore", "--no-index", "-z", "--stdin"],
                cwd=policy_root, env=environment, check=False,
                input="\0".join(probes) + "\0",
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, encoding="utf-8", errors="replace",
            )
            if result.returncode not in (0, 1):
                findings.append(f"{scope} ignore policy could not be evaluated")
                return
            ignored = set(result.stdout.split("\0"))
            for probe in REQUIRED_IGNORE_PROBES:
                if probe not in ignored:
                    findings.append(
                        f"{scope} ignore policy does not quarantine {probe}"
                    )
            if ".env.example" in ignored:
                findings.append(
                    f"{scope} ignore policy hides the public .env.example template"
                )

    index_text, index_error = read_index_text(root, ".gitignore")
    if index_error is not None or index_text is None:
        findings.append("index ignore policy is unavailable")
    else:
        inspect_policy(index_text, "index")

    worktree_text, worktree_error = read_current_text(root, ".gitignore")
    if worktree_error is not None or worktree_text is None:
        findings.append("worktree ignore policy is unavailable")
    else:
        inspect_policy(worktree_text, "worktree")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    history_scope = parser.add_mutually_exclusive_group()
    history_scope.add_argument(
        "--include-reflog",
        action="store_true",
        help="inspect all refs plus local reflog history, including stashes; read-only",
    )
    history_scope.add_argument(
        "--ref",
        dest="history_refs",
        action="append",
        help="restrict history scanning to this exact ref (repeat for a push target set, e.g. origin/main)",
    )
    parser.add_argument(
        "--check-ignore-policy",
        action="store_true",
        help="verify credential/model probes are ignored",
    )
    parser.add_argument(
        "--fail-on-review",
        action="store_true",
        help="treat legacy integration provenance review paths as blocking",
    )
    arguments = parser.parse_args()
    root = arguments.root.resolve()

    repository = run_git(root, "rev-parse", "--show-toplevel")
    if repository.returncode != 0:
        print("PUBLIC_BOUNDARY_FAIL: root is not a Git repository", file=sys.stderr)
        return 2

    selected_refs: list[str] | None = None
    if arguments.history_refs:
        selected_refs = []
        for reference in arguments.history_refs:
            if run_git(root, "check-ref-format", "--allow-onelevel",
                    reference).returncode != 0:
                print("PUBLIC_BOUNDARY_FAIL: selected history ref is invalid", file=sys.stderr)
                return 2
            resolved = run_git(root, "rev-parse", "--verify", "--quiet",
                "--end-of-options", f"{reference}^{{commit}}")
            if resolved.returncode != 0:
                print("PUBLIC_BOUNDARY_FAIL: selected history ref is unresolved", file=sys.stderr)
                return 2
            commit = resolved.stdout.strip()
            if commit not in selected_refs:
                selected_refs.append(commit)

    findings: list[str] = []
    review_candidates: list[tuple[str, str]] = []
    try:
        tracked, untracked = current_paths(root)
        staged, unmerged = staged_index_paths(root)
        tracked_ignored = tracked_ignored_paths(root)
        deleted_paths = deleted_worktree_paths(root)
        historical, commits, historical_objects = history_paths(
            root, arguments.include_reflog, selected_refs)
    except GitInventoryError as error:
        print(f"PUBLIC_BOUNDARY_FAIL: {error}", file=sys.stderr)
        return 2

    for path in unmerged:
        findings.append(f"staged path {path}: unresolved merge stages")

    for path in tracked_ignored:
        if normalize_path(path) in deleted_paths:
            continue
        findings.append(f"tracked path {path}: file is ignored by current policy")

    for source, paths in (("tracked", tracked), ("untracked", untracked)):
        for path in paths:
            if source == "tracked" and normalize_path(path) in deleted_paths:
                continue
            reason = path_reason(path)
            if reason is not None:
                findings.append(f"{source} path {path}: {reason}")
            review = review_root(path)
            if review is not None:
                review_candidates.append((f"{source} root", review))

    for path in historical:
        reason = path_reason(path)
        if reason is not None:
            if selected_refs:
                scope = "selected-ref history"
            elif arguments.include_reflog:
                scope = "reflog/history"
            else:
                scope = "reachable history (all refs; includes stashes)"
            findings.append(f"{scope} path {path}: {reason}")
        review = review_root(path)
        if review is not None:
            review_candidates.append(("history root", review))

    for path in sorted(set(tracked + untracked)):
        text, scan_error = read_current_text(root, path)
        if scan_error is not None:
            findings.append(f"current file {path}: {scan_error}")
            continue
        if text is not None and has_secret_pattern(text):
            findings.append(f"current file {path}: recognizable credential pattern")
        if text is not None and has_workstation_path(text):
            findings.append(f"current file {path}: workstation path")
        if (
            text is not None
            and normalize_path(path) != AUDIT_IMPLEMENTATION_PATH
            and PROTECTED_CONTENT_PATTERN.search(text)
        ):
            findings.append(
                f"current file {path}: protected field-growth geometry preview"
            )
        if text is not None and has_private_build_reference(path, text):
            findings.append(f"current file {path}: private botany build reference")
        if text is not None and has_private_production_build_reference(path, text):
            findings.append(
                f"current file {path}: private production implementation reference"
            )
        if text is not None and has_private_representation_build_reference(path, text):
            findings.append(
                f"current file {path}: private adaptive-representation build reference"
            )
        if text is not None and has_private_sim_build_reference(path, text):
            findings.append(
                f"current file {path}: private runtime-cloth build reference"
            )
        if text is not None and has_private_integration_cmake_reference(path, text):
            findings.append(
                f"current file {path}: private out-of-tree integration build reference"
            )
        if (
            text is not None
            and path_reason(path) is None
            and has_protected_production_content(path, text)
        ):
            findings.append(
                f"current file {path}: private production API reference"
            )

    for path in staged:
        reason = path_reason(path)
        if reason is not None:
            findings.append(f"staged path {path}: {reason}")
        text, scan_error = read_index_text(root, path)
        if scan_error is not None:
            findings.append(f"staged path {path}: {scan_error}")
            continue
        if text is None:
            continue
        if has_secret_pattern(text):
            findings.append(f"staged file {path}: recognizable credential pattern")
        if has_workstation_path(text):
            findings.append(f"staged file {path}: workstation path")
        if (
            normalize_path(path) != AUDIT_IMPLEMENTATION_PATH
            and PROTECTED_CONTENT_PATTERN.search(text)
        ):
            findings.append(
                f"staged file {path}: protected field-growth geometry preview"
            )
        if has_private_build_reference(path, text):
            findings.append(f"staged file {path}: private botany build reference")
        if has_private_production_build_reference(path, text):
            findings.append(
                f"staged file {path}: private production implementation reference"
            )
        if has_private_representation_build_reference(path, text):
            findings.append(
                f"staged file {path}: private adaptive-representation build reference"
            )
        if has_private_sim_build_reference(path, text):
            findings.append(
                f"staged file {path}: private runtime-cloth build reference"
            )
        if has_private_integration_cmake_reference(path, text):
            findings.append(
                f"staged file {path}: private out-of-tree integration build reference"
            )
        if (
            path_reason(path) is None
            and has_protected_production_content(path, text)
        ):
            findings.append(
                f"staged file {path}: private production API reference"
            )

    findings.extend(history_content_findings(root, historical_objects))

    if arguments.check_ignore_policy:
        check_ignore_policy(root, findings)

    candidate_roots = {
        review for scope, review in review_candidates if scope == "history root"
    }
    classified_roots = load_classified_history_roots(
        root, candidate_roots, findings, arguments.fail_on_review, set(commits)
    )
    reviews = sorted({
        f"{scope} {review}: legacy integration path requires provenance/licensing review"
        for scope, review in review_candidates
        if scope != "history root" or review not in classified_roots
    })
    findings = sorted(set(findings))
    print(f"Cartographer public boundary audit: {root}")
    if arguments.history_refs:
        print("HISTORY_SCOPE selected refs: " + ", ".join(arguments.history_refs))
    elif arguments.include_reflog:
        print("HISTORY_SCOPE all refs and reflogs (includes local stashes)")
    else:
        print("HISTORY_SCOPE all refs (includes local stashes; not just pushable refs)")
    if reviews:
        print("REVIEW: legacy integration paths require explicit provenance/licensing classification")
        for review in reviews:
            print(f"  - {review}")
    if arguments.fail_on_review and reviews:
        findings.extend(f"blocking review: {review}" for review in reviews)
    if findings:
        print("PUBLIC_BOUNDARY_FAIL")
        for finding in sorted(set(findings)):
            print(f"  - {finding}")
        return 1
    print("PUBLIC_BOUNDARY_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
