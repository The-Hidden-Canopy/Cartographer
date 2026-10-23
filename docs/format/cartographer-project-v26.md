# Cartographer project schema v26

Schema v26 adds a required `BIOLOGICAL_PROXY_INSTANCE_RECEIPTS` table after
the biological source and lineage tables. Each record binds one editable scene
object and mesh asset to:

- source namespace, asset identity, and exact source revision;
- the structure identity used by the clean-room ellipsoid proxy compiler;
- the canonical serialized source snapshot and its SHA-256 digest; and
- the deterministic proxy product digest.

Readers validate the snapshot's canonical encoding and source identity, compile
the product again, and require its digest to match. Project validation also
checks object-to-mesh attachment and that retained source lineage is not older
than the receipt. Receipt count and aggregate embedded source bytes are bounded.
Missing, malformed, stale, or altered receipts reject the project; readers do
not fabricate scene provenance.

Schema 25 projects remain readable with no proxy-receipt records in memory.
Serialization writes schema 26 and its required table. The receipt preserves
historical source bytes when the active authoring source advances, so the
committed mesh remains traceable to the revision that produced it.

This table is provenance for a coarse editable proxy only. It does not certify
anatomical accuracy, biological simulation, private geometry evaluation, rendered
equivalence, or private downstream-runtime compatibility.
