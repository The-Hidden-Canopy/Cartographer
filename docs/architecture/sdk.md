# Stable native SDK boundary

`carto_sdk` exposes a small C ABI over the headless application boundary. The
header uses opaque context/document pointers, fixed-width integer types, an
explicit ABI version, status codes, and caller-visible ownership rules.

## Implemented surface

- `carto_abi_version()` reports the ABI version.
- context creation is bounded to 64 KiB of optional configuration text;
- context configuration may set a `project_root`; open and save paths are
  resolved beneath that root, reject `..` traversal and outside-root paths,
  and resolve existing links before containment validation;
- document open delegates to the application session and fails closed on load
  or journal-integrity errors;
- query/execute requests and responses are bounded to 64 KiB;
- JSON response buffers and diagnostic messages use `malloc` and are released
  through `carto_free`;
- query and execute catch exceptions before they cross the C boundary.

The default root is the process working directory. A caller that needs a
different project location must provide `{"project_root":"..."}` at context
creation; the context owns this policy and the document copies it so destroying
the context cannot invalidate an open document's path boundary.

The C ABI does not expose C++ object layout, editable mesh pointers, GPU
handles, SQLite objects, or private repository types. Documents remain
application-session boundaries, and callers do not receive arbitrary mutation
callbacks.

## Deferred bindings and compatibility

The current ABI is a smoke-tested C surface, not a complete SDK. C++ wrapper,
Python binding, C# binding, version negotiation beyond the integer version,
threading rules, shared-library packaging, and downstream language acceptance
remain deferred. ABI changes require an explicit version decision and a C smoke
fixture; adding an operation without updating the capability response is a
contract error.
