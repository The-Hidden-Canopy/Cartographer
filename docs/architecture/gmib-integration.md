# GMIB simulation boundary

`carto_simulation` is the first Cartographer-side vertical slice for **GOD
MODE IS A BUG**. It is a session-local derived projection over Cartographer
authoring data, not a second project document and not a VANTA dependency.

```text
Cartographer Scene (authoring truth)
    -> validated session snapshot
    -> explicit intervention in session state
    -> anomaly record + replay event
    -> bounded observer evidence
```

The session captures double-precision world transforms and the source scene
revision when it is created. An intervention is rejected when the source scene
revision has changed, so a stale authoring document cannot silently drive a
simulation run. Interventions never mutate `Scene` or `ProjectDocument`.

The first slice supports spawn, destroy, and teleport projections, monotonic
ticks, deterministic replay records/digests, and observer range/attention/
recording boundaries. `ObservationRecord` is evidence with a quality label; it
is not evaluator truth. Actor/session/intent provenance is not copied into the
public anomaly record.

## Playable game slice

`carto::simulation::GameSession` now composes the authoring scene and the
session-local simulation into a deterministic red-cube game scenario. The
scenario opens at tick 100 with a scientist, skeptic, and camera; teleport,
step, and delete actions produce anomaly/evidence frames and a replayable
`history-scar` phase without deleting authoring objects. Run the headless game
loop with:

```powershell
cmake --preset default
cmake --build --preset default --target cartographer_gmib_game
.\build\default\apps\gmib\cartographer_gmib_game.exe
```

The runner is scripted so CI can verify the exact causal sequence. The session
also accepts `--interactive` with the commands `step`, `teleport X Y Z`,
`delete`, `observe`, and `quit`. It is ready for a future Cartographer
editor/viewport input loop; this change does not claim that the native desktop
UI is already a game surface.

This target remains independently buildable in Cartographer and has no private
VANTA headers, namespaces, build targets, fixtures, or runtime service
dependency. VANTA owns the authoritative-world adapter separately. The two
products can exchange a future neutral GMIB evidence package, but neither is
required to link the other.

Not implemented by this slice: base-transform baking, physics backends, memory
and testimony, claims/hypotheses, social propagation, institutions, durable
history-scar mechanics beyond the playable phase, or native anomaly panels.
Those remain explicit follow-up layers.
