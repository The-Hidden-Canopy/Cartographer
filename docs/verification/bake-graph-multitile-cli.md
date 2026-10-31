# Multi-tile CPU normal-map CLI workflow

## Capability and boundary

The CLI now exposes `bake-normal-tiles` and `inspect-bake-normal-tiles` over
the existing deterministic CPU tile scheduler. Baking discovers target UV tiles
in stable U,V order, rejects faces that cross tile boundaries, compiles every
tile before creating output, and publishes the tile artifacts and receipt
ledger inside one new staging directory. The output directory appears only
after every tile, artifact read-back, receipt, and manifest read-back succeeds.
An existing output path is never overwritten. A failed publication may leave
its digest-named `.pending-*` directory for inspection; it is not presented as
the final output.

`bake-normal-tiles` accepts an optional trailing cache directory. Each tile is
stored as `<dependency-digest>.carto-bake-tile-cache`; the CLI reads only the
expected digest paths, bounds each file by the scheduled tile size, and checks
the serialized entry and current source/target dependencies before reuse.
Symlinked, non-regular, malformed, or mismatched entries fail closed. Newly
compiled tiles are written after artifact/receipt verification and final
project-source revalidation, then read back before output publication. Cache
entries are not deliberately overwritten: a concurrent or pre-existing entry
is accepted only when it safely rebinds to the same deterministic tile output.
A cache directory is created only when there are compiled tiles to store. A
later publication error can leave valid derived cache entries even when the
final output directory was not published.

The scheduler also preflights the full RGBA8 result against a 512 MiB aggregate
budget before allocating tile artifacts. The separate 100-tile count cap still
applies; whichever limit is reached first refuses the complete schedule.
Decoded cache inputs share that memory budget with scheduled outputs, including
space reserved for bounded cache deserialization. When not all valid cache
entries fit that headroom, the CLI reports `cache_skipped_memory_tiles` and
compiles those tiles instead.

Each admitted tile receipt binds the exact per-tile recipe, source/target
identity and digest, artifact bytes, and measured compile duration. The
CLI compiles every tile when no cache directory is supplied. With a cache
directory, `compiled_tiles` and `reused_tiles` report the scheduler result.
Cache reuse reports zero compile time for a reused entry; that value does not
claim that lookup overhead is zero. Cache paths bind source identity/revision,
geometry, target tile UV inputs, bake settings, and normal-map space. Changed
dependencies select a different path and compile again; stale entries remain
immutable derived data and are not selected.

The inspector validates every receipt and content-addressed blob, current
source/target bindings, canonical recipe identity, tile order, and exact
coverage of the target's currently discovered tile set. Missing, duplicated,
reordered, mixed-recipe, stale, or forged-profile artifacts fail closed. The
source project is read-only throughout.

## Authored coverage

The existing `cartographer_bake_tile_incremental_tests` target now checks that
compiled schedule entries carry finite nonnegative compile durations and cache
reuses carry zero compile duration. It also covers aggregate output-budget
refusal before compilation. The existing CLI fixture can create a
two-tile target and a cross-tile face. The normal-bake CLI script now covers
two-tile bake/inspect, source-project byte preservation, cold and warm cache
behavior, cache/output overlap refusal, corrupt cache refusal, recompiling after
source mutation, forged-profile rejection, stale-source refusal, and cross-tile
failure without final output. A symlinked expected cache entry is also refused
when the host permits creating the fixture; unsupported symlink creation is an
explicit test skip.

These cases were authored but not built or run under the active code-only
instruction. No test, build, lint, or native result is claimed.

## Scope limits and follow-up

The original increment closed only the tangent-space CPU normal-map CLI route.
Later code-only continuations in this ledger add CPU CLI routes for AO,
bent-normal, curvature, thickness, height-displacement, world-position, and
material/surface-ID maps. Desktop CPU tile workflows and background
cancellation are wired; tangent-normal CPU tiles now have opt-in cache reuse.
D3D12 multi-tile paths cover AO, bent-normal, and tangent-normal only. Cache
reuse for the other map types, downstream consumer admission, and compiled
acceptance remain open. Cross-tile faces remain unsupported. Full multi-tile
texture-bake acceptance still requires focused build/tests and source-backed
artifact inspection on a retained fixture.

No project, journal, or network schema changes. Cartographer's C++ scheduler
result has an additive per-entry timing field. The RegOS transition, audit,
permission, and organization-scope contracts are untouched. IDA_Training
datasets/evaluations, hidden_canopy_bot APIs, and Ask_IDA query contracts are
untouched. Dataset and training progress do not advance; this is code-only
capability progress, with no promotion or deployment.

Rollback is limited to the schedule timing field and capture, CLI commands,
fixture and script additions, and this documentation plus the work-queue/matrix
notes. Preserve the other dirty workspace changes. No automated or synthetic
readiness evidence was promoted.

## Code-only CPU multi-tile bake continuations (2026-10-26)

