# Cartographer project schema v27

Schema v27 preserves a bounded botanical-generation receipt inside source
import records for generated glTF meshes. The receipt contains the canonical
botanical source snapshot, its input digest, a digest of the opaque job identity,
and the output artifact digest. The surrounding source record binds the exact
generated glTF bytes to the imported mesh geometry digest and content-addressed
project-relative blob.

The receipt intentionally excludes service URLs, credentials, organization
identities, raw job IDs, and private package identifiers. It records the
response lineage Cartographer verified over HTTPS; it is not a vendor signature
and does not independently prove who operated the service. A generated mesh is
still admitted only through the human-reviewed source-import command. Undo,
redo, save, and reopen preserve or remove the source record and mesh together.

Schema v26 projects remain readable. A v26 project containing a v2 source
import record with a botanical-generation receipt is rejected rather than
silently dropping that provenance.
