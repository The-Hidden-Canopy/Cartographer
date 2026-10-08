# Cartographer project format v9

Cartographer v9 is the current deterministic `.carto` authoring format. It
retains every v8 record and adds one required stable source namespace near the
document header:

```text
CARTOGRAPHER_PROJECT 9
AUTHORING "cartographer.authoring" "meters" "right_handed_y_up"
SOURCE_NAMESPACE "cartographer.project.<stable-id>"
NAME "..."
REVISION <unsigned integer>
...
MATERIAL_CATALOG "<versioned source catalog>"
END
```

The namespace is durable project identity, not a path, credential, authority
grant, machine identifier, or content digest. Scene nodes, mesh assets,
material regions, and future prepared records use it to distinguish equal
numeric IDs originating in different projects. Save-as and relocation preserve
it. Creating an independent project produces a different namespace even when
the user-visible name is the same.

The namespace accepts only bounded portable identifier characters. A v9 file
with a missing, empty, malformed, or duplicate-position namespace record fails
before publication. There is no client override in the shared preparation
operation; UI, CLI, and headless callers receive the namespace from the
validated source document.

Versions 1 through 8 remain readable. During legacy read, Cartographer derives
a deterministic migration namespace from the exact legacy document bytes.
Repeated reads of the same legacy source therefore produce the same identity,
and the next supported save persists it explicitly as v9. This derivation is a
migration bridge, not a claim that two historically copied and independently
edited legacy files can always be disambiguated without human input.

All v8 material-catalog, world, scientific, UV/attribute, topology lineage, and
evaluation-graph rules remain in force. Derived render products, native engine
objects, private runtime types, and credentials remain outside authoring truth.
