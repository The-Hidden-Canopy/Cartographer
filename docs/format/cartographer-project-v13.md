# Cartographer project format v13

Cartographer v13 retains the v12 records and adds a per-derived-mesh
`CLOTH_REST_POSE_RECEIPTS` table after `GARMENT_ASSETS`:

```text
CARTOGRAPHER_PROJECT 13
...
GARMENT_ASSETS <count>
GARMENT_ASSET <mesh-asset-id> <sha256-hex> "<serialized GarmentAsset bytes>"
CLOTH_REST_POSE_RECEIPTS <count>
CLOTH_REST_POSE_RECEIPT <derived-mesh-asset-id> <sha256-hex> "<serialized cloth receipt bytes>"
...
END
```

The receipt table is required in v13, including when its count is zero. Each
record refers to an existing derived mesh and carries a SHA-256 digest of the
exact serialized bytes. The project layer treats those bytes as opaque; the
cloth production layer validates the receipt's source, garment, simulation,
settled-frame, and output mesh bindings. Replacing or removing a mesh removes
its attached cloth receipt so the project cannot keep presenting provenance
for an obsolete output.

Readers reject duplicate or unknown mesh bindings, zero or mismatched
digests, empty or oversized receipt payloads, and receipt tables in v12 or
older documents. The project permits at most 100,000 receipt records and
limits each payload to 16 KiB, within the existing 128 MiB project limit.
Versions 1 through 12 remain readable; older projects load without cloth
rest-pose receipts and are saved as v13 with an empty receipt table until a
settled pose is authored.