Added `schedule_ambient_occlusion_tiles_cpu` as a library path over the
existing stable UDIM enumerator. It creates a tile-specific AO recipe for each
complete target face tile, compiles in stable U,V order, and returns no partial
schedule when any tile fails. The scheduler preflights aggregate RGBA8 output
and ray counts against existing bounded limits and retains per-tile and total
compile durations plus aggregate coverage/ray counters. It refuses
cross-boundary faces through the shared enumerator. The new CPU CLI bakes the
full schedule before creating a staging directory, publishes each tile with a
receipt, reads every artifact and the manifest back, then atomically publishes
the directory. Its inspector requires an exact match between the current
target tile set and admitted receipts and checks each payload digest, recipe,
receipt key, and current source binding. Output/staging existence and inspected
manifest/blob paths use non-following status checks, so dangling output links,
symlinked directories, manifests, and payloads are refused. D3D12 multi-tile
execution and the bent-normal path are unchanged.

- **Files changed:** `libs/production/include/carto/production/bake_graph.hpp`,
  `libs/production/src/bake_graph.cpp`, `apps/cli/main.cpp`, this ledger, and
  the ignored local work queue.
- **Capability gained:** callers can compile an all-or-nothing CPU AO result
  across the target mesh's bounded UDIM set, and CLI users can publish and
  inspect the complete set against the current saved project.
- **Boundary improved:** tile order is deterministic; aggregate image and ray
  work are bounded; invalid recipes, stale sources, cross-tile faces, and any
  failed tile return an error without exposing earlier tile artifacts. CLI
  publication refuses overwrite and exposes no final directory before
  artifact/receipt read-back succeeds. Inspection refuses missing, duplicate,
  stale, corrupt, symlinked, and extra receipts or payloads.
- **Governance preserved:** source and target bindings remain revision/digest
  checked; library scheduling is read-only. CLI publication writes typed
  receipts only under a new output directory and does not mutate the project.
- **Tests:** not added or run under the code-only sprint instruction. The
  required follow-up is a two-tile success case plus aggregate-budget,
  cross-tile, stale-source, corrupt/missing/extra receipt, late-tile
  failure/no-partial-result, atomic output, and source-project preservation
  coverage, then focused build and execution.
- **Risks:** the public API and aggregate budget path are uncompiled and
  unexercised. CLI publication and inspection also have no runtime evidence.
  Non-following filesystem checks have not been exercised on Windows or other
  supported hosts. Timing and artifact results are not acceptance evidence.
- **Rollback:** remove the AO tile schedule types/API and implementation, the
  two CLI routes, and their recipe helpers.
- **Rejected:** routing a tile set through the single-tile D3D12 worker or
  returning partial artifacts after a later tile fails.
- **Cross-product trace:** direct impact is Cartographer's CPU production bake
  library. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts are
  unchanged; dataset, training, deployment, and promotion progress are
  unchanged.

### Bent-normal CPU tile scheduler and CLI continuation

Added `schedule_bent_normal_tiles_cpu`, using the same stable tile enumeration,
tile-specific recipe binding, output-byte and aggregate-ray budgets, and
all-or-nothing result semantics as the AO scheduler. It retains per-tile
artifacts and compile timings plus aggregate coverage and ray counts. The
single-tile compilation path remains the existing shared deterministic CPU
AO/bent-normal reference implementation. The CLI now exposes
`bake-bent-normal-tiles` and `inspect-bake-bent-normal-tiles`: baking stages
all tiles in a new output directory, publishes typed artifact receipts, reads
payloads and the manifest back, revalidates the project source, then renames
the staged directory. Inspection requires exact current tile/receipt coverage,
admitted receipts, digest-valid blobs, matching recipes, and current source
bindings. It still has no incremental cache, desktop, or D3D12 multi-tile path.

- **Files changed:** `libs/production/include/carto/production/bake_graph.hpp`,
  `libs/production/src/bake_graph.cpp`, `apps/cli/main.cpp`, this ledger, and
  the ignored local work queue.
- **Capability gained:** users can compile, publish, and inspect an
  all-or-nothing CPU bent-normal schedule for a bounded target UDIM set.
- **Boundary improved:** cross-tile faces, stale source bindings, late compile
  failure, aggregate ray excess, aggregate output excess, mismatched or extra
  receipts, and symlinked inspection entries fail closed without exposing an
  incomplete final tile directory.
- **Governance preserved:** project data is read-only; source and target
  bindings remain revision/digest checked; publication requires typed
  production receipts and read-back before the final rename.
- **Tests:** not added or run under the code-only sprint instruction. Follow-up
  coverage needs two-tile order/output, cross-tile refusal, source staleness,
  budget boundaries, late-failure atomicity, receipt/payload corruption and
  symlink refusal, followed by build and CPU artifact inspection.
- **Risks:** API and CLI are uncompiled and unexercised; staging directories
  from failed publication remain for manual inspection; cache, desktop, and
  D3D12 multi-tile acceptance remain open.
