# Prepared-scene compatibility fixture

This is a small producer-authored Cartographer prepared publication used to
test the public handoff boundary. It contains one square render mesh, two
material regions, one serialized material catalog with a mesh-scoped roughness
variant, and one scene node. Its
declared centimeter units, right-handed Z-up/Y-forward frame, and nonzero node
translation exercise consumer coordinate conversion. The fixture is checked
by `cartographer_prepared_publication_compatibility_tests`
using Cartographer's own publication, render-mesh, and material-catalog
readers.

Independent consumers can use the fixture in their own conformance tests and
admit only explicitly mapped realizations. The fixture carries no runtime
handles, machine-specific paths, telemetry, or implementation-specific data.
