# ADR-0008: Sandboxed plugin default

Status: host protocol foundation accepted; OS process sandbox deferred.

## Decision

Native community plugins default to `sandboxed_process`. The host exchanges
bounded, versioned envelopes and logical input permissions. Plugins propose
neutral data; the host validates and commits through an authoring transaction.

## Non-authority boundary

No plugin protocol message is an authoring mutation by itself. Network access,
arbitrary paths, credentials, mutable project pointers, GPU handles, and
implicit process authority are outside the manifest and envelope contract.
