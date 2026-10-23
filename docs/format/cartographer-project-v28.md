# Cartographer project schema v28

Schema v28 adds durable receipts for field-driven CONDITION vegetation
placements. Each receipt binds one scene object and mesh asset to the stable
placement identity and scatter recipe, any legacy species-asset ID (zero for a
native-source-bound recipe), exact registered native plant source
generation/digest, enclosing project revision at preview time,
CONDITION authoring revision/digest, preview digest, all five required spatial
field receipts, and the resulting position, yaw, uniform scale, and suitability.

The project validator requires each receipt to match its scene object's mesh
and transform, rejects duplicate `(recipe, stable placement ID)` pairs, and
limits the receipt table. Save/reopen preserves the receipts. Undo removes the
batch and its receipts together; redo restores the exact object identities and
receipts. The `CONDITION_VEGETATION_PLACEMENT_RECEIPTS` table is mandatory in
schema v28, including when empty. Schemas v1–v27 remain readable through their
existing migration paths; they do not synthesize placement receipts.

Preview commit is a human-only, revision-checked operation. The application
recomputes the current preview and compares its project-revision-bound digest
before creating a bounded, atomic, undoable instance batch. A source must be an
explicitly selected Cartographer-native botanical mesh with the exact source
generation and digest recorded by the recipe. A stale preview, changed source,
wrong prototype, duplicate stable placement, invalid transform, or partial
batch failure is rejected without publishing partial placements.

The receipts retain no private runtime credentials, organization identity,
opaque job IDs, or external service endpoint. Schema v28 does not imply UV or
material fidelity, production vegetation rendering, collision evaluation, or
performance acceptance.
