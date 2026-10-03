# `.cartoplug` package format

Cartographer's first plugin package wave is a local inspection and install
boundary. It is deliberately independent of Hub and VANTA: a package can be
inspected and installed from a local path without granting either system
project authority.

## Layout

The package is a ZIP archive with bounded relative paths:

```text
manifest.json
signature.json                         optional
cartographer/<platform>/<entrypoint>  declared executable, optional
vanta/<platform>/<entrypoint>          optional companion, later execution wave
docs/                                  optional
licenses/                              optional
assets/                                optional
```

The current reader accepts stored ZIP entries only. Compression, encrypted
entries, data descriptors, symlink attributes, duplicate paths, traversal and
absolute paths, central/local-directory disagreement, and archive or entry
budget violations are rejected before the package becomes an install
candidate. The implementation does not extract files during inspection.

## Manifest identity

`manifest.json` uses `cartographer-plugin.v1` and carries:

- `plugin_id`, `display_name`, `publisher`, `version`;
- `cartographer_api` and `min_cartographer_version`;
- `trust_class`: `sandboxed_process`, `trusted_native`, or `data_only`;
- capability tokens and bounded logical filesystem input names;
- explicit permissions, with network and project-write authority rejected by
  the current host;
- platform entrypoints and SHA-256 digests; and
- optional VANTA companion metadata.

The manifest is parsed with the shared duplicate-key-rejecting JSON boundary,
then serialized into a canonical form before its `manifest_digest` is made.
The package verifier reparses the archive manifest and rejects a mismatch
between the supplied inspected identity and the archive's canonical identity.

## Signature and receipt binding

The signature message is a length-prefixed sequence containing the signature
domain, key ID, algorithm, canonical manifest digest, and deterministic content
digest. The content digest covers sorted non-signature entries by path, path
length, bytes length, and bytes; excluding `signature.json` avoids a circular
digest. The raw archive SHA-256 is separately stored as `package_digest` in the
install receipt.

Verification rejects unknown keys, unsupported algorithms, malformed signatures,
non-canonical Ed25519 scalars/points, and small-subgroup points. Unsigned
packages require explicit Developer Mode and cannot install as
`trusted_native`. Native trust therefore requires both a declared native trust
class and a verified signature.

An install receipt records package identity, publisher, raw package digest,
manifest digest, signature verdict, trust class, capabilities, install time,
and Cartographer version. Listing installed packages rechecks the package file
against the receipt digest and rejects stale identity or digest state.

## Explicit boundary

This format layer is not a plugin host. It does not execute an entrypoint,
create an OS sandbox, grant network or project-write authority, admit geometry,
resolve a Hub catalog, update a package, or run a VANTA companion. Those
capabilities require the later execution and admission waves from the plugin
ecosystem docuseries.
