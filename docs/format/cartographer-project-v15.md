# Cartographer project format v15

Cartographer v15 retains all v14 records and adds a required
PRESENTATION_CATALOG record after CONDITION_AUTHORING. The nested value is a
serialized CARTOGRAPHER_PRESENTATION_CATALOG v1 document.

The catalog stores source camera bookmarks and analytic light intent keyed by
stable scene-object identity. Camera records include projection, clip range,
review purpose, and optional logical reference-image identity. Light records
include analytic shape, photometric intent, optional color temperature,
fixture/power/switch ownership labels, and normal/failure intent. These fields
are descriptive authoring truth; they do not authorize runtime switching,
simulate failure, or bind renderer/device handles.

The project transaction validates every referenced scene object and journals
the resulting project snapshot before publication. Removing an object while a
camera or light record still references it is rejected. At most one camera may
be active for review. The nested catalog is size-bounded and rejects malformed
enums, non-finite values, invalid analytic parameters, duplicate identities,
and trailing data.

Versions 1 through 14 remain readable. A v14 project loads with an empty
presentation catalog and is saved as v15 on its next write. A v15 document
must contain the record, including an explicit empty catalog.

This format does not claim a native camera/light viewport preview, physical
unit calibration, renderer integration, runtime switch ownership, or
reference-image asset resolution. Those remain separate acceptance work.
