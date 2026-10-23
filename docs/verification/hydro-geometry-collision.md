# Hydro geometry and collision verification

The bounded public tranche proves:

- deterministic domain, graph, state, and receipt serialization/digests;
- revision-bound bathymetry sampling, wet/dry derivation, and shoreline
  extraction with stable segment identities;
- bounded D8 routing derived from the canonical bathymetry field, deterministic
  plateau resolution, explicit outlet declarations, and retained closed sinks;
  stale source digests, malformed routes, duplicate/out-of-grid outlets, and
  conservation of contributing catchment area are covered by the CPU fixture;
- deterministic rainfall, infiltration, and runoff partitioning whose gross
  volume ledger closes and whose boundary flux is admitted only against the
  current graph source and edge capacity;
- duplicate, dangling, stale, non-finite, future-version, trailing-data, and
  capacity failures reject before accepting a derived product, including stale
  runoff terrain revisions, crossed tile lineage, uphill outlets, non-finite
  precipitation, over-capacity boundary flux, and graph source mismatch;
- internal graph transfers conserve volume and boundary fluxes reconcile the
  volume ledger;
- planar point/sphere contact reports monotonic submergence without becoming a
  solid collision result;
- entry and exit sweeps report a bounded crossing fraction and reject invalid
  surfaces.

This is CPU-reference and structural/analytic evidence only. Terrain routing
does not model culvert hydraulics, time-varying water, erosion, or runtime water
state. It does not claim GPU timing, 4K/8K rendering, device-loss recovery,
full hydrology, game acceptance, or private runtime integration.
