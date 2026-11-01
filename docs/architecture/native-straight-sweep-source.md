# Native straight sweep source measure

`geometry::measure_straight_sweep` evaluates a retained `SweepSource` and its
exact revision-bound `CurvePath`. It supports one axis-aligned cubic segment
whose control coordinates are strictly monotone, a constant-scale filled
convex profile, and both caps. It reuses the sweep kernel's profile-area and
initial-frame calculations. The model's cross-sectional area times centerline
length gives its source-model volume; curved, hollow, open, tapered, reversing,
multi-segment and unsupported sources refuse. The calculation uses binary64;
no area, length or volume numerical error bound is certified. It is not a
measurement of a manufactured object, mass, material, or physical acceptance.

`native_source::derive_project_straight_sweep` copies one project snapshot,
resolves an existing selected root mesh object and the explicitly named sweep
and centerline in that object's frame, invokes the existing sweep builder, and
requires the regenerated editable-mesh snapshot digest to equal the selected
mesh snapshot digest. It binds the exact serialized source and centerline,
their revisions, the native project source projection and a content-derived
operation identity. Its selected mesh carries the existing authored face IDs
and viewport/source candidates from `derive_project_mesh_source`.

This establishes equality under the editable-mesh snapshot digest contract,
not that the selected asset was historically created by that operation; the
operation identity is not a persisted operation log or an owner admission. The
editable snapshot digest does not establish every reconstructed half-edge ID.
The source-model volume is not independently compared to mesh-derived volume:
there is no measured source-versus-tessellation residual or certified error
budget. Project units are meters, so wrapper area is in m^2 and volume in m^3.
Physical construction, density, closure, compiled-mesh evidence and acceptance
remain with their existing owner. Reimport, durable lineage, live selection,
engineering native-source schema and display owner binding remain separate.

VANTA's owned `ReferenceKernel` implements box, cylinder and triangular
polyhedron operations, not this general polygon-profile extrusion; its current
`vanta_eng` target also pulls broader VANTA engine libraries into its link
graph. This slice reuses the existing Cartographer sweep calculation rather than
copying primitive algorithms or introducing a mandatory VANTA dependency. A
shared first-party kernel boundary and analytic-to-tessellated verification
remain open architectural work.
