# ADR-0007: Capability provider registry

Status: metadata/lifecycle foundation and OBJ built-in adapter gate accepted;
dynamic/external adapter migration deferred.

## Decision

Resolve providers by bounded capability IDs and explicit lifecycle state. The
registry retains provider kind, version, trust class, and distinct failure
states. Built-ins use the same registration path as future extensions; the
headless CLI now registers and resolves the OBJ importer/exporter before
calling their statically linked implementations.

## Rejected

The host does not dispatch by private class name, treat discovery as readiness,
or collapse version/dependency/trust/load failures into one boolean.
