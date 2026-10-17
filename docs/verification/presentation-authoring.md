# Authored camera and light presentation

Cartographer stores source presentation intent in a project-owned catalog.
Camera and light records bind to existing scene objects; analytic values and
review roles are authoring data, while preview and runtime behavior remain
derived or owned by the consuming application.

Camera records validate projection mode, field of view, clip planes,
bookmark identity, review purpose, and optional logical reference-image
identity. A project can have at most one active review camera. Light records
validate directional, point, spot, or area parameters and preserve descriptive
fixture, power, and switch ownership labels, color temperature, shadow intent,
normal state, and failure/flicker intent. Ownership labels are not permission
checks and do not actuate runtime switches.

The project catalog record is admitted through current schema v20 and
ProjectTransaction. A journal event is written before staged source state is
published. Dangling scene-object references, invalid source parameters, and
object deletion while referenced fail closed. Schema-v14 and earlier projects
migrate with an empty catalog; schema-v15 and v16 records remain intact during
upgrade, and schema-v15-and-later projects require an explicit catalog record.

The presentation catalog tests cover deterministic serialization, invalid
camera/light data, active-camera uniqueness, project save/reopen, schema-v14
migration, required-record enforcement, dangling-reference rejection,
delete-while-referenced rejection, and journal preservation after failed
mutations.

Not claimed: renderer/viewport preview, photometric calibration, resolved
reference-image bytes, camera capture, lighting simulation, or runtime
switch/failure authority.
