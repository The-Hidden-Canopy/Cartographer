# Agent Workbench verification contract

## End-to-end fixture

- Create a project with separate mesh objects.
- Capture exact face targets and queue native inset/extrude steps.
- Run the plan on the speculative session.
- Confirm the source revision and geometry remain unchanged before review.
- Confirm per-step receipts, project/viewport gates, and structured mesh diffs.
- Accept the result and confirm the source advances once and the saved-project journal contains one application mutation event.

## Adversarial boundaries

- A target outside objective scope is rejected before execution.
- A protected target is rejected before execution.
- A target mesh shared by an out-of-scope or protected instance is rejected, and all in-scope instances count toward the touched-object budget.
- A topology edit is blocked when the envelope permits geometry only.
- A plan based on a stale source revision cannot start.
- A missing goal cannot start a task, and a source edit after speculative execution prevents acceptance without overwriting the newer source.
- An invalid native operation blocks the task without changing the source.
- Acceptance before gates pass, repeated execution, or rejection after acceptance is invalid.
- The UI captures only a single exact face or vertex per step and exposes explicit accept/reject actions.
- Distinct geometry results produce distinct digests even when their mesh counts and revision shape match.

## SceneQuery coverage

- Exact object ID, stable name, and mesh-asset queries return copied identities at the source revision.
- Name, spatial, topology, and CONDITION results are limited to the task scope; an explicit out-of-scope identity request fails closed.
- Proximity results report world object-origin distance; orientation results report computed world-space normals; boundary-edge results come from validated mesh topology.
- Missing CONDITION sidecars return an explicit unavailable diagnostic, and CONDITION data with a stale project revision is rejected instead of being shown as current.
- Query count, object scans, topology elements, semantic records, result count, and result-byte limits truncate or reject deterministically without mutating the source.
- Invalid query parameters fail before consuming query budget.

## Capability registry coverage

- Every known native mesh tool has a complete typed contract; kernel/ID drift remains unclassified and cannot be provider-requestable.
- Parameter names match the native tool, numeric bounds are finite and coherent, and length/factor/count units are explicit.
- Geometry-only envelopes expose vertex positioning while excluding topology-changing previews; unsupported registered tools never appear in the planner projection.
- The projected context contains no command callback, project document, or direct mutation handle.

## Follow-up coverage

Add failure injection for durable journal append rollback, saved/reopened task state, unrelated and conflicting source edits, bounded repair exhaustion, semantic-sidecar size pressure, and provider timeouts as those tranches are implemented. Do not report task persistence, automatic rebase, model planning, or viewport ghosting as supported by this slice.
