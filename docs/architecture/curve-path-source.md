# Curve path source kernel

`carto::geometry::CurvePath` is a provider-neutral authored cubic-Bezier path
for future road, ditch, gutter, cable, pipe, fence, and guide workflows. The
source binds a stable logical identity and revision to bounded cubic segments.
Adjacent segments share an exact endpoint; fixed-parameter evaluation is
deterministic for a fixed source and sample budget, returns finite unit
tangents, and preserves per-segment tangent changes at joints. Parameter
spacing is not a claim of equal arc-length spacing.

The source rejects invalid identities, revision zero, empty or oversized paths,
disconnected segments, non-finite/out-of-range control points, hostile record
counts, and sampling requests above the explicit sample budget. Project schema
v20 stores the serialized source and its active/deleted lineage. The native
CONDITION workbench can create or edit a one-segment centerline paired with
road intent. The application command stages both records, binds the road to
the exact resulting centerline revision, checks the project snapshot, and
journals one project revision. Undo/redo retains tombstones and advances
source revisions rather than restoring old revision numbers. Transaction APIs
also reject stale or skipped updates before journal publication.

The native editor is deliberately narrow: it edits a single cubic segment and
supports one road source per centerline. It does not yet provide multi-segment
editing, adaptive arc-length sampling, frames/roll, offsetting, road
cross-sections, terrain cuts, hydro coupling, generated mesh products, or
runtime realization. No terrain, collision, or runtime capability is implied
by evaluating a curve.
