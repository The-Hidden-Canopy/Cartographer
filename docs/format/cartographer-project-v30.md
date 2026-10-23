# Cartographer project schema v30

Schema v30 adds the mandatory `RIG_MODULE_TEMPLATES` table after the
CONDITION vegetation placement receipt table, including when the library is
empty. Each `RIG_MODULE_TEMPLATE` record stores a stable module name, a
project-local edit revision, a SHA-256 digest, and the canonical serialized
`RigModuleTemplate` bytes. The project parser uses the public character-module
codec and rejects unknown schemas, malformed or non-canonical payloads,
identity mismatches, duplicate names, digest mismatches, invalid revisions,
and entries outside the per-template (16 MiB), count (4,096), or aggregate
(64 MiB) limits.

Insertion is create-only and refuses an existing module name. Replacement is
available only through `ProjectTransaction` and requires the expected entry
revision and digest; a successful replacement advances the entry revision and
project revision together. The journal records only committed transactions.
There is no module deletion API in schema v30, so a deleted identity cannot be
silently recreated because deletion is not exposed.

Schemas v1-v29 remain readable. Migration initializes the v30 module library
as empty and never fabricates module content. Save/reopen preserves the
canonical template bytes, digest, and per-entry revision. This project library
does not provide a native editor workflow or make accepted autorig reviews a
committed rig.
