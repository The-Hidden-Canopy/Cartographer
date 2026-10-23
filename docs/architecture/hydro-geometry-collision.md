# Hydro geometry and collision boundary

Cartographer's public hydro layer authors a provider-neutral `HydroDomain` and
`HydroGraph`. It stores stable identity, source revision/digest, coordinate
frame, typed cell/edge topology, and deterministic CPU-reference water state.
The graph's `advance` operation is a bounded conservative reference step: it
does not claim GPU, game, weather, terrain, or production-fluid evidence.

The same public tranche includes a revision-bound `BathymetryField`, a
deterministic `WetDryField`, and stable-id `ShorelineField` extraction. These
products are derived from bed samples and water level; changing the terrain
identity, terrain revision, terrain digest, or hydro source makes dependent
products stale instead of silently reusing them.

The runoff slice adds a provider-neutral `HydroRunoffCatchment`, terrain
evidence, and deterministic rainfall-to-infiltration/runoff partition. A plan
binds the authored catchment to the terrain revision/digest and sampled tile
lineage, closes the gross water-volume ledger, and can produce a boundary flux
only after binding to the current `HydroGraph`. The graph remains authoritative
for edge kind, enablement, source revision, and capacity; runoff planning does
not mutate graph state and does not import a terrain, weather, game, or private
runtime implementation.

Water is not registered as a solid collider. `PlanarFreeSurface` and the
contact queries return source-bound fluid evidence: signed distance,
submerged fraction/volume, wetted area, normal, current, and a bounded
surface-crossing time of impact. Bed, bank, hull, and structural collision
remain separate products owned by the ordinary collision representation layer.

The public boundary intentionally stops at authored topology and neutral
reference products. Runtime solver selection, rendering, game consequences,
private integrations, and hardware-specific acceleration consume these records
out of tree and cannot mutate the canonical graph through this API.
