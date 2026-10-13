# Cartographer project format v14

Cartographer v14 retains the v13 records and adds digest-bound engineering
authoring and CONDITION source-authoring records after
`CLOTH_REST_POSE_RECEIPTS`:

```text
CARTOGRAPHER_PROJECT 14
...
CLOTH_REST_POSE_RECEIPTS <count>
CLOTH_REST_POSE_RECEIPT <derived-mesh-asset-id> <sha256-hex> "<serialized cloth receipt bytes>"
ENGINEERING_AUTHORING <sha256-hex> "<serialized engineering authoring bytes>"  # optional
CONDITION_AUTHORING NONE
# or: CONDITION_AUTHORING <sha256-hex> "<serialized CONDITION authoring bytes>"
...
END
```

The `CONDITION_AUTHORING` record is required in v14, including when no
CONDITION source document is bound. Engineering authoring is optional. For
either payload, the digest covers the exact serialized bytes. Readers validate
the digest, size limit, and domain document before accepting the project. Each
payload is limited to 32 MiB within the existing 128 MiB project limit.

CONDITION records represent source authoring only. They do not carry runtime
condition, wear, or game-owned state. Engineering records preserve inspection
authoring and evidence; their publication package still requires explicit
admission at its consumer boundary. A stale or invalid source record remains
visible as such and cannot be published as an admitted package.

Versions 1 through 13 remain readable. Older documents load without these
authoring records and are saved as v14 with no engineering record and
`CONDITION_AUTHORING NONE` until authoring is added. Readers reject either new
record in a document whose schema version predates v14.
