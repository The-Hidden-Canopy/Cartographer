# Plugin isolation boundary

`carto_plugin_protocol` is the host-side contract for future extensions. It
does not start a process or grant authority.

## Implemented foundation

- plugin manifests are bounded and use a stable API version;
- the default native trust class is `sandboxed_process`;
- implicit remote-service and network permissions are rejected;
- project-write permission requests are rejected until a host transaction
  admission path exists;
- filesystem permissions are logical bounded input names, not arbitrary paths;
- request envelopes use `carto.plugin.v1` and a four-byte length-prefixed frame;
- frame size, request ID, capability, method, and object payload boundaries are
  validated before a host can consider dispatching them.

## Package inspection and local install foundation

The first plugin-ecosystem wave is implemented in `carto_plugin_package` and
is intentionally local-first. A `.cartoplug` is a bounded ZIP package with a
`manifest.json`, optional resources and platform entrypoints, and an optional
`signature.json`. The reader currently accepts stored ZIP entries only; it
rejects traversal, absolute or backslash paths, duplicate entries, symlinks,
encrypted/data-descriptor entries, central/local-directory disagreement,
entrypoint digest mismatches, and archive/file size violations before any
package is installed.

Verification binds the canonical manifest and a deterministic content digest of
all entries except `signature.json` (the raw archive digest is retained in the
install receipt). Ed25519 signatures are checked against an explicit local
keyring, including canonical field/scalar encodings and small-subgroup
rejection. Unsigned packages are installable only in explicit Developer Mode,
and never for `trusted_native`; every successful local install writes an
identity- and package-digest-bound receipt. The receipt listing path also
rechecks the installed package digest so stale or tampered receipts do not
become trusted state.

This wave does not claim process isolation, OS sandboxing, plugin execution,
geometry admission, Hub/catalog authority, update resolution, network
permissions, or VANTA runtime control. Those are later waves and remain
outside the current host authority boundary. The signing primitive has standard
RFC8032 vector coverage but is not an external cryptographic-audit claim.

## Required host flow

```text
package -> manifest -> inspect -> capability registry -> bounded frame
         -> sandbox process -> neutral proposal
         -> host geometry/size validation -> ProjectTransaction
```

The plugin never receives a `ProjectDocument*`, mutable mesh pointer, GPU
handle, credential, or arbitrary filesystem authority. A malformed frame,
oversized payload, process crash, timeout, or invalid geometry result must stay
outside authoring truth. Process limits, cancellation, OS sandbox policy, and
neutral-result validation remain deferred runtime work. Before a host dispatches
an envelope, it must validate the manifest and require that the envelope
capability is explicitly listed by that manifest; envelope framing alone is not
an authority grant.
