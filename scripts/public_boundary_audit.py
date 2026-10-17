#!/usr/bin/env python3
"""Audit the Cartographer public/private publication boundary.

The audit is intentionally read-only.  It checks the index, non-ignored
working-tree files, reachable Git history, and optionally local reflogs.  It
does not rewrite history, expire reflogs, delete files, or contact a remote.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
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
)

FORBIDDEN_SUFFIXES = (
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

REVIEW_PREFIXES = (
    "apps/gmib",
    "libs/anatomy",
    "libs/owh",
    "libs/simulation",
    "docs/architecture/gmib-integration.md",
)

HISTORY_PROVENANCE_MANIFEST = "docs/verification/public-history-provenance.json"
HISTORY_PROVENANCE_SCHEMA = 1

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
    "libs/web_geometry/certified/src/certified.cpp",
    "sample.owh.carto",
    "sample.carto.journal",
    "sample.carto.lock",
    "sample.journal.lock",
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
AUDIT_IMPLEMENTATION_PATH = "scripts/public_boundary_audit.py"

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
    return None


def review_root(path: str) -> str | None:
    normalized = normalize_path(path)
    for prefix in REVIEW_PREFIXES:
        if normalized == prefix or normalized.startswith(prefix + "/"):
            return prefix
    return None


def load_classified_history_roots(
    root: Path,
    candidates: set[str],
    findings: list[str],
    strict: bool,
    history_commits: set[str],
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
    for entry in entries:
        if not isinstance(entry, dict):
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: entry is not an object"
            )
            continue
        candidate_root = entry.get("root")
        normalized_root = normalize_path(candidate_root) if isinstance(candidate_root, str) else ""
        if normalized_root not in REVIEW_PREFIXES:
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: unknown review root {candidate_root!r}"
            )
            continue
        if normalized_root not in candidates:
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
                for commit in commits
            )
            or any(commit not in history_commits for commit in commits)
        ):
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: incomplete classification for {normalized_root}"
            )
            continue
        if normalized_root in classified:
            findings.append(
                f"history provenance manifest {HISTORY_PROVENANCE_MANIFEST}: duplicate classification for {normalized_root}"
            )
            continue
        classified.add(normalized_root)
    return classified


def nul_paths(result: subprocess.CompletedProcess[str]) -> list[str]:
    return [item for item in result.stdout.split("\0") if item]


def current_paths(root: Path) -> tuple[list[str], list[str]]:
    tracked = nul_paths(run_git(root, "ls-files", "-z"))
    untracked = nul_paths(
        run_git(root, "ls-files", "--others", "--exclude-standard", "-z")
    )
    return tracked, untracked


def tracked_ignored_paths(root: Path) -> list[str]:
    """Return tracked files hidden by the current ignore policy.

    `.gitignore` is not retroactive. A private file can remain tracked after a
    rule is added, so the publication audit must inspect the index separately
    from the ordinary non-ignored working-tree scan.
    """
    return nul_paths(
        run_git(root, "ls-files", "--cached", "--ignored", "--exclude-standard", "-z")
    )


def deleted_worktree_paths(root: Path) -> set[str]:
    """Return tracked paths explicitly deleted in the working tree."""
    result = run_git(root, "diff", "--name-only", "--diff-filter=D", "-z")
    return {normalize_path(path) for path in nul_paths(result)}


def history_paths(root: Path, include_reflog: bool) -> tuple[list[str], list[str]]:
    arguments = ["rev-list", "--objects", "--all"]
    if include_reflog:
        arguments.append("--reflog")
    result = run_git(root, *arguments)
    paths: set[str] = set()
    commits: set[str] = set()
    for line in result.stdout.splitlines():
        parts = line.split(" ", 1)
        if len(parts) != 2:
            commits.add(parts[0])
            continue
        paths.add(parts[1])
    return sorted(paths), sorted(commits)


def read_current_text(root: Path, relative_path: str) -> str | None:
    candidate = (root / Path(relative_path)).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        return None
    try:
        if not candidate.is_file() or candidate.stat().st_size > MAX_TEXT_SCAN_BYTES:
            return None
        data = candidate.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:4096]:
        return None
    return data.decode("utf-8", errors="replace")


def has_secret_pattern(text: str) -> bool:
    return any(pattern.search(text) for pattern in SECRET_PATTERNS)


def has_workstation_path(text: str) -> bool:
    return any(pattern.search(text) for pattern in WORKSTATION_PATH_PATTERNS)


def check_ignore_policy(root: Path, findings: list[str]) -> None:
    for probe in REQUIRED_IGNORE_PROBES:
        result = run_git(root, "check-ignore", "--no-index", "-q", "--", probe)
        if result.returncode != 0:
            findings.append(f"ignore policy does not quarantine {probe}")
    example = run_git(root, "check-ignore", "--no-index", "-q", "--", ".env.example")
    if example.returncode == 0:
        findings.append("ignore policy hides the public .env.example template")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--include-reflog",
        action="store_true",
        help="also inspect local reflog/unreachable history; read-only",
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

    findings: list[str] = []
    review_candidates: list[tuple[str, str]] = []
    tracked, untracked = current_paths(root)
    tracked_ignored = tracked_ignored_paths(root)
    deleted_paths = deleted_worktree_paths(root)
    historical, commits = history_paths(root, arguments.include_reflog)

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
            scope = "reflog/history" if arguments.include_reflog else "reachable history"
            findings.append(f"{scope} path {path}: {reason}")
        review = review_root(path)
        if review is not None:
            review_candidates.append(("history root", review))

    for path in sorted(set(tracked + untracked)):
        text = read_current_text(root, path)
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

    for commit in commits:
        result = run_git(root, "grep", "-I", "-l", "-E", GIT_SECRET_PATTERN, commit, "--")
        if result.returncode == 0:
            for path in result.stdout.splitlines():
                findings.append(f"history content {commit[:12]}:{path}: recognizable credential pattern")
        result = run_git(root, "grep", "-I", "-l", "-E", GIT_WORKSTATION_PATH_PATTERN, commit, "--")
        if result.returncode == 0:
            for path in result.stdout.splitlines():
                findings.append(f"history content {commit[:12]}:{path}: workstation path")
        result = run_git(
            root, "grep", "-I", "-l", "-E", GIT_PROTECTED_CONTENT_PATTERN,
            commit, "--",
        )
        if result.returncode == 0:
            for item in result.stdout.splitlines():
                _, path = item.split(":", 1)
                if normalize_path(path) == AUDIT_IMPLEMENTATION_PATH:
                    continue
                findings.append(
                    f"history content {commit[:12]}:{path}: "
                    "protected field-growth geometry preview"
                )

    if arguments.check_ignore_policy:
        check_ignore_policy(root, findings)

    candidate_roots = {review for _, review in review_candidates}
    classified_roots = load_classified_history_roots(
        root, candidate_roots, findings, arguments.fail_on_review, set(commits)
    )
    reviews = sorted({
        f"{scope} {review}: legacy integration path requires provenance/licensing review"
        for scope, review in review_candidates
        if review not in classified_roots
    })
    findings = sorted(set(findings))
    print(f"Cartographer public boundary audit: {root}")
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
