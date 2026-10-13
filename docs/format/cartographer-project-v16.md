# Cartographer project format v16

Cartographer v16 retains the required v15 presentation catalog and adds a
required `CURVE_PATHS` table. Each entry binds an explicit source identity to
the exact serialized `CARTOGRAPHER_CURVE_PATH` v1 payload. The source path's
own revision is independent from the enclosing project revision.

New curve identities begin at source revision 1. Replacements must advance
that identity by exactly one revision; stale, skipped, or wrapped updates are
rejected. Project changes are staged through `ProjectTransaction` and become
visible only after the journal append succeeds. Curve sources are bounded by
count and aggregate serialized bytes, and all nested paths are validated on
both save and reopen.

Versions 1 through 15 remain readable. A v15 project loads with an empty curve
table and is saved as v16 on its next write. A v16 document must contain the
curve table even when it is empty.

This format does not claim curve editing commands, deletion/tombstone
semantics, adaptive arc-length sampling, frames/roll, offsets, road or utility
mesh generation, terrain cuts, hydro coupling, native UI controls, or runtime
realization. Source persistence is not a claim that those downstream systems
consume the curve yet.
