# Public/private boundary scanner continuation

## Code-only reachable-history production-header fix (2026-10-27)

The current and staged scanners already reject build/source references to
private production headers such as
`carto/production/sculpt_evaluation.hpp`. The reachable-history scanner first
uses `HISTORY_CONTENT_MARKER_PATTERN` to skip irrelevant lines, but that
prefilter did not include the private production header paths. A header
reference could therefore remain in an older commit after the current source
was cleaned up without reaching the existing private-reference detector.

The history prefilter now recognizes the protected production header family.
The later path-aware detector remains authoritative, so the scan still reports
only protected references in eligible source/build files.

- **Files changed:** `scripts/public_boundary_audit.py`,
  `tests/public_boundary_audit_tests.py`, `docs/public-private-boundary.md`,
  this ledger, and the ignored local work queue.
- **Capability gained:** reachable history can be screened for protected
  production API-header references after those references have been removed
  from the working tree.
- **Boundary improved:** authored regression source covers current, staged,
  and removed-then-committed references to
  `carto/production/sculpt_evaluation.hpp`.
- **Governance preserved:** the audit remains read-only; it does not rewrite
  history, contact a remote, or change ignore rules. No product authority or
  API/schema changed.
- **Tests authored:** focused scanner regression across current, staged, and
  selected reachable history.
- **Tests run:** none, per the user's instruction not to test or build.
- **Tests not run:** scanner suite, full public/private audit, ignore-policy
  probes, remote-ref review, and publication workflow.
- **Risks:** this adds a bounded path family to a line prefilter; other
  protected aliases not represented in the marker expression may still be
  missed. No publication result is claimed.
- **Rollback:** revert the marker expression, regression case, policy wording,
  this ledger, and the queue note. No Git object or remote state changes.
- **Rejected:** scanning every historical line with every regex regardless of
  the existing bounded prefilter, or reporting a clean current tree as proof
  that cleaned history contains no protected reference.
- **Cross-product trace:** direct impact is Cartographer's local publication
  audit. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA are unchanged;
  dataset/training/capability/promotion state outside this scanner is
  unchanged. No network or deployment work occurred.
- **Required follow-up:** run the focused scanner regression, then the full
  publication audit with the intended refs and ignore-policy check before any
  release decision.
