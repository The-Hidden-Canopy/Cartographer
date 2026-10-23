# ADR-0006: Public C ABI

Status: initial C surface accepted; opt-in shared-library packaging and bounded
ABI-range negotiation accepted; language bindings deferred.

## Decision

Expose a deliberately small C ABI over the application/session boundary. Use
opaque handles, fixed-width types, explicit status values, bounded strings, and
one allocator/free function for returned buffers.

## Invariants

- no C++ layout or STL type crosses the boundary;
- every output pointer is initialized or rejected;
- returned error/response allocations have one documented release path;
- malformed, oversized, unknown, or unsupported requests fail without a
  mutation receipt;
- exceptions are translated to a status and never escape an exported call;
- the ABI version remains explicit and test-covered.
- filesystem paths are root-scoped by the context; absolute outside-root paths,
  `..` components, and resolved link escapes are rejected before open/save.

## Rejected

The SDK does not expose `ProjectDocument*`, mutable mesh pointers, GPU native
handles, direct SQL, provider credentials, or private Hidden Canopy types.
