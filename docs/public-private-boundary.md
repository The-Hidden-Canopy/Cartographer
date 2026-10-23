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
| provider-neutral scientific entities, fields, studies, results, and provenance | private genome interpretation, solver binaries, anatomy catalogs, and unpublished scientific datasets |
| revision/digest-bound biological authoring records for microstructure, anatomy, tissue, morphology, and representation correspondence | private biological solvers, runtime lowering policy, game-owned organism history, and proprietary organism datasets |
| revision/digest-bound SculptStack and GarmentAsset authoring, constraints, and prepared-product receipts | private brush/remesh/cloth kernels, native solver policy, runtime cloth state, and game-owned clothing history |
| geometry, topology, evaluation, and mesh-attribute kernels | proprietary native kernels, protected adaptive-representation and continuity algorithms, optimization recipes, and tuning data |
| scene, project, revision, undo/redo, and receipts | private telemetry, workflow-learning stores, and unreleased datasets |
| provider-neutral semantic areas/paths, structure graphs, reconstructible instance fields, and sparse variants | runtime materialization policy, proprietary world generation, and unreleased game-world corpora |
| OBJ, PLY, STL, glTF, and other reviewed interchange | private schemas, catalogs, and hosted-service state |
| render-graph, RHI, CPU reference, and clean-room GPU contracts | proprietary GPU passes, performance recipes, and entitlements |
| generic Vulkan/D3D12/backend acceptance and offline benchmarks | credentials, internal automation, and uncleared source specifications |
| offline UI/workbench and provider-neutral proposal contracts | account authority and service-side collaboration state |
| the explicit botanical runtime client protocol and typed source/output receipts | botanical generation implementation/policy, credentials, and hosted job state |

The public side may expose a typed context and a typed proposal contract. It may
not expose raw mutable authoring pointers or permit a remote service to bypass
local validation.

Protected algorithm surfaces include adaptive representation/certification
policy, persistent topological continuity identity, field-growth or other
private geometry-generation previews, and their benchmarks and specifications.
They live in the adjacent private companion tree. The public checkout has no
target, header, test, benchmark, or documentation path for those surfaces.

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
- private sandbox policy, private runtime payloads, or replay evidence that has
  not cleared its release boundary.

Public packages may contain only public verification keys or test-only key
material. A verified signature establishes package identity; capability
admission and any future sandbox or transaction boundary remain separate.

## Project-file rule

The core `.carto` document remains public and versioned. Optional public
extensions must be namespaced and versioned independently. Hub-specific state
belongs outside the ordinary project file or is represented only by an opaque
reference that has no authority by itself.

The v5 scientific record is intentionally generic: typed IDs and provenance
can be shared publicly, but it does not carry private telemetry, credentials,
provider runtime handles, model weights, or execution authority. Provider
publication manifests carry only reviewed semantic IDs, source geometry/license
lineage, and opaque candidate references; they do not admit private assets or
runtime components. A result field is not source truth, and a
provider-import preservation report carries only bounded source keys,
disposition labels, targets, and notes—not imported payloads, credentials, or
provider execution state. Functional-genome exchange envelopes carry the
public semantic payload, source receipt, and canonical digest only; they do not
carry private interpretation logic, provider handles, or runtime
admission state. The derived development timeline is read-only
inspection data and is not a second public source of truth. The bilateral
landmark, measurement, and evolutionary-morphospace previews are likewise
deterministic local projections; the morphospace surface contains only
explicit lineage/signature bindings and derived coordinates. It does not
contain private morphospace models, alignment weights, genome interpretation
logic, or provider output.
sequence/adaptation payload is not implicitly a functional genome.

The v7 world record is likewise generic authoring truth. It stores stable
semantic identity, explicit Cartographer geometry bindings, bounded path and
structure topology, deterministic instance recipe/seed/extent metadata, and
sparse canonical property edits. It does not store a private generator,
adaptive representation or certification policy, runtime entity handles,
private residency state, private game content, or execution telemetry.

The anatomy provider candidate envelope is likewise provider-neutral. It carries
source revision and geometry/manifest digests, but no provider credentials,
runtime entity handles, private catalog payloads, or private admission state.

The following do not belong in public project files:

- credentials or bearer tokens;
- model prompts, weights, or provider handles;
- private telemetry or workflow history;
- runtime entity/device handles;
- private service URLs or arbitrary commands; and
- hidden mutation or promotion instructions.

## Repository rule

The default build is the public foundation. Private runtime and uncleared
integration source trees are not CMake inputs and cannot be added to the
Cartographer target graph through an out-of-tree source-root option. Private
services may be developed separately; any future connection must use an
explicit provider-neutral API and must not embed their runtime into Cartographer.
The standalone project remains usable without such a service.

Project recovery sidecars are local state rather than source: `.carto.journal`,
`.carto.lock`, `.journal.lock`, and temporary `.carto.tmp-*` publication files
are quarantined by the repository ignore policy and are not part of a public
commit.

The existing `carto_runtime_api` botanical client is a public protocol adapter,
not the private service or runtime. Its fixed submit, status, and artifact
receipt contract is intentionally reviewable source. It sends data only after
an explicit user action, reads credentials and the service origin from process
environment variables, and never stores those values or hosted job/artifact
identities in `.carto` files. Downloaded output remains untrusted until local
revision/digest validation and a separate user-triggered import through the
project journal. This classification covers this narrow botanical contract;
it does not make other private service schemas, policies, catalogs, or runtime
implementations public.

The local branch-graph and plant-representation generator in `libs/botany`
implements botanical geometry generation, so it remains private pending an
explicit owner release review. The public source/receipt contract can still be
used by the separately configured runtime client; it does not publish this
generator or its policy.

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
files, recognizable credential patterns, workstation-specific paths, required
quarantine rules, and all reachable Git history. The tracked-but-ignored check
is explicit because
`.gitignore` is not retroactive. It never rewrites history, expires reflogs,
deletes files, or contacts a remote. Use `--include-reflog` for a local hygiene
audit that also inspects reflog/unreachable objects. Legacy integration paths
are reported for provenance/licensing review and are never cleared by an
in-repository donor-name/commit allowlist.
The audit also scans current text and reachable history for known protected
field-growth implementation markers, so placing a private preview under a
generic geometry directory does not bypass the path quarantine.
It also rejects build and source references to the quarantined botanical
generator, including target, include, and namespace references; an ignore rule
does not make a build-graph dependency private-safe.
`--fail-on-review` makes any unclassified review item blocking for an
owner-approved release gate. Automatic hosted CI is currently disabled to
avoid CI spend; if CI is re-enabled, it must invoke the same blocking mode so
an unclassified legacy path cannot pass the public build merely because it is
absent from the current checkout. Clearing the gate requires provenance/
licensing classification or owner-approved history remediation; this
repository does not rewrite history automatically.
