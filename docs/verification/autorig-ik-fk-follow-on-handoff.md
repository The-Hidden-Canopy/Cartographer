# Autorig IK/FK follow-on handoff

## Scope and lineage

This is an in-progress follow-on to `f077bfa` (reviewed autorig persistence
groundwork), based on branch `codex/autorig-durable-acceptance-20261031`.
It adds source-level IK/FK authoring records, a bounded proposal path, a
schema-v37 nested-bundle extension, and human application commands. The
integrator must record the final commit and exact evidence after the build.
No integration queue claim or merge is made here.

The earlier v36 bundle persists an accepted review, preview-to-durable joint
mapping and graph, rig/control definition, limits, and transferred binding
with per-vertex weights. The follow-on serializes explicit two-bone IK/FK
configuration in a v2 nested bundle under project schema v37. Existing v1
bundles remain readable; empty configuration writes v1, while nonempty
configuration writes v2. The v37 reader refuses v2 labeled as v36 or earlier.
An older v36 reader refuses the v37 outer schema, and a nested v1-only reader
refuses v2. See [schema v37](../format/cartographer-project-v37.md).

## Intended mechanism and trace

The requested capability is an artist-controlled, source-bound pipeline, not
just a skeleton or a solver result: source mesh and parts, optional semantic
evidence and human anchors flow through topology, components, geodesics and
symmetry to alternative joint and linkage hypotheses. The system should
expose confidence, ambiguity and alternatives in a read-only preview; the
human chooses or repairs a proposal before stable IDs and a durable rig are
created. Artist, imported, procedural and learned evidence have different
provenance. Shape alone must not be treated as proof of a physical constraint.
Manual construction remains a fallback, and failed or cancelled work must
leave the last accepted source intact.

The current derivation is source and revision bound: feature analysis records
topology, geodesics and symmetry; authored partonomy, landmark assertions and
their provenance constrain a bounded enumeration of alternatives. Transparent
policy-weighted scores rank those alternatives, including optional source
surface crossing evidence; that centerline proxy is not a volumetric overlap
proof or a riggability verdict. Review locks, edits and replans remain
speculative until a caller-supplied human review assertion binds the selected
candidate. Durable joint IDs, parent-local rest transforms, rig controls and
limits are explicitly authored, validated and journaled with transferred
weight influences and receipts. This follow-on binds explicit chain and
control roles to those exact records, then invokes the existing no-position-
jump matchers to produce a proposal. It does not replace candidate ranking,
constraint evidence or human authority with a generic solver shortcut.

The local ledger `docs/CARTOGRAPHER_WORK_QUEUE.local.md`
and `retained character-production execution matrix`
trace this path and its proof obligations. The latter is local reference
material, not a committed project artifact:

| Requirement | Required mechanism or acceptance gate |
| --- | --- |
| `AUT-001` | Deterministic topological, geodesic and symmetry analysis across a diverse mesh corpus. |
| `AUT-002` | Alternative skeleton generation and transparent ranking, with humanoid, quadruped, creature and mechanical candidate receipts. |
| `AUT-003` | Optional semantic landmark and partonomy constraints, including conflicting evidence refusal and provenance. |
| `AUT-004` | Native candidate preview, lock, edit, replan and commit; reviewed proposals must leave the project untouched until acceptance. |
| `AUT-005` | Unfamiliar and unriggable inputs, including unseen classes and documented ambiguity or refusal. |
| `RIG-004` | Reusable templates that can be instantiated, edited and migrated without overwrite. |
| `RIG-005` | Mirror and symmetry/asymmetry rig edits and control copies, including double-mirror identity and divergence. |
| `RIG-006` | Mixed mechanical and organic articulation with rigid links, a multi-link fixture and non-skin export. |
| `SKN-003` / `UI-002` | Painting, locking, mirroring, flood and normalization, plus native brush, heatmap, undo and reopen. |
| `SKN-004` | Certified transfer with stable topology/multires correspondence and explicit seam ambiguity and failure reports. |
| `IK-004` | Editable matching, pole, aim and space controls with a no-pose-jump native switch fixture. |
| `ANI-002` / `UI-003` | Human key, tangent and retime editing with undo and visual proof; native scrub/key editing must show the correct pose. |

