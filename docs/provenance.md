# Public provenance and release boundary

Cartographer is the public, standalone authoring foundation. The public
checkout must build and test without login, network access, a model runtime,
private repositories, or hosted services.

## Public source boundary

The public repository contains generic authoring and interchange behavior:

- versioned `.carto` authoring data and validation;
- scene, geometry, topology, revision, undo/redo, and local receipts;
- v4 `.carto` persistence for project-bound topology receipt lineage;
- OBJ, PLY, STL, and bounded glTF interchange;
- backend-neutral render-graph and GPU contracts;
- local precision-role and kernel execution receipts with explicit artifact
  identity and no outbound telemetry;
- optional generic Vulkan and native-shell acceptance seams; and
- provider-neutral, revision-bound proposal contracts.

The public C++ export surface for polygon-kernel lineage is intentionally
narrow and installable without private integrations:

```cpp
#include <carto/geometry/mesh.hpp>
#include <carto/geometry/topology_edit.hpp>
#include <carto/mesh_attributes/provenance.hpp>
```

`carto_geometry` owns validated `TopologyEditReceipt` values and deterministic
edge-loop, edge-ring, boundary-loop, vertex-fan, face-region, linked-component,
and shortest-path traversal. `carto_mesh_attributes`
owns the `provenance_from_receipt` adapter. It requires the source revision to
match `receipt.revision_before` and the destination revision to match
`receipt.revision_after`; source IDs are resolved within their declared domain,
and unknown IDs fail closed. The adapter produces source-index provenance for
`carto_attributes::transfer`, not a second mutation or persistence path.

`ProjectDocument` owns the separate durable `TopologyReceiptRecord` ledger in
project schema v4. A project-bound extrusion, inset, face deletion, or edge
split supplies a receipt to the revision-bound mesh replacement, so the mesh
snapshot and its lineage record publish in one project revision. The same
receipt-aware replacement contract is available through `ProjectTransaction`,
so its staged document and journal snapshot preserve the same lineage. Save/load
round-trips the bounded receipt codec and rejects duplicate, future, dangling,
or malformed records. The ledger is historical metadata: undo and redo publish
authoritative snapshots without fabricating inverse receipts. Direct kernel
calls, other replacement paths, and future modeling operators must opt into
the API before they can claim durable lineage. Receipt decoding also applies
bounded byte and aggregate identity/source-element budgets before allocating
large lineage containers.

These surfaces do not contain account authority, credentials, model weights,
provider routing, private telemetry, runtime entity handles, or hosted project
state.

Kernel evidence is intentionally local and operator-readable. It distinguishes
requested representation from actual arithmetic/path and records fallback
reasons without collecting usage, timing, power, prompts, model handles, or
account identity.

## Moat boundary

The public default is open Canopy. The permanently private boundary is the
moat: model weights and private prompts, provider credentials and routing,
proprietary native kernels and optimization recipes, private telemetry and
unreleased datasets, hosted entitlements or service state, credentials,
internal automation, and uncleared source specifications. These must not become
dependencies of the public authoring graph or ordinary `.carto` files.

OWH, GMIB, simulation, anatomy, and other ordinary integration work may be
maintained outside this checkout while provenance, licensing, or release review
is incomplete. That quarantine is provisional; it is not a claim that the
integration is part of the moat. Once cleared, the public contract should be
usable without a private runtime.

The public build rejects moat or not-yet-cleared integration options unless an
explicit out-of-tree private source root is supplied:

```text
CARTO_BUILD_PRIVATE_INTEGRATIONS=OFF       public default
CARTO_PRIVATE_SOURCE_ROOT=<private tree>  private development only
```

The private root and any optional GMIB root must resolve outside the public
checkout. The configure step fails closed if either root points into this
repository, including through a filesystem alias or symlink.

This is a build boundary, not a license grant. The presence of a generic
interchange profile does not imply compatibility, endorsement, or access to a
private runtime.

## Proposal boundary

The supported Hub shape is:

```text
local revision-bound context
    -> optional Hub request
    -> typed proposal / study / export
    -> local revision and policy validation
    -> local preview
    -> local governed apply
```

The Hub never receives a mutable mesh pointer and never silently changes a
Cartographer document. Project files remain local authoring truth. Private
service state belongs in a private service record or sidecar; it must not place
credentials, prompts, model handles, telemetry payloads, or runtime entity IDs
inside ordinary `.carto` files.

## Release rule

Moat assets are not public release material. Ordinary integration adapters,
generated catalogs, benchmark data, and workflow components must be cleared by
provenance and licensing review before publication, but they are not
automatically moat material. `.gitignore` is not a history-remediation
mechanism for material that was already tracked or published.

Before a public push, inspect every reachable ref and object, not only the
working tree. Deleting a moat asset from `HEAD` does not remove its historical
blob from a public remote. A reachable moat asset requires owner-approved
history remediation and remote verification before the repository can be
described as publication-safe. Automatic hosted CI is currently disabled to
avoid CI spend; the local release command remains blocking for unclassified
legacy integration paths and must be used before publication. If CI is
re-enabled, it must run that same blocking command. The audit does not rewrite
history or infer a license classification from file ownership.
