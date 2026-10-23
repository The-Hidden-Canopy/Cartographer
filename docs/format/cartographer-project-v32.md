# Cartographer project schema v32

Schema v32 adds the mandatory `AUTHORING_STANDARDS_HEX` record after the v31
`DECAL_PLACEMENTS` table, including when the standards collection is empty.
The record contains the hex-encoded canonical serialization of the bounded
project authoring standards collection. The collection preserves explicit
human-authored constraints and their provenance; it does not infer standards
from scene or fallback data.

Standards are validated before they enter the project. Evidence must refer to
a project revision earlier than the revision that stores it. Updating the
standards collection advances the project revision, and unchanged values do
not create a new revision.

Schemas v1-v31 remain readable. Explicit standards records written by schema
v23 or later are retained when loading and migrating those projects. Older
schemas without a standards record initialize the collection as empty; they
never fabricate project standards.
