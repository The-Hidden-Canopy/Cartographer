# Capability provider registry

`carto_providers` resolves capabilities instead of naming a concrete private
or vendor implementation. Built-in and future external providers use the same
descriptor and lifecycle path.

## Lifecycle

```text
discovered -> inspected -> registered -> verified -> ready -> loaded
```

Each stage transition is explicit. Version mismatch, missing dependency,
untrusted provider, load failure, unsupported capability, and unavailability
remain distinct states. A capability query returns a ready/loaded descriptor;
if a matching provider exists but is not ready, the diagnostic includes the
provider states rather than flattening the failure into a generic plugin error.

Provider descriptors contain only bounded metadata: stable ID, kind,
capabilities, semantic version, and trust class. The registry does not load
code, grant permissions, access a project, or imply that a provider has been
verified merely because it was discovered.

## Built-ins and future adapters

The in-process OBJ, PLY, STL, and deterministic glTF interchange functions are
registered through this path and the CLI requires their capabilities to be
ready before invoking them. The actual functions remain statically linked and
deterministic; registry resolution is a lifecycle/authority gate, not dynamic
loading. Hosted providers and private runtime adapters remain outside the
public target graph and require their own release decision and evidence.
