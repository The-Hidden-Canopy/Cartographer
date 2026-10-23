# Site/parcel authoring

`condition::SiteParcelModel` is a headless, revisioned authoring graph for
parcel boundary source references, road frontage, access claims, qualitative
grade relations, and drainage intent. Stable author-supplied IDs are retained;
replacement edits require the exact model revision and apply atomically.

Each source reference binds an identity to one exact source revision. The
caller supplies the current source revision inventory explicitly. Inspection
reports missing and stale source bindings, missing frontage/access records,
broken frontage references, and unresolved grade or drainage relationships.
The model never upgrades an old reference to a newer revision or supplies a
fallback source.

All relationship values remain authored claims. The model does not inspect
boundary geometry, establish surveyed or legal parcel lines, establish
ownership or access rights, measure grade, or calculate/certify drainage. The
caller-supplied source inventory is not a live data feed. This foundation is
not connected to `.carto` persistence, a road-source command, or a native UI.

Acceptance evidence:

- `cartographer_site_parcel_tests` covers stable identity, complete source
  bindings, stale and missing source diagnostics, absent relationship
  diagnostics, malformed references/enums/endpoints, stale revision refusal,
  and atomic invalid-edit refusal.
- CTest: `cartographer_site_parcel_tests`,
  `cartographer_district_topology_tests`, and
  `cartographer_utility_network_tests` passed together on 2026-10-20.
