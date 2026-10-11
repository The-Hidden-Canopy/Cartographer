# Surface workflow compiler

Cartographer's first portfolio-donor tranche is a standalone, public CPU
workflow around the existing `carto_surface_core` compiler. It ports patterns,
not sibling runtime dependencies:

| Cartographer mechanism | Donor concept | Adaptation | Intentionally not copied |
| --- | --- | --- | --- |
| `SurfaceWorkCandidate` and deterministic planner | VANTA utility / expected-information-gain scheduling | Rank bounded representation work by importance, uncertainty, expected error reduction, and estimated cost | Runtime adaptive rendering and model-backed prediction |
| `SurfaceEvidenceReservoir` | IDA hard-case reservoir and selective verification | Reuse the latest evidence only when representation identity, source content, revision, and dependencies match | Training, telemetry, or remote evidence services |
| `SurfaceInvalidationGraph` | TraceGlass derived edges and `StateDelta` | Traverse typed surface artifacts from an exact face/edge/material/process/semantic delta | Cross-repository event buses and hidden state mutation |
| `FidelityBudget` and greedy selector | Low-SWaP utility governor | Select at most one revision-matched representation per domain under package, geometry, closure, runtime-estimate, and cook-estimate budgets | Hardware power or measured runtime claims |

## Standalone boundary

The workflow uses only Cartographer-native `SurfaceDomainId`,
`RepresentationId`, `Revision`, geometry IDs, `Result`, `Diagnostic`, and
content digests. It compiles without VANTA, IDA, MENAGERIE, GALVANI, TraceGlass,
CONDITION, or any private repository. Donor names are documentation lineage,
not link-time dependencies.

## Authority and failure behavior

All four systems are planning or evidence projections. They do not mutate an
editable mesh, publish a renderer resource, alter the representation tree, or
silently promote a candidate. Invalid identities, stale revisions, malformed
digests, duplicate IDs, cycles, non-finite metrics, and budget overflow fail
closed. Candidate plans and budget selections carry canonical digests; candidate
and option inputs are bounded; selections are bound to one source revision;
evidence records are append-only and the latest record for an identity wins, so
a stale latest record cannot fall back to older evidence.

The invalidation graph treats a dependency with no local footprint as global,
while face- and edge-scoped artifacts are invalidated only when the changed
footprint intersects. Downstream invalidation is deterministic and cycle
checked.

The selector uses deterministic greedy quality-per-estimated-cost ordering. Its
runtime and cook fields are estimates for planning only; they are not measured
hardware telemetry and do not imply GPU, VANTA, or 8K performance.

## Deferred layers

This tranche does not implement topology-aware remeshing, PBR-parity evaluation,
GPU acceleration, process-driven topology mutation, a CONDITION/Pine Ridge
corpus, or VANTA runtime admission. Those remain separate milestones with their
own evidence gates.
