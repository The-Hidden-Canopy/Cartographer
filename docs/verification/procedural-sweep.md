# Procedural sweep verification

The `cartographer_procedural_sweep_tests` target covers canonical source
serialization, deterministic stable-ID mesh generation, exact centerline
identity/revision binding, invalid/future/trailing input, explicit frame
singularity, convexity and self-intersection rejection, tangent-discontinuous
joints, and endpoint taper.

Synthetic geometry fixtures exercise a pipe, a four-part railing, and a tapered
branch using Cartographer-native `SweepSource` records. They are algorithm
fixtures, not production CONDITION model-library imports and not a donor-parity
or visual-acceptance claim.

The native Condition instrument creates and edits regular radial-profile
recipes against an explicitly selected centerline. Custom profiles and frames
are preserved for preview but are not flattened into the radial editor. Its
derived wireframe is ephemeral, source-revision checked, and capped at 16,000
edges per viewport frame. Application tests cover source mutation, stale
commands, undo/redo with non-reused revisions, and save/reopen. This does not
yet constitute desktop pointer/capture acceptance.

Still required before this can count as a completed CONDITION procedural-kit
promotion:

- support material-region semantics and attribute/UV transfer;
- rebuild three reviewed CONDITION generator families from Cartographer
  sources and compare dimensions/topology/visual output against approved
  acceptance evidence;
- decide and implement a governed path for committing a derived mesh, if the
  product requires placed geometry rather than a source recipe plus preview;
- add desktop-level save/reopen and pointer/capture acceptance in an interactive
  environment; and
- complete visual/native fidelity acceptance for the three reviewed families.

The focused procedural-sweep and project-persistence tests pass, along with
adjacent curve, road, CONDITION authoring/UI-fixture, application-history, and
donor-ledger tests. The desktop executable compiles. GPU execution, desktop
pointer interaction, and visual-acceptance claims are not made.