- **Rollback:** remove the bent-normal CLI bake/inspect routes and command
  dispatch, then remove the bent-normal schedule structs, declaration, and
  implementation.
- **Rejected:** returning a prefix of artifacts after later tile failure or
  reporting CPU-reference output as GPU/native acceptance.
- **Cross-product trace:** direct impact is Cartographer's CPU production bake
  library and CLI. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts
  remain unchanged; dataset, training, deployment, and promotion progress are
  unchanged.

### Curvature, thickness, and height-displacement CPU tile schedulers

Added a shared bounded scheduler implementation for `CurvatureRecipe`,
`ThicknessRecipe`, and `HeightDisplacementRecipe`. Each function validates the
base recipe and source bindings, enumerates complete target UDIM tiles in the
existing stable order, derives a per-tile recipe, enforces the aggregate
RGBA8 output limit before compiling, and returns no partial schedule after a
tile failure. Results retain each typed artifact and compile duration plus
aggregate coverage and timing. The CLI now exposes `bake-curvature-tiles`,
`bake-thickness-tiles`, and `bake-height-displacement-tiles` with matching
inspect commands. Shared publication stages the typed receipts and artifacts,
reads every artifact and the manifest back, revalidates both project meshes,
and publishes only after all tiles succeed. Inspection binds the request to
the current project and requires exact tile/receipt coverage and digest-valid
payloads. Incremental cache reuse, desktop controls, and D3D12 execution remain
open.

- **Files changed:** `libs/production/include/carto/production/bake_graph.hpp`,
  `libs/production/src/bake_graph.cpp`, `apps/cli/main.cpp`, this ledger, and
  the ignored local work queue.
- **Capability gained:** callers can compile bounded all-or-nothing CPU tile
  schedules, and CLI users can publish and inspect the three scalar projection
  maps against current project source bindings.
- **Boundary improved:** stale bindings, cross-tile faces, aggregate output
  excess, invalid per-tile recipes, and late compiler failures fail without a
  partial schedule.
- **Governance preserved:** schedule generation does not mutate project state;
  CLI receipts are written only into a new output directory, and each typed
  publisher retains source, recipe, and admission validation.
- **Tests:** none added or run under the no-test sprint instruction. Follow-up
  coverage needs two-tile order and output, aggregate budget boundaries,
  stale-source refusal, cross-tile refusal, and late-tile failure atomicity.
- **Risks:** APIs and CLI routes are uncompiled and unexercised; users still
  have no desktop, cache, or D3D12 multi-tile path for these maps.
- **Rollback:** remove the three CLI routes and shared publication/inspection
  helpers, then remove the generic schedule result aliases and declarations,
  shared scheduler helper, and three typed scheduler wrappers.
- **Rejected:** returning a prefix of compiled map artifacts when a later
  tile fails.
- **Cross-product trace:** Cartographer production library only. RegOS,
  IDA_Training, hidden_canopy_bot, and Ask_IDA contracts and deployment state
  remain unchanged; dataset, training, and promotion progress do not change.

### World-position and material/surface-ID CPU tile routes (2026-10-26)

Added whole-project-bound CPU tile schedulers and CLI bake/inspect routes for
world-position and material/surface-ID maps. World-position recipes bind a
scene node, UV set, and exact serialized project snapshot. Material/surface-ID
baking continues to require caller-authored domain membership; the CLI does
not infer domain meaning from geometry or material-region values.

Added `bind-material-surface-id-recipe` to make that recipe workflow usable
from the CLI. Its bounded, non-symlink TSV input starts with
`carto-surface-domain-assignments-v1`; each following nonblank row contains
`domain_id<TAB>semantic_class<TAB>material_class<TAB>face_id[,face_id...]`.
The author supplies all IDs, class labels, and face membership. Class labels
are bounded to 256 bytes and reject tab, newline, NUL, and canonical semicolon
delimiters. The command recomputes domain digests from the current compiled
target mesh, then binds the current project snapshot, mesh revision/digest, UV
set, material-region layer, and output dimensions. Existing project validation
still requires domains to cover every mesh face exactly once and material
regions to have durable assignments. Recipe output refuses existing paths and
must deserialize identically after publication read-back.

- **Files changed:** `libs/production/include/carto/production/bake_graph.hpp`,
  `libs/production/src/bake_graph.cpp`, `apps/cli/main.cpp`, this ledger, and
  the ignored local work queue.
- **Capability gained:** the CPU CLI can schedule, publish, and inspect
  complete world-position and material/surface-ID tile sets. Authors can bind
  material/surface-ID recipes to the current project using explicit face-domain
  assignments rather than manually constructing source digests.
- **Boundary improved:** aggregate serialized output is preflighted before map
  compilation; stale project snapshots, incomplete/overlapping domain
  membership, malformed assignment rows, non-regular inputs, output overwrite,
  mismatched recipes, and corrupt receipt-backed payloads refuse. Tile
  scheduling returns no partial schedule after failure.