The component queue contains historical CPU-level completions for some of
these IDs. They do not satisfy the separate native, independently authored
asset, non-skin export or runtime acceptance gates in the execution matrix.
The full design also requires typed control and constraint dependencies,
declared priority and conflict handling, and provenance through pose,
deformation, keying and publication. At least two independent assets per
target morphology class are called for; synthetic fixtures alone are not
that corpus.

## Current source boundary

`IkFkAuthoring` pins the exact rig definition, graph and limit set and records
one contiguous root/mid/effector chain, distinct FK controls, an IK target,
optional pole, mode control/axis and values, deterministic solver budget and
position-jump bound. A record is bounded to 4 KiB; the nested bundle supports
up to 128 sorted, unique chains. Distinct chains cannot share controls until
an arbitration contract exists. Common rig, graph and limit validation is
shared across a collection, with a conservative 64 MiB byte proxy based on
component sizes multiplied by chain count; it is not a strict CPU-operation
bound. Configuration requires explicit authoring;
generic control labels or geometry do not infer those choices.

Project configuration first checks bundle and current mesh consistency and
then stages a compare-and-swap replacement. `HumanApplicationAccess` exposes
acceptance and configuration with expected project generation, project
revision and record revision, using the application command/history path.
It is a software human-admission boundary, not cryptographic authentication
of the claimed reviewer. Application reads check freshness before producing a
bundle or switch proposal. The external source digest and source binding
provenance remain caller assertions; later source changes do not automatically
rewrite the stored layer's freshness field.

`propose_ik_fk_switch` computes a proposal for one explicit chain. It requires
the default coordinate policy and refuses posed ancestors outside that chain.
The result includes a mode value and matching data; it does not apply generic
controls or persist a mode change. It enforces a positional jump threshold,
while effector orientation jump is reported rather than bounded. There is no
simultaneous chain or constraint arbitration, control-space application,
mode blending, timeline keying, native UI or runtime integration in this
source boundary. Sparse transferred weights may leave vertices rigid.

## Decisions needed before completing the workflow

The next implementation needs explicit product direction on how a world-space
IK target position and pole direction map to control channels and spaces,
including parent/follow-space behavior and orientation. It also needs a
defined policy for simultaneous chains and conflicting hard constraints:
ownership, priorities, arbitration and failure reporting cannot be inferred
from independent proposals. Finally, decide who owns IK/FK mode state, how
mode values blend, when and at what exact time a switch is keyed, and how the
result is committed, undone and replayed. Do not silently derive these
semantics from generic affected-joint lists or make a proposal mutate controls.

## Verification and coordination

The local queue records an active prior build-coordinator lease. No CMake
build or registered CTest for this follow-on has been run; confirm lease
release and coordinate before building the combined candidate. One isolated,
single-process compile linked `tests/ik_fk_authoring_tests.cpp` and
`libs/pose/src/ik_fk_authoring.cpp` against the existing f077bfa base archives
using the configured LLVM-MinGW C++23 compiler with `-Wall -Wextra -Wpedantic`,
and the resulting standalone test passed (exit 0), including after shared
validation was introduced. It did not link the project or application changes.
A serial `-std=c++23 -fsyntax-only` pass completed without diagnostics for
`project.cpp`, `transaction.cpp`, `application.cpp`, and the accepted-autorig,
IK/FK-project, application-history, character-records, prefab-placement,
authoring-standards, source-registry and rig-module test sources. The final
`project.cpp` was checked again after shared validation (exit 0). The pass was
voluntarily stopped during the sweep-source fixture to limit resource use;
that fixture was not checked. No linked project/application tests or migration
CTest were run. The integrator must record final candidate command lines, exit
codes and pass/failure counts. Tests of canonical records or a single two-bone CPU fixture do
not establish real asset quality, native interaction, visual correctness,
orientation continuity, multi-contact behavior, undo/reopen, export or VANTA
runtime parity.

The passing standalone check was run from this checkout as follows; the
executable was written outside the checkout's build tree:

