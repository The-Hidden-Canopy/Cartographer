# Biological authoring verification

The `cartographer_biological_authoring_tests` target is the acceptance slice
for `carto_biological`.

It covers:

1. deterministic source serialization and canonical digest stability;
2. round-trip preservation of structures, fields, attachments, layers, stages,
   kinematic graph correspondence, and representation variants;
3. hostile trailing input and future schema rejection;
4. cyclic structure rejection and reversed/non-finite field-bound rejection;
5. duplicate representation-tier rejection;
6. prepared-publication receipt validation and stale source/digest rejection;
7. reimport continuity with a changed product digest but preserved authored
   identity and continuity token;
8. same-revision source replacement rejection;
9. a negative-space check that the public serialized record contains no runtime
   pose, game health, or telemetry state.

The test is a CPU/reference contract test. It does not claim VANTA execution,
GPU timing, native biological simulation, medical correctness, or a presented
frame. Those are separate private/runtime evidence gates.
