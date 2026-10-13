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

## Follow-up coverage

Add failure injection for durable journal append rollback, saved/reopened task state, unrelated and conflicting source edits, bounded repair exhaustion, query budgets, and provider timeouts as those tranches are implemented. Do not report task persistence, automatic rebase, model planning, or viewport ghosting as supported by this slice.
