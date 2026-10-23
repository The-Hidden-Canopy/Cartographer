# Cartographer project schema v25

Schema v25 appends `BIOLOGICAL_AUTHORING_SOURCES` and
`BIOLOGICAL_AUTHORING_LINEAGE` tables after the source asset registry
and sidecar lineage records. The two tables persist external biology
provider-authored biological documents as project-owned provenance.

The `BIOLOGICAL_AUTHORING_SOURCES` table stores, per source namespace and
asset identity, the exact canonical serialized `BiologicalAuthoringDocument`
bytes and their SHA-256 digest. Readers re-hash the embedded bytes, require
the digest to match, deserialize the document, and require the canonical
re-serialization, embedded namespace, and embedded asset identity to agree
with the record key. Source count and per-source payload bytes are bounded;
duplicate identities reject.

The `BIOLOGICAL_AUTHORING_LINEAGE` table records the latest source lineage
revision per identity. Revisions must be nonzero and bounded; duplicate
identities reject. Admission binds edits to exact-next revisions so that
undo, redo, deletion, and later recreation cannot reuse an earlier source
revision.

Both tables are mandatory in v25: a v25 project missing either table
rejects. Schema 24 and older projects remain readable without these tables
and gain no invented biological source values; they seed no lineage for
sources that are not already present. Serialization writes schema 25 with
both tables. These tables are provenance for authored biological documents;
they do not certify anatomical accuracy, biological simulation, or private
downstream-runtime compatibility.