- **Governance preserved:** all source bindings remain revision and digest
  checked; surface-domain semantics and face membership remain human-authored;
  CLI writes derived receipts and recipe files only to new paths and does not
  mutate the project or its journal.
- **Tests:** none added or run under the code-only sprint instruction. Follow-up
  coverage should include two-tile determinism, exact snapshot staleness,
  malformed and over-budget domain files, duplicate/omitted/overlapping faces,
  durable material assignment refusal, no-overwrite/read-back behavior,
  corrupt/missing/extra receipts and blobs, and late-tile failure atomicity.
- **Risks:** the schedulers, recipe binder, CLI argument dispatch, filesystem
  behavior, and receipt inspectors are uncompiled and unexercised. The
  recipe-file format is additive CLI surface without runtime compatibility
  evidence. Desktop authoring, incremental cache reuse for these maps, D3D12
  multi-tile execution, and native acceptance remain open.
- **Rollback:** remove the two document-bound schedulers and result aliases,
  the world-position and material/surface-ID CLI bake/inspect routes, the
  recipe binder command, and this ledger/queue continuation.
- **Rejected:** deriving surface-domain membership from geometry or durable
  material regions, returning partial tiles, or treating CPU-reference output
  as accelerated/native evidence.
- **Cross-product trace:** direct impact is Cartographer's production library
  and CLI. No RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA API, schema,
  dataset, evaluation, or deployment contract changed.

Dataset progress: none. Training/prompt progress: none. Capability progress:
code-only CPU map and recipe-authoring wiring. Promotion progress: none; no
build, test, native, accelerated, real-asset parity, or visual evidence was
produced.

### Desktop saved-UV CPU tile workflows (2026-10-26, code-only)

The saved-UV inspector now exposes CPU tile bakes for tangent-normal, ambient-
occlusion, bent-normal, curvature, thickness, height-displacement,
world-position, and material/surface-ID maps. Width and height are explicit.
AO and bent-normal also expose sample count, maximum ray distance, and
deterministic seed. Scalar maps expose projection extent, normalization settings,
and explicit rejection policies for projection misses, thickness misses, and
height clamping. The user chooses a new output path with
the `.carto-bake` directory suffix. Each bake runs in a background task, uses
the selected saved mesh/scene object and verifies its captured mesh revision
and UV digest. World-position and material/surface-ID also verify their exact
project snapshot or serialized recipe bindings. Each map uses its own typed
scheduler, cook-key, publish, and read path while sharing bounded staging,
applicable aggregate ray/output limits, receipt and artifact read-back, final
source revalidation, and staged directory rename. The result message identifies
the map and labels the output as CPU-reference evidence.
World-position binds the inspected scene object and exact saved-project digest,
revision, mesh, UV set, and world transform.
Material/surface-ID reads a caller-selected, serialized recipe containing
explicit domain membership and rejects stale project or mesh bindings; the
desktop never infers semantic domains.

- **Files changed:** `apps/desktop/bake_workflow.hpp`,
  `apps/desktop/main.cpp`, `apps/desktop/CMakeLists.txt`,
  `libs/production/include/carto/production/bake_graph.hpp`,
  `libs/production/src/bake_graph.cpp`,
  `tests/bake_tile_incremental_tests.cpp`, this ledger, and the ignored local
  work queue.
- **Capability gained:** users can start and monitor saved-project tangent-
  normal, AO, bent-normal, curvature, thickness, height-displacement,
  world-position, and material/surface-ID UDIM bakes from the native UV
  inspector without blocking the UI thread, cancel between tiles, and request
  cancellation during desktop shutdown. Each output is a receipt-backed
  directory published only after tile and manifest read-back succeeds.
- **Boundary improved:** stale or mismatched inspected mesh/UV inputs, invalid
  parameters, policy-rejected misses/clamps, unsupported/cross-tile coverage,
  empty/over-budget schedules, output overwrite, late tile failures, source
  namespace drift, and mesh source drift before publication produce no final
  output directory. The job reads a saved project and does not mutate project
  or journal state.
- **Governance preserved:** all recipe and source checks stay in the existing
  production scheduler and artifact-store paths. The desktop action grants no
  project mutation authority and reports CPU-reference evidence only.
- **Tests:** added an unrun pre-cancelled normal-map scheduler regression.
  Existing happy-path scheduler coverage remains in place. Follow-up cases
  should cover each typed dispatch and recipe identity, exact world-position
  project snapshot and transform binding, material/surface-ID recipe and domain
  staleness, dispatch staleness after the path dialog, worker success and failure,
  empty/cross-tile coverage,
  aggregate bounds, each miss/clamp policy, exact output overwrite refusal,
  late failure without final directory, receipt/blob corruption, source drift
  during work, cancellation between tiles and before final publication, and app
  shutdown while the worker is active. Then build the Windows desktop and
  exercise the native controls. No tests or builds were run at the user's
  request.
