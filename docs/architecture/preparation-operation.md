# Shared preparation operation

`carto_production::prepare_and_publish()` is the single public operation used by
headless callers, the CLI, and the desktop shell. It captures one validated
`ProjectDocument`, one revision-bound stable-ID scope, and one deterministic
profile before deriving any product. Canonical source identity is serialized
and hashed once for the operation, then reused by each scoped product compiler;
multi-asset preparation does not re-hash the entire project per mesh.

Profiles use the bounded, reopenable
[preparation-profile v1](../format/preparation-profile-v1.md) contract. The
profile digest, rather than an unsaved UI default, participates in operation
and prepared-scene identity.

The operation executes the same stages for every client:

1. resolve the source revision, durable source namespace, scope, and profile;
2. plan the exact scoped material and dependency closure;
3. validate channel choices and authored material-region assignments;
4. compile render meshes and supported optional lookdev products;
5. capture and inspect source texture bytes, compose the neutral envelope, and
   verify every payload digest; and
6. publish through the compare-and-swap prepared revision selector.

A request may carry a standard C++ stop token. Cancellation is checked at
stage and source-item boundaries and again immediately before publication. It
returns a structured `*.cancelled` diagnostic and never replaces the selected
last-valid revision. Atomic publication itself is not interrupted midway.

The portable profile uses each mesh's explicit active render UV and at most one
unambiguous lightmap candidate. It derives tangents only when a primary UV is
present and recognizes the durable `condition.material_region` layer. Source
material meaning is always carried in a scoped material-catalog product.
Cartographer-native lookdev is an optional derived product: unsupported native
packing is reported as a warning and cannot erase or downgrade the preserved
source catalog.

Texture locators are authoring inputs only. They are resolved beneath the
project directory, checked from the filesystem root through every source and
locator component against symbolic links and Windows reparse points, bounded,
structurally inspected according to the
declared source format, and matched to the catalog digest and known dimensions.
The captured bytes become content-addressed included products. A relocated
prepared revision therefore remains usable after the source tree is removed.

Failures return stable diagnostic codes, operation stage, source namespace,
optional source IDs, capability, explanation, and lower-level context. They do
not publish. Optional warnings can accompany a successful report without being
promoted into native or runtime evidence.

CLI examples:

```text
cartographer_cli prepare project.carto project.carto.prepared scene
cartographer_cli write-default-preparation-profile portable.carto-prep
cartographer_cli prepare project.carto project.carto.prepared --profile portable.carto-prep scene
cartographer_cli prepare project.carto selection.prepared selection 12 18
cartographer_cli inspect-prepared project.carto.prepared
```

The desktop writes a sibling `<project>.prepared` directory. `Ctrl+Alt+B`
prepares the saved scene; `Ctrl+Alt+Shift+B` prepares the captured object
selection. Both routes call the shared operation rather than implementing
preparation in UI callbacks.

`carto_production::PreparationJob` owns the background form of that same
operation. It copies one immutable project snapshot, links caller and UI
cancellation to a cooperative stop token, joins on destruction, and exposes
queued, running, cancellation-requested, published, cancelled, stale, and
failed states. A cancellation request arriving after atomic publication cannot
relabel the completed result as cancelled.
