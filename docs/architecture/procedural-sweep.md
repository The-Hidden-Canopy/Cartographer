# Procedural sweep source

`carto::geometry::SweepSource` is a serializable source recipe for building a
closed, convex-profile sweep from an exact `CurvePath` identity and revision.
The authored profile, sample density, endpoint taper, cap policy, and initial
reference-up vector remain available independently of the derived mesh.

The reference-up vector defines an explicit local frame; the implementation
does not assume a Z-up or Y-up donor convention. Subsequent frames use minimal
parallel transport. Generated vertex and face identities come from
`EditableMesh`, and `SweptMeshProduct` preserves the source and centerline
identities/revisions consumed by the build.

Admission is bounded by profile, station, serialized-size, and output-element
limits. Profiles must be strictly convex, counter-clockwise, and non-self-
intersecting. A centerline must match the source identity and revision, have
nondegenerate tangents, and be tangent-continuous at joined cubic segments.
Ambiguous reversals, frame singularities, stale paths, and invalid profiles
fail closed rather than picking an arbitrary orientation or regenerating from
stale input.

`ProjectDocument` now persists source recipes and independent source lineage
in schema 22. `ProjectTransaction` publishes create/update/delete operations
through the normal journal boundary. A source cannot be created against a
missing or stale path, and project validation refuses publication if a path
edit would stale an active sweep binding. The path cannot be deleted while an
active sweep references it.

The native Condition authoring instrument can create and edit regular radial
profiles, bind them to the selected centerline revision, and build a derived
wireframe preview. Existing custom profiles remain intact and previewable, but
the compact radial editor refuses to flatten them into a different profile.
Viewport drawing is capped at 16,000 edges per frame; the complete generated
mesh remains bounded by the geometry contract.

This is the first generic solid sweep primitive, not the complete procedural
geometry kit. Arbitrary profile editing, material regions, UV transfer,
implicit surfaces, committed derived-mesh placement, and production donor-family
parity remain unimplemented.
