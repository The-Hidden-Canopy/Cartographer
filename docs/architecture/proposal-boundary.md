# Private authoring-service boundary

The public Cartographer build contains no authoring planner, operation catalog,
remote service client, account authority, provider routing, model asset,
telemetry sink, or service-to-project mutation path. Its Drafting Board is a
local notes surface only.

Private authoring services are packaged and operated outside this repository.
They may interact with a private deployment only through that deployment's
authenticated, organization-scoped, allowlisted artifact boundary. No private
source, executable path, command line, environment, raw diagnostic, or
operation catalog is part of the public SDK or project format.

The public editor continues to own project validation and its human command
path. Enabling an out-of-tree private integration is a separate build and
deployment decision; the default public build does not silently fall back to
one or present unavailable service data as live.
