# Cartographer project format v10

Cartographer v10 is the current deterministic `.carto` authoring format. It
retains every v9 record and adds one optional digest-bound semantic sidecar
record between the material catalog and the end marker:

```text
CARTOGRAPHER_PROJECT 10
AUTHORING "cartographer.authoring" "meters" "right_handed_y_up"
SOURCE_NAMESPACE "cartographer.project.<stable-id>"
NAME "..."
REVISION <unsigned integer>
...
MATERIAL_CATALOG "<versioned source catalog>"
SEMANTIC_SIDECAR "<sha256-hex>" "<opaque serialized sidecar bytes>"
END
```

The `SEMANTIC_SIDECAR` record is present only when the project carries
semantic extension state. Its first field is the SHA-256 digest that binds
the exact serialized sidecar bytes; the second field is the opaque,
self-delimited serialized form of one `carto::semantic::SemanticSidecar`.
ProjectDocument stores the bytes verbatim: it verifies that the recorded
digest equals the SHA-256 of the exact bytes and never parses the payload,
so the project format gains no dependency on `carto_semantic` and semantic
records gain no mutation authority over the document. Serialization-ready
consumers reparse the bytes through the semantic library's own fail-closed
deserializer.

The sidecar record is rejected when the digest is missing, malformed, zero,
or does not match the bytes, when the byte payload exceeds the 16 MiB sidecar
bound, or when a v9-or-older file claims the record (a precise
`version_mismatch` failure). A v10 file without the record is valid and
simply carries no semantic extensions.

Versions 1 through 9 remain readable. v9 files contain no sidecar record and
upgrade silently: they load as v10 documents with empty sidecar attachment
and the next supported save persists them as v10. Runtime handles, engine
types, game-owned state, and credentials remain outside authoring truth;
the sidecar binds only neutral, versioned, digest-bound extension records to
stable authored identities.
