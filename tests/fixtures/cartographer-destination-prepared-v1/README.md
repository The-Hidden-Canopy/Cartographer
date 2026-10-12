# Prepared-scene compatibility fixture

This is a small producer-authored Cartographer prepared publication used to
test the public handoff boundary. It contains one square render mesh, two
material regions, one serialized material catalog, and one scene node. The
fixture is checked by `cartographer_prepared_publication_compatibility_tests`
using Cartographer's own publication, render-mesh, and material-catalog
readers.

A private destination runtime copies the same fixture into its compatibility
tests. Its independent adapter decodes the public render-mesh bytes and then
admits only the verified, explicitly mapped realization. The fixture carries
no runtime handles, private source paths, telemetry, or proprietary worker
implementation.
