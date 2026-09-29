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

## Required host flow

```text
manifest -> inspect -> capability registry -> bounded frame
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
