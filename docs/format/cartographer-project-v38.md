# Cartographer project schema v38

Schema v38 adds the mandatory `UNADMITTED_MODE_KEY_DRAFTS` table after
`ACCEPTED_AUTORIGS`. Older projects, including v37, migrate with an empty
table; a table under an older outer schema and a v38 document missing the
table are refused. Older readers must refuse v38. The table is independent
of accepted-rig replacement and is not automatically deleted or rewritten
when a rig, weight binding, or control configuration changes.

Each immutable insert-only row has a project-local draft identity,
revision one, canonical `IkFkModeKeyRecord` bytes and their digest, plus
the project namespace and project revision and accepted rig row identity,
revision and digest checked at insertion. A separate length-delimited
envelope checksum binds those values and the draft identity and record
digest. These checks provide integrity, not authentication, currentness, or
proof that external assertions are true. A reader must not infer that the
referenced rig is still current merely because the stored pins decode.
Undo can remove a row, after which its identity and row revision can be reused.
External references therefore need project-state and digest pins and verified
branch/restore lineage; the draft identity alone is not globally stable.

The table permits at most 512 records, 64 MiB aggregate canonical record
bytes, and the mode-key record codec's individual bound. Other project and
whole-snapshot journal limits also apply; a near-limit insertion may fail
atomically at publication. Duplicate identities and nonzero expected insert
revisions are refused. No replace, delete, or generic public raw insertion API
is supplied by this storage layer.

Every row is explicitly **unadmitted authored intent**. It is not a key in an
authoritative `MotionDocument` or `TemporalChannel`, an observed or executed
pose, a physical event, or a grant of runtime authority. Source and pose
digests, lineage, reference frames, clock correspondence, and the explicit
channel-to-control crosswalk remain caller assertions unless independently
resolved by their owning subsystem. Later owner admission requires fresh
source checks and its own decision; undo is authoring-history restoration,
not reversal of physical time. This format description is source-level work;
coordinated tests and native/runtime acceptance remain separate gates.
