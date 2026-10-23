# Surface spectrum reference

Cartographer now has a bounded native CPU reference for the first step of the
cross-repository fidelity compiler: turning a revision-bound surface into a
deterministic graph spectrum.

## Contract

`build_surface_spectrum` accepts a validated `CompiledMesh`, a validated
`SurfaceDomain`, and one finite scalar sample per compiled vertex. It selects
the domain's source triangles, builds a vertex graph, and returns:

- sorted source `VertexId` provenance and lumped triangle-area masses;
- non-negative cotangent conductances on the selected triangle edges;
- a mass-normalized symmetric Laplacian;
- deterministic low-frequency eigenmodes;
- graph-Fourier coefficients for the supplied signal;
- total signal energy and graph smoothness energy;
- a canonical SHA-256 receipt bound to the domain source digest and revision.

The conductance clamp for negative cotangents is deliberate. It preserves a
positive-semidefinite graph operator on obtuse or locally irregular authored
surfaces, which is safer for a bounded authoring reference than emitting
unstable negative modes.

## Safety and evidence boundary

The implementation uses a deterministic dense Jacobi eigensolver and rejects
inputs above explicit vertex, mode, sweep, mass, and signal bounds. It does not
mutate the mesh, publish a representation, choose a runtime LOD, or claim GPU
throughput. The receipt is an analysis input for later candidate generation,
not an acceptance certificate by itself.

The output is source-revision and source-content bound. Duplicate source vertex
IDs, stale domains, non-finite signal values, degenerate-only domains, forged
digests, and mass-normalization/orthogonality failures are rejected.

## Design adaptation

The implementation ports the graph-Laplacian idea from the donor spectral
service, but keeps the public surface native and dependency-free. NumPy,
randomized fallback iteration, donor graph labels, and donor runtime services
are intentionally not part of Cartographer.

## Next bounded layer

The next layer should consume these modes to compare distributions of removed
surface information. It must retain the same source revision, domain identity,
and provenance boundary before a topology-aware simplifier or downstream consumer package
can use the result.