- **Risks:** this new header and UI wiring are uncompiled and unexercised.
  Failed publication can leave a `.pending-*` staging directory for inspection.
  Cancellation is checked between tiles and before final publication; an active
  tile must finish before it responds. Shutdown requests cancellation before the
  asynchronous future is destroyed, which can still wait for the active tile.
  Material/surface-ID domain authoring still uses the existing CLI recipe binder.
  Tangent-normal CPU cache reuse is now wired separately; other desktop map
  families still have no incremental cache. Native interaction evidence and
  CPU/accelerated parity remain open.
- **Rollback:** remove `bake_workflow.hpp` from the desktop source list and
  delete the workflow helper, CPU tile task state and cancellation source,
  saved-UV controls, stop-token scheduler overloads, and file-dialog option.
- **Rejected:** running the bounded bake synchronously on the UI thread,
  silently rebinding a stale inspection to current geometry, inferring
  material/surface domain membership, overwriting an existing output path, or
  labeling CPU output as GPU evidence.
- **Cross-product trace:** direct impact is Cartographer desktop and production
  bake code. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts,
  datasets, evaluations, and deployment state are unchanged.

Dataset progress: none. Training/prompt progress: none. Capability progress:
code-only desktop CPU tile workflow for eight map families with tile-boundary
cancellation. Promotion progress: none; compilation, tests, native interaction,
accelerated parity, and real-asset acceptance remain open.

### Desktop tangent-normal cache integration (2026-10-26, code-only)

Added an opt-in cache toggle to the desktop saved-UV bake panel. CPU
tangent-normal tiles use immutable content-addressed entries under the selected
output's parent directory; the path is shown in the control text. D3D12 and
other map families bypass this cache. Cache lookup rejects symlinked or
non-regular entries, oversized or malformed data, dependency-key mismatch, and
output/cache overlap. Decoded entries share the scheduler's 512 MiB RGBA8
budget; tiles that cannot fit cache headroom are compiled normally. Before
cache publication, the desktop path reloads the project and revalidates its
namespace, source/target identities, mesh revision, and content digest. New
entries are written atomically without intentional overwrite and read back.
The result reports compiled and reused tile counts.

- **Files changed:** `apps/desktop/bake_workflow.hpp`,
  `apps/desktop/main.cpp`, `tests/bake_tile_incremental_tests.cpp`, this
  ledger, and the ignored local work queue.
- **Capability gained:** desktop CPU tangent-normal tile bakes can reuse
  verified per-tile artifacts across runs when their exact source, tile UV,
  settings, and normal-space dependencies match.
- **Boundary improved:** cache/output overlap, symlink and non-file entries,
  size excess, malformed serialization, changed dependencies, source drift,
  and corrupt cache bytes refuse publication. Memory headroom prevents decoded
  cache artifacts from exceeding the shared schedule budget.
- **Governance preserved:** the cache is opt-in, project and journal state stay
  read-only, and typed receipts plus artifact and manifest read-back remain in
  the existing publication path. D3D12 output is not labeled as CPU cache reuse.
- **Tests:** added source coverage for cache-entry write/read, missing and
  undersized entries, overwrite refusal, corrupt-entry refusal, and symlink
  refusal when the host permits creating the fixture. **Run:** none; the user
  requested no testing. Build, native interaction, scheduler integration,
  symlink race behavior, and warm/cold desktop bake behavior remain unverified.
- **Risks:** source is uncompiled; cache entries can remain as valid derived
  data if later output publication fails, and the persistent cache can grow
  until the user clears its directory. Concurrent filesystem replacement
  races are limited by path rechecks but have not been exercised. Cache reuse
  remains limited to CPU tangent-normal tiles.
- **Rollback:** remove the normal-tile cache helpers and optional cache
  argument from `bake_workflow.hpp`, the desktop checkbox/result counts, and
  the new test case.
- **Rejected:** silently enabling a persistent cache, using CPU entries for
  D3D12 output, overwriting content-addressed entries, treating corruption as
  a cache miss, or bypassing final receipt/read-back publication.
- **Cross-product trace:** direct impact is Cartographer desktop and its
  production bake cache. RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA
  contracts, datasets, evaluations, and deployment state are unchanged.

Dataset progress: none. Training/prompt progress: none. Capability progress:
code-only opt-in desktop tangent-normal tile cache with bounded reuse and
compiled/reused reporting. Promotion progress: none; no tests, build, native
interaction, GPU parity, or real-asset acceptance ran.

### D3D12 multi-tile scheduler source continuation (2026-10-26, code-only)

Added bounded `schedule_tangent_normal_map_tiles_d3d12` and
`schedule_ambient_occlusion_tiles_d3d12` APIs. They use the shared stable U,V
enumerator, derive tile-bound recipes, preflight aggregate output and AO-ray
bounds, execute each tile through its existing D3D12 worker plus canonical CPU
artifact parity check, retain per-tile provenance and timing, and return no
partial schedule when any tile fails. Both schedulers revalidate source and
target bindings after the complete schedule. Cross-tile faces remain refused.

