# Exact-tick step curves

`motion::KeyedCurve` supports an explicitly authored `step` interpolation for
discrete scalar intent. Between two keys it holds the earlier key's value; at
the next exact key tick the new value takes effect. Values outside the key
range clamp to the nearest endpoint, and a sample on the wrong timebase is
refused. The step path compares integer ticks before any tick subtraction, so
even the full signed tick range does not overflow interpolation arithmetic.
No intermediate mode value is invented.

The existing linear, smoothstep and cubic-Bezier modes retain version-one
serialization and byte-identical canonical digests. Step writes a version-two
curve with `INTERPOLATION step`; a version-one curve naming step, a
version-two curve naming a legacy interpolation, an unknown interpolation,
an unknown version or noncanonical v2 text is refused. Version-two encoding
and parsing use the classic locale; legacy v1 behavior remains unchanged.
Readers limited to version one therefore fail
closed on step data. Existing pure curve-edit operations retain their normal
source/result digest receipt and collision checks; changing a curve does not
alter its input. Step sampling ignores stored tangents, and explicit tangent
edits on step curves refuse instead of reporting an ineffective edit.

This is a curve primitive, not an IK/FK authoring transaction or evidence of
a physical switch, runtime state, executed motion, or observation. The
`TemporalChannel` format is separate and does not acquire step semantics from
this change. Scenario/branch/session/restore lineage, control-axis target and
frame binding, temporal-intent admission, durable curve storage, native
timeline and downstream runtime mapping remain separate work. In particular,
exact numerical tick ordering does not prove real-world timing or execution.

`tests/motion_step_curve_tests.cpp` contains focused source cases for endpoint
clamping, negative and extreme ticks, exact step boundaries, v1 compatibility,
version/interpolation fencing, locale independence, malformed input,
digest-bound key insertion, tangent refusal and atomic collision refusal.
These tests were authored but not run in this lane;
the active build-coordinator lease remains in force. The test is registered as
`cartographer_motion_step_curve_tests`; the integrating worker must execute it
and the existing motion/migration suites on
the coordinated candidate, followed by native and runtime acceptance as
appropriate.
