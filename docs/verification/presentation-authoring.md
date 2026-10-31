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
camera. Unpinned placement fields are in world space. New camera-pinned
placements are camera-local (+X right, +Y up, -Z forward); pinning, unpinning,
or changing the pinned camera converts the placement through the current scene
transform to preserve its world pose. Schema-v2 camera-pinned records retain
their legacy world-space placement until explicitly re-pinned. The Scene Presentation panel can create
and rename view sets, change plane view modes, pin planes to authored cameras,
and edit placement, opacity, visibility, lock, and known-distance calibration.
These changes use one revision-bound catalog command and preserve each plane's
source binding. A known-distance calibration binds two in-image pixel anchors
to a physical distance and requires the authored width to equal the derived
scale. Locators reject absolute paths and traversal.

The source-state check is deliberately pure: the caller supplies the current
registry and an observed content digest. Missing registry/bytes remain
`missing`; changed registry metadata or bytes become `stale`; only exact
identity, path, generation, and digest match returns `verified`. Native import
inspects bounded image-container metadata, stages the exact bytes at the
project's content-addressed locator, and records the registry and plane through
human-reviewed project commands. The resolver reads only that project-local
blob and verifies its digest and dimensions; it does not use the workstation
path or substitute fallback bytes. The Scene Presentation panel can decode
TGA pixels through the bounded asset decoder and PNG/JPEG pixels through the
Windows Imaging Component from the verified blob. The panel preview is capped
at 64 x 64 samples. It can also explicitly display the cached decoded source
as a textured, opacity-modulated top-view X/Z overlay when the plane is visible
and has the canonical plan-aligned basis. In the perspective viewport, the
desktop projects the authored world-space quad for front, side, top, and free
perspective planes. Newly pinned camera-relative planes use the current world
transform of their exact authored scene object; the stored local basis follows
that object when it moves. Legacy schema-v2 camera pins keep world placement
until re-pinned. Camera scale is ignored. The plane origin is its geometric
center; width and pixel aspect determine the quad corners. The Vulkan desktop
uploads one digest-keyed texture at a time, independently caps uploads at
16,777,216 pixels, and releases the texture with the renderer. These are
screen-space UI overlays, not depth-tested 3D compositing. Native GPU display
and screenshot-backed visual acceptance remain unverified. The PNG/JPEG
decoder and perspective overlay changes were added on 2026-10-25 and were not
built or tested in this code sprint. Camera-pinned projection, pin/unpin pose
preservation, and schema-v2 world-placement migration regression cases are
authored but unrun; existing acceptance evidence predates them. Locked planes
reject source, calibration, and placement changes;
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

The nested presentation catalog is schema v3; schema-v1 catalogs migrate to
empty reference-plane collections, and schema-v2 camera pins retain world-space
placement until explicitly re-pinned. The presentation catalog and application
tests cover deterministic serialization, invalid camera/light/plane data,
complete view-set admission, calibration, missing/stale image sources,
active-camera uniqueness, project save/reopen, schema migration,
required-record enforcement, dangling-reference rejection, delete-while-
referenced rejection, stale project/source revision rejection, locked-placement
rejection, undo/redo, and journal preservation after failed mutations.

### PNG DEFLATE prefix admission (2026-10-26, code-only)

Before WIC pixel decode, the Windows reference-image decoder now reads the
bounded PNG chunk prefix, collects the first three bytes across contiguous IDAT
chunks, validates the PNG-compatible zlib header, and refuses a reserved
DEFLATE block type in the first block. WIC remains responsible for full pixel
decoding; this prefix check does not claim a standalone full-stream validator.
The existing Windows decoder regression includes a valid-CRC PNG with a
reserved first block type. No build or test was run at the user's request.

Risk: the new decoder code is uncompiled. Later-block deflate validity still
depends on WIC's decode behavior and remains unverified. No project schema,
serialized source, mutation, or cross-product contract changed. Rollback is
limited to the prefix validator and its call from the WIC decode path; the
existing digest, dimensions, and pixel bounds remain independent checks. The
read-only public/private audit passed for the current tree and selected refs
`HEAD` and `origin/main`; its separate optional ignore-policy probe was not
included in that pass.

Not claimed: non-Windows PNG/JPEG pixel decoding, depth-tested 3D reference
rendering, screenshot-backed visual acceptance,
camera/light preview, photometric calibration, camera capture, lighting
simulation, or runtime switch/failure authority.

### Perspective reference-image near-plane clipping (2026-10-27; unverified)

The perspective viewport now clips a reference-image quad against the same
bounded near plane used for scene geometry. The clipper carries UV coordinates
along each world-space edge, so a partially visible quad can be triangulated
with source coordinates preserved at the clipped boundary. A fully behind-camera plane
produces no overlay; non-finite corner/depth data fails closed. The plan-view
quad path is unchanged. The overlay remains a screen-space composition with no
scene-depth occlusion.

- **Files changed:** `apps/desktop/reference_image_projection.hpp`,
  `apps/desktop/main.cpp`, `tests/presentation_catalog_tests.cpp`, this ledger,
  and the ignored local queue.
- **Regression source authored:** one-corner near-plane clipping and UV
  interpolation, fully visible and fully clipped quads, non-finite depth, and
  invalid clip-plane refusal. **Run:** none, at the user's instruction.
- **Not verified:** desktop compilation, test execution, near-camera native
  display, and screenshot review.
- **Risk:** ImGui still draws the clipped polygon as a bounded textured overlay;
  it is not depth-tested and does not establish rendered-scene acceptance.
- **Rollback:** remove the clipper and perspective branch; retain the prior
  all-corners-visible quad path. No project schema or saved source changes.
- **Rejected:** drawing vertices behind the camera at the viewport center or
  claiming the UI overlay occludes/obeys scene depth.
- **Cross-product trace:** Cartographer desktop only. RegOS, IDA_Training,
  hidden_canopy_bot, and Ask_IDA contracts/datasets are unchanged; dataset and
  training progress are unchanged, capability work is code-only, deployment is
  unverified, and promotion remains open.