- **Files changed:** `libs/d3d12/include/carto/d3d12/bake_graph.hpp`,
  `libs/d3d12/src/bake_graph.cpp`, `tests/d3d12_tests.cpp`, this ledger, and the
  ignored local work queue.
- **Capability gained:** callers can execute D3D12 normal-map and AO baking for
  every complete target UDIM tile in a bounded schedule.
- **Boundary improved:** the full schedule is budgeted before GPU work, tiles
  run in deterministic U,V order, worker provenance and per-tile CPU parity are
  retained, and invalid/cross-tile inputs or a failed tile return no schedule.
- **Governance preserved:** no project mutation or new serialized schema was
  introduced. Each tile still passes through the existing D3D12 worker evidence
  checks and production CPU-reference validation.
- **Tests:** authored an unrun two-tile GPU/CPU artifact-parity fixture for
  normal maps and AO, plus cross-tile refusal cases. No tests or builds were run
  at the user's request.
- **Risks:** the new API and GPU fixture are uncompiled and unexercised. This
  adds device-backed scheduling only; staged multi-tile receipt publication,
  desktop/CLI invocation, supported-device runs, and native acceptance remain
  open.
- **Rollback:** remove the two scheduler APIs/result types, their source
  implementations, the added fixture helpers and test, and this ledger/queue
  continuation. Preserve existing single-tile workers and all other dirty work.
- **Rejected:** publishing each completed tile directly to a live store, which
  could expose a partial set after a later tile failure; claiming native or
  hardware acceptance from source changes.

Direct impact is Cartographer's D3D12 and production bake APIs. No RegOS,
IDA_Training, hidden_canopy_bot, or Ask_IDA contracts, datasets, evaluations,
schemas, or deployments changed.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source-only bounded D3D12 multi-tile normal-map and AO scheduling.
Promotion progress: none; compilation, native device runs, staged publication,
desktop/CLI wiring, and real-asset acceptance remain open.

### D3D12 multi-tile CLI wiring continuation (2026-10-26, code-only)

Added `bake-normal-tiles-d3d12` and `bake-ao-tiles-d3d12` plus matching
inspectors. Both routes invoke the bounded library schedulers, retain a single
worker platform profile across the set, publish through typed production
receipts into a private staging directory, read each artifact and the ordered
manifest back, revalidate the project source snapshot, and atomically rename
the complete set into place. The inspectors enforce current tile coverage and
backend-specific receipt profiles. The D3D12 scheduler parity/cross-tile test
is now registered in the native test executable.

- **Files changed:** `apps/cli/main.cpp`, `tests/d3d12_tests.cpp`, this ledger,
  `tests/CMakeLists.txt`, `tests/run_ao_cli_tests.cmake`,
  `tests/run_bake_normal_cli_tests.cmake`, and the ignored local work queue.
  This composes with the earlier scheduler source changes in `libs/d3d12`.
- **Capability gained:** command-line callers can publish and inspect complete
  D3D12 normal-map and AO tile sets with per-tile receipts and worker identity.
- **Boundary improved:** mixed worker profiles, stale project sources, missing
  artifacts, manifest reordering, receipt mismatch, cross-tile faces, or a
  failed tile cannot produce the final output directory.
- **Governance preserved:** each tile still passes the existing D3D12 CPU
  parity and provenance checks, then the production artifact-store boundary.
  The routes do not change project state or infer authority from worker data.
- **Tests:** the two-tile GPU/CPU parity and cross-tile refusal test is
  registered. Conditional CLI cases cover multi-tile normal/AO publication,
  inspection, source immutability, and cross-tile refusal. No build or tests
  were run at the user's request.
- **Risks:** CLI integration and inspectors remain uncompiled and unexercised;
  failed publication can leave a `.pending-*` directory for inspection.
- **Rollback:** remove the two multi-tile D3D12 CLI routes, their inspector
  backend option, the shared staged-publication helper, and the test registration.
  Preserve the library schedulers and existing single-tile commands.
- **Rejected:** publishing individual tile outputs as each GPU task finishes,
  and accepting a ledger whose tile receipts use different worker profiles.

Direct impact is Cartographer's CLI, D3D12, and production receipt workflow.
RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts, datasets,
evaluations, schemas, and deployments did not change.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source-only D3D12 normal-map/AO tile CLI and inspection wiring.
Promotion progress: none; compilation, native device runs, desktop invocation,
and real-asset acceptance remain open.

### Desktop D3D12 bake invocation continuation (2026-10-26, code-only)

The desktop target now links `carto_d3d12_bake` when available and exposes an
opt-in D3D12 choice for AO and tangent-normal tile bakes. The background task
uses the scheduler stop token, typed per-tile D3D12 publication, CPU parity and
source rechecks. Cancellation is observed between GPU tiles and before final
publication; an active tile is allowed to finish. Other map types continue to
use their existing CPU-reference workflow.

