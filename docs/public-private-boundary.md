# Cartographer public/private split

Cartographer is open by default. The standalone authoring foundation and
ordinary integrations should be reviewable, buildable, and usable without a
Hub service. The Hub does not become a runtime requirement for opening,
editing, validating, or exporting a project.

The private boundary is the moat, not a synonym for anything developed outside
this checkout. A companion tree may temporarily hold work while provenance,
licensing, or release review is completed, but that does not make the work
permanently proprietary.

| Public / open-by-default Canopy | Private moat |
| --- | --- |
| `.carto` authoring format and validation | model weights, private prompts, provider credentials, and routing policy |
| geometry, topology, evaluation, and mesh-attribute kernels | proprietary native kernels, optimization recipes, and tuning data |
| scene, project, revision, undo/redo, and receipts | private telemetry, workflow-learning stores, and unreleased datasets |
| OBJ, PLY, STL, glTF, and other reviewed interchange | private schemas, catalogs, and hosted-service state |
| render-graph, RHI, CPU reference, and clean-room GPU contracts | proprietary GPU passes, performance recipes, and entitlements |
| generic Vulkan/D3D12/backend acceptance and offline benchmarks | credentials, internal automation, and uncleared source specifications |
| offline UI/workbench and provider-neutral proposal contracts | account authority and service-side collaboration state |

The public side may expose a typed context and a typed proposal contract. It may
not expose raw mutable authoring pointers or permit a remote service to bypass
local validation.

## Plugin-package rule

The local `.cartoplug` foundation is public: the manifest schema, bounded
archive reader, canonical digests, signature verification contract, install
receipt shape, and adversarial fixtures are reviewable source. Those surfaces
identify and constrain a package; they do not grant project, network, or
runtime authority.

The following remain private and must stay outside the public checkout and
public package fixtures:

- Ed25519 signing private keys and publisher key-management material;
- Hub credentials, private catalog endpoints, entitlement state, and update
  policy;
- private plugin binaries, proprietary kernels, optimization recipes, and
  unreleased execution telemetry; and
- private sandbox policy, VANTA runtime payloads, or replay evidence that has
  not cleared its release boundary.

Public packages may contain only public verification keys or test-only key
material. A verified signature establishes package identity; capability
admission and any future sandbox or transaction boundary remain separate.

## Project-file rule

The core `.carto` document remains public and versioned. Optional public
extensions must be namespaced and versioned independently. Hub-specific state
belongs outside the ordinary project file or is represented only by an opaque
reference that has no authority by itself.

The following do not belong in public project files:

- credentials or bearer tokens;
- model prompts, weights, or provider handles;
- private telemetry or workflow history;
- runtime entity/device handles;
- private service URLs or arbitrary commands; and
- hidden mutation or promotion instructions.

## Repository rule

The default build is the public foundation. The moat categories above are kept
out of the public tree and are never required by the public build. OWH, GMIB,
simulation, anatomy, and other ordinary integration work may be developed in
the adjacent companion tree while provenance and licensing are being cleared;
they are open-by-default candidates, not an automatic permanent exclusion.

Private development can use the same public contracts by configuring an
explicit out-of-tree source root. CMake rejects a moat or not-yet-cleared
source root or GMIB root located inside the public checkout, so an ignored
`private/` or `moat/` directory cannot silently become part of a supposedly
public configuration. Those paths are deliberately not part of the public
presets, release instructions, or runtime dependency graph.

Project recovery sidecars are local state rather than source: `.carto.journal`,
`.carto.lock`, `.journal.lock`, and temporary `.carto.tmp-*` publication files
are quarantined by the repository ignore policy and are not part of a public
commit.

## History rule

The publication check covers reachable Git history as well as the current
checkout. A deleted PDF, archive, model, credential, or source specification
can remain recoverable from a public remote. If a moat object is found in
reachable history, stop the release, obtain explicit owner approval for history
rewriting or remote purge, and verify every public ref after remediation.

## Automated publication check

The repository includes a read-only publication audit:

```text
python scripts/public_boundary_audit.py --check-ignore-policy
```

It checks tracked files, tracked-but-ignored files, non-ignored working-tree
files, recognizable credential patterns, required quarantine rules, and all
reachable Git history. The tracked-but-ignored check is explicit because
`.gitignore` is not retroactive. It never rewrites history, expires reflogs,
deletes files, or contacts a remote. Use `--include-reflog` for a local hygiene
audit that also inspects reflog/unreachable objects. Legacy GMIB, anatomy, OWH,
and simulation paths are reported for provenance/licensing review;
`--fail-on-review` makes those review items blocking for an owner-approved
release gate. Automatic hosted CI is currently disabled to avoid CI spend; if
CI is re-enabled, it must invoke the same blocking mode so an unclassified
legacy path cannot pass the public build merely because it is absent from the
current checkout. Clearing the gate requires provenance/licensing
classification or owner-approved history remediation; this repository does not
rewrite history automatically.