```powershell
$compiler = (Get-Content build/default/CMakeCache.txt | Where-Object { $_ -like 'CMAKE_CXX_COMPILER:FILEPATH=*' }).Split('=')[1]
$includes = @('libs/pose/include','libs/character/include','libs/core/include','libs/assets/include','libs/botanical_contract/include','libs/geometry/mesh/include','libs/attributes/include','libs/kinematic/include','libs/scene/include','libs/temporal/include') | ForEach-Object { '-I' + $_ }
$libraries = @('build/default/libs/pose/libcarto_pose.a','build/default/libs/character/libcarto_character.a','build/default/libs/temporal/libcarto_temporal.a','build/default/libs/geometry/mesh/libcarto_geometry.a','build/default/libs/attributes/libcarto_attributes.a','build/default/libs/kinematic/libcarto_kinematic.a','build/default/libs/assets/libcarto_assets.a','build/default/libs/botanical_contract/libcarto_botanical_contract.a','build/default/libs/scene/libcarto_scene.a','build/default/libs/core/libcarto_core.a')
& $compiler -std=c++23 -Wall -Wextra -Wpedantic @includes tests/ik_fk_authoring_tests.cpp libs/pose/src/ik_fk_authoring.cpp @libraries -lwindowscodecs -lole32 -o ../ik_fk_authoring_check.exe
& ../ik_fk_authoring_check.exe
```

After build coordination, build the exact reconciled candidate's
`cartographer_ik_fk_authoring_tests`, `cartographer_autorig_ik_fk_project_tests`,
`cartographer_application_autorig_history_tests`,
`cartographer_accepted_autorig_project_tests`, and the affected schema
regression targets; run their CTests and record the result before treating this
follow-on as ready for integration. Re-run the broader application tests after
reconciling the UV lane's shared files.

The active UV lane also touches `libs/application/include/carto/application/application.hpp`,
`libs/application/src/application.cpp` and `tests/CMakeLists.txt`. Reconcile
those shared files with the UV worker before integration; do not overwrite
their changes or claim their scope. Manual acceptance still needs independent
assets in each target class, native review and repair, skin painting/heatmap,
timeline and switch interaction, save/reopen and undo, visual and numerical
checks, non-skin export where applicable, and external-runtime validation.

## Per-file line counts

Compared with `f077bfa`; new files were untracked at handoff preparation.

Previously tracked files (added / deleted):

- `docs/architecture/character-production.md`: +52 / −0
- `libs/application/include/carto/application/application.hpp`: +53 / −0
- `libs/application/src/application.cpp`: +101 / −0
- `libs/pose/CMakeLists.txt`: +1 / −0
- `libs/project/CMakeLists.txt`: +2 / −2
- `libs/project/include/carto/project/accepted_autorig.hpp`: +31 / −0
- `libs/project/include/carto/project/project.hpp`: +1 / −1
- `libs/project/include/carto/project/transaction.hpp`: +6 / −0
- `libs/project/src/project.cpp`: +90 / −9
- `libs/project/src/transaction.cpp`: +45 / −7
- `tests/CMakeLists.txt`: +15 / −0
- `tests/accepted_autorig_project_tests.cpp`: +19 / −3
- `tests/character_records_project_tests.cpp`: +27 / −19
- `tests/condition_prefab_placement_tests.cpp`: +1 / −1
- `tests/project_authoring_standards_tests.cpp`: +4 / −4
- `tests/project_source_asset_registry_tests.cpp`: +2 / −2
- `tests/rig_module_project_tests.cpp`: +6 / −6
- `tests/sweep_source_project_tests.cpp`: +2 / −2

New files (added / deleted):

- `docs/format/cartographer-project-v37.md`: +52 / −0
- `docs/verification/autorig-ik-fk-follow-on-handoff.md`: +204 / −0
- `libs/pose/include/carto/pose/ik_fk_authoring.hpp`: +77 / −0
- `libs/pose/src/ik_fk_authoring.cpp`: +211 / −0
- `tests/application_autorig_history_tests.cpp`: +254 / −0
- `tests/autorig_ik_fk_project_tests.cpp`: +274 / −0
- `tests/ik_fk_authoring_tests.cpp`: +160 / −0
