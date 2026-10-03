# ADR-0008: Sandboxed plugin default

Status: host protocol foundation accepted; OS process sandbox deferred.

## Decision

Native community plugins default to `sandboxed_process`. The host exchanges
bounded, versioned envelopes and logical input permissions. Plugins return
request-bound neutral data; the host validates the optional source revision
and later performs domain validation before any authoring transaction.

## Non-authority boundary

No plugin protocol message or `NeutralResult` is an authoring mutation by
itself. Network access,
arbitrary paths, credentials, mutable project pointers, GPU handles, and
implicit process authority are outside the manifest and envelope contract.
The currently declared project-write permission is rejected until a host-side
admission and `ProjectTransaction` path exists. A valid frame must also use a
capability explicitly granted by its manifest; syntax validation is not
authorization.
