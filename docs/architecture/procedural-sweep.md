# Procedural sweep source

`carto::geometry::SweepSource` is a serializable source recipe for building a
closed, convex-profile sweep from an exact `CurvePath` identity and revision.
The authored profile, sample density, endpoint taper, cap policy, and initial
reference-up vector remain available independently of the derived mesh.

An optional `inner_profile_scale` adds a hollow shell inside a convex profile
that strictly contains its origin. Its end caps are annular rims, leaving the
bore open. The parameter is an inner/outer radial scale, not a uniform-offset
guarantee for arbitrary profiles. Zero preserves the exact legacy solid source
encoding; hollow recipes use nested sweep source version 2. Both shells and
rim faces are included in preflight work budgets. See
[hollow sweep acceptance](../verification/hollow-sweep.md).

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

The Condition authoring layer can bind each named sweep endpoint (`start` or
`end`) to a generic `ConnectionPort`. `check_sweep_endpoint_compatibility`
requires exact sweep identity/revision and port identity, rejects a stale or
conflicting source recipe, requires an owner-bound sweep's endpoint port to
have the same scene-object owner, and delegates compatibility to the shared
namespaced-tag and required/offered-capability contract. Its deterministic
evidence includes digests for both sweep recipes and both ports. This check is
read-only: it does not spatially snap a port frame to the curve, persist a
connection graph, create a fitting, or alter the generated mesh.

`review_sweep_endpoint_spatial_fit` is a distinct read-only measurement path.
It resolves the exact curve endpoint and the selected port's same-owner named
frame chain through the current scene transform, then reports position error
and outward-axis dot product. The caller supplies the port-local axis and both
tolerances. Its canonical digest binds those choices, the source/path/port and
frame-chain digests, plus project and scene revisions. ApplicationSession,
UiController, and the desktop sweep panel expose the review against a current
project snapshot. A passing measurement does not move the source or port,
create an edge, create fitting geometry, or establish physical compliance.

This is a generic solid/hollow sweep primitive, not the complete procedural
geometry kit. Arbitrary profile editing, material regions, UV transfer,
implicit surfaces, automatic endpoint snapping, conduit/fitting geometry,
supports, sag, wall penetrations, branching, committed derived-mesh placement,
and production donor-family parity remain unimplemented. Spatial fit is a
caller-policy measurement, not a connection or flow contract.
