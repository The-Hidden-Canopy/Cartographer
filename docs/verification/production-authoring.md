# Sculpt and garment authoring verification

`cartographer_production_authoring_tests` covers the public production layer:

- SculptStack serialization and canonical digest round-trip;
- bounded surface samples, mask/settings/displacement references, symmetry,
  parent-cycle rejection, hostile trailing/future input, and stale publication;
- fabric intent, panel outline/rest-shape identity, seam edge bounds, body-bound
  pins, constraint validation, and representation-tier uniqueness;
- GarmentAsset serialization and canonical digest round-trip;
- DrapeRequest and DrapeReceipt source/garment/request/iteration binding;
- rejection of a receipt produced for a changed garment;
- a negative-space check that public garment records do not carry runtime pose,
  handles, or telemetry.

The receipt test is a CPU/reference contract test. It does not claim that the
XPBD solver, native cloth backend, VANTA runtime lowering, GPU execution, or a
presented cloth frame exists in the public checkout.
