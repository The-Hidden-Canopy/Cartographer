# Cartographer project schema v36

Schema v36 adds an `ACCEPTED_AUTORIGS` table after `KINEMATIC_GRAPHS` and
before the project's outer `END`. Each `ACCEPTED_AUTORIG` row contains a
project-local identity, record revision, SHA-256 digest, and quoted canonical
bundle bytes. The table is mandatory for v36, including when empty. Schemas
1 through 35 remain readable with an empty table; a table labeled as an
older schema is refused. The project serializes older documents as the
current schema on save.

The version-one bundle contains the accepted review and preview-to-durable
joint mapping with canonical kinematic graph, a canonical `RigDefinition`
including its control graph, the referenced `JointLimitSet`, and a
`TransferredWeightRecord` containing the full `SkinBinding`, layer metadata,
and transfer receipt. Component codecs and cross-references are validated:
graph and limit digests, control and symmetry joint references, binding
graph and joint references, and the binding target's asserted mesh identity
and digest must agree. The outer row digest binds the exact bundle bytes.

`ProjectTransaction::accept_autorig` checks the staged source mesh against
the accepted candidate set and review, uses explicitly supplied durable
joint IDs and parent-local rest transforms, and performs the requested weight
transfer before staging one bundle. Composed rest positions must agree with
reviewed anchors. Omitted candidate parts and mechanical port evidence are
refused rather than silently discarded. Insert requires expected revision zero;
replacement requires the current record revision. A byte-identical update is
idempotent. Commit uses the existing whole-project journal transaction.

The table permits at most 512 rows and 64 MiB aggregate bundle bytes. The
separate whole-project journal payload limit and framing apply as well, so an
admitted bundle near that bound may still fail to commit; that failure must
leave the published project unchanged. Embedded records have their own
component bounds.

`decode_accepted_autorig_bundle` checks historical internal consistency; it
does not authenticate a reviewer or establish that the mesh is still current.
Call `validate_accepted_autorig_current` against the current document before
using a decoded bundle. A later mesh edit or removal does not automatically
change the stored layer's freshness field. The source content digest and
input binding provenance are caller assertions, not independently verified
external source bytes. Sparse target correspondence is allowed: vertices
without transferred influences remain rigid.

This schema does not persist an IK/FK chain, control mapping, mode switch, or
solver configuration. Generic control definitions and graph data do not
establish those semantics. Callers must provide the relevant evaluation inputs
explicitly; this is not a complete persistent IK/FK authoring setup. No native
editor or independently authored asset acceptance is implied by this format.
