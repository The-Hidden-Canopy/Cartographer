# Authored presentation sources

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

Reference-image planes are also revision-bound source records. Each stores a
project-relative locator, exact source-asset identity/generation and SHA-256,
inspected image dimensions, placement basis, world width, opacity, visibility,
and lock state. Front/side/top views form complete named sets; perspective
planes may stand alone, while camera-pinned planes must name an authored
camera. The Scene Presentation panel can create and rename view sets, change
plane view modes, pin planes to authored cameras, and edit placement, opacity,
visibility, lock, and known-distance calibration. These changes use one
revision-bound catalog command and preserve each plane's source binding. A
known-distance calibration binds two in-image pixel anchors to a physical
distance and requires the authored world width to equal the derived scale.
Locators reject absolute paths and traversal.

The source-state check is deliberately pure: the caller supplies the current
registry and an observed content digest. Missing registry/bytes remain
`missing`; changed registry metadata or bytes become `stale`; only exact
identity, path, generation, and digest match returns `verified`. Native import
inspects bounded image-container metadata, stages the exact bytes at the
project's content-addressed locator, and records the registry and plane through
human-reviewed project commands. The resolver reads only that project-local
blob and verifies its digest and dimensions; it does not use the workstation
path or substitute fallback bytes. The Scene Presentation panel can decode
TGA pixels from the verified blob for a panel preview capped at 64 x 64
samples. It can also explicitly display the cached decoded source as a
textured, opacity-modulated X/Z overlay when the plane is visible, uses the top
view, and has the canonical plan-aligned basis. The plane origin is its
geometric center; width and pixel aspect determine the quad corners. The
Vulkan desktop uploads one digest-keyed texture at a time, independently caps
uploads at 16,777,216 pixels, and releases the texture with the renderer. This
is a plan-view overlay only: front, side, perspective, and
camera-pinned modes are not rendered, and depth-tested 3D compositing is not
implemented. Native GPU display and screenshot-backed visual acceptance remain
unverified. Locked planes reject source, calibration, and placement changes;
visibility and opacity remain available as review controls. Every accepted
source or display edit advances the plane revision exactly once.

The project catalog record is admitted through project schema 26 and
ProjectTransaction. A journal event is written before staged source state is
published. Dangling scene-object references, invalid source parameters, and
object deletion while referenced fail closed. Schema-v14 and earlier projects
migrate with an empty catalog; schema-v15 and v16 records remain intact during
upgrade, and schema-v15-and-later projects require an explicit catalog record.

The native Inspector exposes project-bound camera and analytic light source
authoring for the selected scene object. Edits remain in a local draft until
explicit Apply; a project-generation or revision change blocks Apply and
requires a reload, so a same-revision project replacement cannot inherit a
stale draft.
Camera/light source revisions advance only when their authored fields change,
and activating another review camera deactivates the prior one in the same
catalog command. The application command supports undo/redo and is covered
through save/reopen and durable journal receipts.

The nested presentation catalog is schema v2 and reads schema-v1 catalogs as
empty reference-plane collections. The presentation catalog and application
tests cover deterministic serialization, invalid camera/light/plane data,
complete view-set admission, calibration, missing/stale image sources,
active-camera uniqueness, project save/reopen, schema migration,
required-record enforcement, dangling-reference rejection, delete-while-
referenced rejection, stale project/source revision rejection, locked-placement
rejection, undo/redo, and journal preservation after failed mutations.

Not claimed: non-TGA pixel decoding, front/side/perspective/camera-pinned or
depth-tested 3D reference rendering, screenshot-backed visual acceptance,
camera/light preview, photometric calibration, camera capture, lighting
simulation, or runtime switch/failure authority.
