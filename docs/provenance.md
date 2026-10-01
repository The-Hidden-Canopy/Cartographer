# Public provenance and release boundary

Cartographer is the public, standalone authoring foundation. The public
checkout must build and test without login, network access, a model runtime,
private repositories, or hosted services.

## Public source boundary

The public repository contains generic authoring and interchange behavior:

- versioned `.carto` authoring data and validation;
- scene, geometry, topology, revision, undo/redo, and local receipts;
- OBJ, PLY, STL, and bounded glTF interchange;
- backend-neutral render-graph and GPU contracts;
- optional generic Vulkan and native-shell acceptance seams; and
- provider-neutral, revision-bound proposal contracts.

These surfaces do not contain account authority, credentials, model weights,
provider routing, private telemetry, runtime entity handles, or hosted project
state.

## Private boundary

Private integrations are maintained outside this checkout. They may consume a
versioned public artifact or proposal, but they must not become a dependency of
the public authoring graph. A private integration may add an adapter, provider,
optimization pass, catalog, workflow service, or runtime projection only from
the private side of the boundary.

The public build rejects private integration options unless an explicit
out-of-tree private source root is supplied:

```text
CARTO_BUILD_PRIVATE_INTEGRATIONS=OFF       public default
CARTO_PRIVATE_SOURCE_ROOT=<private tree>  private development only
```

This is a build boundary, not a license grant. The presence of a generic
interchange profile does not imply compatibility, endorsement, or access to a
private runtime.

## Proposal boundary

The supported Hub shape is:

```text
local revision-bound context
    -> optional Hub request
    -> typed proposal / study / export
    -> local revision and policy validation
    -> local preview
    -> local governed apply
```

The Hub never receives a mutable mesh pointer and never silently changes a
Cartographer document. Project files remain local authoring truth. Private
service state belongs in a private service record or sidecar; it must not place
credentials, prompts, model handles, telemetry payloads, or runtime entity IDs
inside ordinary `.carto` files.

## Release rule

Internal automation contracts, uncleared source specifications, private
integration adapters, generated catalogs, private benchmark data, and workflow
telemetry are not public release material. They must be cleared, relocated, or
kept in the private companion tree before publication. `.gitignore` is not a
history-remediation mechanism for material that was already tracked or
published.
