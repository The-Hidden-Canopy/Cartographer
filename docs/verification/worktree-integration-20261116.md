# Cartographer worktree consolidation — 2026-11-16

This is a local integration and cleanup ledger. It is not a release,
deployment, external-runtime acceptance, or permission to publish historical
private objects. Validation and final cleanup results are recorded below.

## Candidate and preservation

- Starting primary branch: `integration/staging-20261112`, `173a3c0`.
- Starting `main`: `76fc917`; rollback ref:
  `backup/main-before-worktree-integration-20261116`.
- Texturing workflow: `a854432`, based on the complete staging tip.
- Merge `df0cd18` reconciles main with staging and texturing. It preserves
  main's bounded texel addressing, magnification and HDR fixes, the staged
  source/import regression suites and bounded editing commands, and the
  texturing commands, provenance and export paths.
- Merge `84e4f78` adds the six committed fixes through `96dbc11` from the
  separately registered render copy. Both anisotropic and masked-mip tests
  are retained. Preview status uses the tested fixed-height helper and checks
  frame generation/revision before reporting readiness. The render copy's
  current window was not operated on or closed.
- Ten previously untracked first-use documents are recovered with explicit
  historical notices. They remain dated proposals and observations, not
  current UI or alpha-acceptance claims.

Before cleanup, every registered tree was inventoried. Exact tracked WIP
patches, staged patches and untracked source contents were archived under
`build/worktree-integration-20261116/` in the primary checkout. `inventory.json`
records original HEADs, dirty paths, content hashes, comparison hints and
ignored-file summaries. Each tree has a `*-work.zip`; ignored project/assets,
journals, receipts, images and logs have separate `*-evidence.zip` archives
and content manifests. Compiled binaries, object files under `CMakeFiles`,
libraries and generated build-system state are rebuildable cleanup outputs.
They are explicitly listed as excluded; source OBJ assets are preserved.

These local archives deliberately remain ignored. They can contain obsolete
or uncleared historical source and must not be added to a public commit.
Original branch refs are retained, and detached HEADs receive archive refs.
Git ancestry alone is insufficient for older redacted histories: restoring
those histories would undo the publication boundary. Their accepted source
was previously reconciled into the public baseline and later integrations.

## Worktree disposition

The following older copies are superseded by the candidate's current
implementations. Original draft differences and historical handoffs remain in
the archives; superseded drafts are not represented as newly shipped features.

| Copy | Integration evidence / retained contract |
| --- | --- |
| `carto-sprint3-decoder` | Current JPEG entropy decoder; recovered coverage and WIC sprint suites in `2b623c7` / `d773a28`; old decoder commentary retained in archive. |
| `carto-sprint3-registry` | Bounded source registry and recovered budget/lifecycle suites; later product identity and source-budget checks retained. |
| `carto-sprint3-journal` | Superseded by persistent `.uncertain` markers and real I/O fault fixtures in `journal_io_uncertainty_tests.cpp`, including fresh-process refusal. Obsolete in-memory-only acknowledgment API and its incompatible private-access tests are archived. |
| `2026-10-31/task-4/cartographer` | Frozen autorig/key admission integrated by `e893146`, with subsequent caller repairs; current project schema 40 retained over draft schema 39. |
| `cartographer-200md-public-20261012` | Public-baseline documentation; current ignore policy includes the local authoring artifact rule. |
| `cartographer-branding-20261012` | Hydro, branding and viewport contracts already in the public baseline; current package configuration handles both named and NOCONFIG exports. Quarantined perceptual documentation stays in archive. |
| `cartographer-community-market-20261011` | Current market/signature and production-authoring implementations; bake functionality lives in `carto_production`, not a revived parallel `carto_bake`; current ImGui pin retained. |
| `cartographer-outstanding-public-20261011` | Historical public work reconciled by the public baseline; current ignore policy supersedes the local-only delta. |
| `cartographer-public-next-20261008` | Historical public work reconciled by the public baseline; current ignore policy retained. |
| `cartographer-public-release` | Historical public work reconciled by the public baseline; current ignore policy retained. |
| `cartographer-public-wip-integration-20261012` | Historical integration reconciled by the public baseline; current ignore policy retained. |
| `condition-room08-join-20261030` | Linked-scene integration carried forward through `5282159`, `94c4b69` and `85138e3`; current provider-neutral boundary retained. Historical owner-specific report archived. |
| `first-use-20261028` | First Block recovered by `f15fb14`, native re-save documented by `c44f3de`; dated first-use docs now recovered separately. |
| `terra-sprint-import-20261028` | Current governed native glTF importer, source registry and extended native-import tests; historical handoff already present. |
| `public-boundary-audit-commit` | Current scanner and its later boundary tests; old pre-redaction ancestry is deliberately not merged. |
| `Cartographer-boundary-content-guard` | Current named-target clarification, explicit choice and checkpoint identity contracts; later checkpoint versions retained over draft version 12. |
| `Cartographer-dv012-engineering` | Agent Workbench source and later readiness/authority checks already carried forward. |
| `cartographer-prepared-integration` | Current provider-neutral prepared publication and scene contracts; current ignore policy retained. |
| `texturing-workflow` | `a854432` is an ancestor of the combined candidate; final evidence/logs are preserved before this integration workspace is removed. |