- **Files changed:** `apps/desktop/CMakeLists.txt`,
  `apps/desktop/bake_workflow.hpp`, `apps/desktop/main.cpp`,
  `libs/d3d12/include/carto/d3d12/bake_graph.hpp`,
  `libs/d3d12/src/bake_graph.cpp`, `tests/d3d12_tests.cpp`,
  `tests/CMakeLists.txt`, `tests/run_ao_cli_tests.cmake`,
  `tests/run_bake_normal_cli_tests.cmake`, this ledger, and the ignored local
  work queue.
- **Capability gained:** desktop users can select D3D12 execution for AO and
  tangent-normal tiles while keeping CPU-reference output parity and receipt
  publication visible in the same saved-UV workflow.
- **Boundary improved:** stale inspection/source state, tile failure, profile
  inconsistency, cancellation, output overwrite, or artifact/manifest
  read-back mismatch prevents final publication.
- **Governance preserved:** desktop actions remain read-only with respect to
  project state. They pass through D3D12 provenance validation, canonical CPU
  parity, the production store, and final mesh/UV binding checks.
- **Tests:** added pre-cancelled D3D12 scheduler assertions to the native test
  source and registered the test. Conditional native CLI cases cover normal
  and AO multi-tile publish/inspect and cross-tile refusal. No tests or builds
  were run at the user's request.
- **Risks:** desktop header/UI integration and the updated schedulers remain
  uncompiled. Cancellation waits for the current GPU tile. Failed staging can
  leave a `.pending-*` directory for inspection.
- **Rollback:** remove the desktop backend selector and dispatch, the
  D3D12-specific desktop workflow helpers, conditional CMake link/definition,
  and cancellation-token scheduler overloads/test assertions. Preserve the
  existing CPU workflow, scheduler APIs without token arguments, and CLI routes.
- **Rejected:** running GPU work on the UI thread, interrupting an active GPU
  command, labeling a worker result as accepted without CPU parity, or applying
  D3D12 to map types that do not have verified worker paths.

Direct impact is Cartographer's desktop, D3D12, and production bake workflow.
RegOS, IDA_Training, hidden_canopy_bot, and Ask_IDA contracts, datasets,
evaluations, schemas, and deployments did not change.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source-only opt-in desktop D3D12 AO and tangent-normal tile workflow
with tile-boundary cancellation. Promotion progress: none; compilation,
native device interaction, and real-asset acceptance remain open.

### Normal tile inspector filesystem boundary continuation (2026-10-26, code-only)

Hardened the shared normal tile inspector used by CPU and D3D12 commands to
reject symlinked output roots, receipt manifests, blob directories, and tile
payloads before reading them. Added conditional CLI fixtures for those paths;
symlink-dependent cases skip when the host cannot create the fixture.

- **Files changed:** `apps/cli/main.cpp`,
  `tests/run_bake_normal_cli_tests.cmake`, this ledger, and the ignored local
  work queue.
- **Capability gained:** inspectors refuse receipt or artifact data that is
  redirected outside the selected output directory through a filesystem link.
- **Boundary improved:** CPU and D3D12 inspection now applies the same
  non-following checks to the full output read path.
- **Tests:** authored root, manifest, and payload symlink rejection cases; no
  builds or tests were run at the user's request.
- **Risks:** behavior remains uncompiled and unexercised; symlink tests are
  conditional on host support.
- **Rollback:** remove the non-symlink checks and the three corresponding
  adversarial CLI cases.
- **Rejected:** following a link after checking only the manifest contents.

Direct impact is Cartographer's normal-map CLI inspector. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA contracts, datasets, evaluations, schemas, and
deployments did not change.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source-only non-following multi-tile normal inspection. Promotion
progress: none; compilation and CLI execution remain open.

### D3D12 bent-normal tile continuation (2026-10-26, code-only)

Added D3D12 bent-normal reconstruction using the checked AO ray projection,
canonical ray-identity remapping, source-triangle validation, deterministic CPU
artifact parity, bounded all-or-nothing U,V scheduling, and receipt-backed
publication. Added CLI bake/inspection commands and the desktop backend option
with tile-boundary cancellation. Regression sources cover CPU parity, receipt
publication, worker-report tampering, stale bindings, cancellation, cross-tile
faces, CLI inspection, and no-partial-output behavior.

- **Files changed:** D3D12 and production bake graph sources/headers, CLI and
  desktop bake workflow, D3D12 and CLI regression sources, this ledger, and the
  ignored local work queue.
- **Capability gained:** D3D12 can now produce, schedule, publish, and inspect
  multi-tile bent-normal maps through the existing CPU-reference and artifact
  receipt boundaries.
