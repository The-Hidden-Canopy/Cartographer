# Cartographer project schema v31

Schema v31 adds the mandatory `DECAL_PLACEMENTS` table after the v30
`RIG_MODULE_TEMPLATES` table, including when empty. Each `DECAL_PLACEMENT`
record stores a stable nonzero placement ID, a project-local revision, a
SHA-256 digest, and the exact canonical decal placement bytes. The project
layer validates bounds and the digest; the production decal codec validates
the payload, and its loader also checks that the embedded ID and revision
match the project record.

Placement changes go through `ProjectTransaction`, require the expected
current placement revision (zero for creation), and advance the placement and
project revisions together. A successful transaction is journaled before the
staged document is published. The table is bounded to 100,000 entries, 1 MiB
per placement, and 64 MiB aggregate serialized bytes. Schema v31 does not
claim a native viewport placement workflow.

Schemas v1-v30 remain readable. Migration initializes the v31 placement table
as empty; it never infers or fabricates placements.
