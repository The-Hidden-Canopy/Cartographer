# Prepared-scene publication

`carto_production::publish_prepared_scene()` publishes one verified prepared
revision without exposing a reader to a partially written bundle.

The filesystem contract is:

```text
<output-root>/
  current
  current.lock
  revisions/
    <prepared-scene-sha256>/
      manifest.carto-prepared
      products/...
```

Products and the manifest are written under a unique staging directory. The
publisher then reopens the stage, parses the manifest, verifies its canonical
identity, hashes every declared product, and checks that the revision tree has
no undeclared file or directory. Only a verified stage can be renamed to its
immutable digest path. The small `current` selector is replaced atomically.

Publication uses a mandatory compare-and-swap expectation. A null expectation
means no revision may already be selected; otherwise the exact current digest
must match. In-process and cooperating cross-process writers share the
`current.lock` lease. A late older operation therefore receives `stale_data`
instead of moving the selector backward. While that lock is held, the publisher
also verifies the selected revision and binds the output root to its source
namespace. It rejects a lower source revision and rejects different source
content claiming the same source revision. A different preparation profile is
still allowed for the same source digest and revision.

Readers resolve `current` once and then verify that immutable revision. They do
not follow the selector again mid-load. Missing products, changed bytes,
malformed selectors, unexpected files, path traversal, symbolic links, Windows
reparse points anywhere in the path ancestry, oversized products,
aggregate-size overflow, and conflicting
content under an existing immutable digest fail closed. A failed attempt never
replaces the last valid selection. An exact replay verifies and reuses the
existing immutable revision. If `current` is missing while the immutable
revision store is non-empty, both readers and publishers fail closed; selector
loss cannot make an owned root appear new or permit a lineage takeover.

This boundary proves coherent portable file publication. It does not claim an
Unreal, Unity, or private runtime editor applied native changes atomically;
native adapters own their own staging, mapping, rollback, and evidence.