- **Boundary improved:** worker output is mapped by stable ray identity, rejects
  unknown/duplicate rays and source triangles, fails closed on stale sources,
  and never returns a partial tile schedule.
- **Tests:** regression cases were authored; no builds or tests were run at the
  user's request.
- **Risks:** implementation and CLI/UI paths are uncompiled and unexercised.
  Bent-normal and AO compilation share the checked D3D12 ray projection; their
  output reconstruction and CPU parity checks remain separate.
- **Rollback:** remove the bent-normal D3D12 API/scheduler/publication path,
  CLI commands, desktop dispatch and option, and the corresponding regression
  sources; CPU bent-normal baking remains available.
- **Rejected:** treating AO RGBA output as bent-normal output, claiming D3D12
  parity without the worker report, or returning partially completed tiles.

Direct impact is Cartographer BakeGraph, CLI, and desktop tile baking. RegOS,
IDA_Training, hidden_canopy_bot, and Ask_IDA contracts, datasets, evaluations,
schemas, and deployments did not change. API/schema risk is limited to new
in-process D3D12 APIs and CLI commands; serialized artifact schemas are
unchanged. Dataset/eval, deployment, and promotion status are unchanged; native
execution and real-asset acceptance remain open.

Dataset progress: unchanged. Training/prompt progress: unchanged. Capability
progress: source-only D3D12 bent-normal tile path. Promotion progress: none;
build, tests, device execution, and acceptance remain open.

### Receipt-inspector evidence boundary continuation (2026-10-26, code-only)

Changed CLI inspectors to report `verification=artifact-source-receipt-readback`
after validating the persisted receipt, payload digest, artifact, recipe, and
current source binding. They retain the recorded platform profile but no longer
present bake-time CPU parity as if an at-rest inspection had independently
replayed the transient worker report. Bake commands continue to report their
live CPU-reference/parity evidence.

- **Files changed:** `apps/cli/main.cpp`, the AO and normal CLI test sources,
  this verification ledger, and the ignored local work queue.
- **Capability gained:** inspection output separates current read-back/source
  verification from the producer's recorded platform profile.
- **Boundary improved:** persisted D3D12 receipts cannot be mistaken for
  revalidated worker parity; CPU and D3D12 inspectors share explicit scope.
- **Tests:** CLI assertions now require the read-back scope, preserve profile
  visibility, and reject inspector output that reintroduces `evidence=`. No
  builds or tests were run at the user's request.
- **Risks:** CLI text output changed and remains uncompiled/unexercised; callers
  that parse the prior `evidence=` inspect field must switch to
  `verification=` plus `platform_profile=`. Serialized schemas are unchanged.
- **Rollback:** restore the previous inspector output fields and corresponding
  CLI assertions; leave bake-time evidence fields intact.
- **Rejected:** claiming parity from a platform-profile string alone or adding
  an unauthenticated worker claim to the serialized receipt.

Direct impact is Cartographer CLI inspection. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA contracts, datasets, evaluations, schemas, and
deployments did not change. Dataset, training, capability promotion, and
deployment status are unchanged; no build/test/device acceptance is claimed.

### Desktop tile-count reporting continuation (2026-10-27; code-only)

The desktop bake result now carries the schedule's compiled-tile count and an
explicit zero reused count for CPU AO, bent-normal, curvature, thickness,
height-displacement, world-position, material/surface-ID, and D3D12 normal/AO/
bent-normal paths. Tangent-normal CPU reporting continues to use the scheduler's
actual compiled/reused counts. The shared result helper only derives counts
from returned schedule entries; it does not estimate progress.

- **Files changed:** `apps/desktop/bake_workflow.hpp`,
  `tests/bake_tile_incremental_tests.cpp`, this ledger, and the ignored local
  queue.
- **Capability gained:** desktop completion messages distinguish tiles that
  were compiled from tiles reused through the existing CPU tangent-normal
  cache, across the remaining published tile workflows.
- **Boundary improved:** counts are sourced from completed schedules and the
  cache's real reuse receipt; unsupported or failed schedules still publish no
  completion counts.
- **Tests authored:** added a regression for schedule-derived compiled and
  reused metrics. **Run:** none, per the code-only sprint instruction.
- **Not run:** builds, tests, D3D12 execution, or native UI review.
- **Risks:** the desktop header and reporting path are uncompiled and the
  displayed counts have not been exercised in a native completion flow.
- **Rollback:** remove the schedule-metrics helper and its callback/constructor
  wiring, plus the focused regression; other bake artifact and receipt behavior
  remains unchanged.
- **Rejected:** inferring reuse from cache-directory presence or labeling every
  scheduled tile as reused; only tangent-normal CPU reports actual reuse.

Direct impact is Cartographer desktop bake reporting. RegOS, IDA_Training,
hidden_canopy_bot, and Ask_IDA contracts, datasets, evaluations, schemas, and
deployments did not change. Dataset, training, capability promotion, and
deployment status are unchanged. Bake workflow acceptance remains open.