### Copies retained

- **`carto-material-v5`: unresolved, not safe to delete.** Its committed
  sampler change is represented on main, and the CPU sampling work is
  reconciled with the newer guarded implementation. Its studio-lighting
  changes and uncommitted offline-glass renderer, blend-factor, expanded glTF
  limits and packed-attribute codec remain separate drafts. The codec writes
  a new `ATTRIBUTE_PACKED` representation without a distinct project schema
  version; resource limits and narrowing/malformed-payload cases need review.
  The new packed-attribute fixture covers positive round trips, but does not
  establish backwards-reader compatibility or the renderer's optical
  behavior. This integration does not claim those drafts complete.
- **Separate render repository:** the six committed fixes are integrated,
  but its running editor and its other linked submodule copies are retained.
  No Condition repository index or submodule pin is changed by this task.
  Remove the active copy only after its session is safely closed and its
  original repository's worktree metadata/dependencies are rechecked.

The adjacent-directory inventory also found eleven standalone historical
verification clones, one non-Git preservation directory, and two worktrees
registered to separate private Hub repositories. These are recorded in the
local `adjacent-copies.json`; they are outside this repository's worktree
registry and are retained. No private Hub or kernel source is admitted into
Cartographer main by treating a directory name as integration authority.

## Invariants, rejected options and risks

Source edits continue through existing validated application/command/journal
paths. The parented-instance change keeps non-human hierarchy mutation
refused. Render-derived products preserve source pixels and digest lineage;
previews do not grant picking authority. No new service permissions,
organization scope or backend state machine is introduced.

Rejected: blanket merging pre-redaction branch histories; replacing newer
source with old WIP file snapshots; reviving duplicate bake/UI implementations;
silently admitting the packed-format and optical drafts; force-discarding
unarchived files; closing the active editor; deleting rollback refs; pushing,
publishing or changing other products' pins.

Direct impact is Cartographer source, tests and documentation plus removal of
verified superseded local copies. RegOS, IDA_Training, hidden_canopy_bot and
Ask_IDA source and datasets are unchanged. Downstream C++ consumers need a
rebuild. No project schema or external API change is introduced by the
consolidation itself. Cross-product, VANTA-runtime and deployment acceptance
remain separate from local native tests. No training or promotion is claimed.

Rollback: create a new checkout at the preserved main/staging/feature or
per-tree archive ref, then restore that tree's `*-work.zip` and, when needed,
`*-evidence.zip`. The raw patches also retain staged/unstaged history context.
Do not reset a dirty checkout. The primary branch can be restored to the
recorded backup only after preserving any newer work. Local archives and refs
are intentionally retained after directory cleanup.

## Validation and final cleanup

Pending completion of the combined candidate's native checks. Test results,
exact promoted commit and cleanup totals will replace this paragraph before
the ledger is finalized. Earlier per-feature visual observations in
`texturing-workflow.md`, `masked-fur-mip-filter.md` and
`preview-thin-strand-sampling.md` are historical evidence; the combined merge
has not received a new interactive visual acceptance run.
