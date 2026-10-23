# Contributing

Keep changes narrow and make the source of truth visible. A contribution that
changes authoring semantics must include:

1. the invariant and failure behavior;
2. a deterministic unit or regression test;
3. persistence impact and migration notes when applicable;
4. capability-matrix and roadmap updates when the user-visible boundary
   changes; and
5. provenance for any external code, fixture, SDK, or binary.

Run the headless configure, build, and CTest commands from the README before
opening a change. Do not add private repository dependencies, credentials,
network requirements, or silent approximation paths.

