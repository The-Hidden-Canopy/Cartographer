# Agent goal compilation proposal

**Status: draft for owner review.** This document defines a proposed boundary
for broader objective compilation and clarification generation. It is not an
approved implementation plan and does not change runtime behavior.

## Current boundary

The goal editor compiles an explicit sequence from the registered native
command grammar into a bounded, review-only plan. It rejects unsupported
clauses and preserves the existing manual draft on failure. The workbench can
rank declared clarification questions, but it does not infer general objective
meaning or generate questions from free-form goals.

## Proposed behavior

1. Parse a free-form objective into a **candidate objective record** containing
   only registered action types, typed parameters, explicit targets, hard
   constraints, evidence references, unresolved choices, and provenance back to
   the objective text. The record is a draft, not an executable command.
2. Resolve targets and facts only through the existing revision-bound
   `SceneQuery` vocabulary and current reviewed evidence. Missing, stale,
   incomplete, or conflicting evidence remains explicit and blocks any
   dependent plan branch. It must never be replaced with defaults or fallback
   values presented as current facts.
3. Generate a clarification only when an unresolved choice changes a material
   outcome, scope, constraint, or plan dependency. Each question must list its
   consequence categories, affected objects, downstream branches, and the
   evidence that made the choice unresolved. Rank questions deterministically
   by consequence severity, affected-object count, downstream-branch count, and
   stable question ID. Do not collapse conflicting interpretations into one
   answer.
4. Bound compilation by explicit limits for parsing work, candidate branches,
   questions, plan steps, query usage, and serialized output. Reaching any
   limit produces a diagnostic and no partial candidate plan.
5. Compile only to registered native capabilities already allowed by the
   operator's work envelope. The compiler cannot add targets, effects,
   permissions, or authority. It cannot create new capability registrations,
   relax hard constraints, or invoke mutation APIs.
6. Bind each answer receipt to the objective digest, target scope, source and
   query revisions, question context, and candidate-plan digest. A changed
   answer, source revision, target set, constraint, or plan invalidates affected
   receipts and requires a fresh operator decision.
7. Keep compilation and clarification review-only. The existing application
   boundary remains responsible for explicit plan acceptance, source revision
   checks, validation, journaling, and mutation. No model-generated answer can
   accept or execute a plan.

## Required failure behavior

- Unsupported intent, ambiguous target, missing constraint, stale evidence,
  conflicting interpretations, malformed output, or exhausted budget must
  fail closed with a diagnostic.
- Failure must preserve the prior manual or compiled draft and must not mutate
  the source project, task state, work envelope, journal, or accepted plan.
- An answer may choose among presented, in-scope candidates or provide an
  operator-authored value that passes the existing native validators. It may
  not broaden scope or weaken constraints.
- Recompilation after relevant source or objective changes must invalidate
  stale candidate plans and receipts rather than silently reusing them.

## Acceptance tests required before implementation

- Registered, unambiguous objective compiles to a bounded review-only plan and
  leaves the source project unchanged.
- Unsupported action, extra instruction, conflicting constraints, and malformed
  model output fail atomically and preserve the prior draft.
- Cross-target and out-of-scope candidate expansion is refused; work-envelope
  effects and hard constraints cannot be widened by generated text or answers.
- Missing, stale, unreviewed, and conflicting source evidence is surfaced and
  blocks dependent planning without live-looking fallback data.
- Ambiguous objectives produce deterministic, provenance-backed questions;
  minority/conflicting interpretations remain visible.
- Question and plan budgets at the boundary pass; one-over-budget inputs fail
  without partial state.
- Answer receipts are invalidated by changes to the objective, evidence
  revision, target scope, constraints, or ordered plan.
- Only the human acceptance path can produce the existing journaled mutation;
  rejection, cancellation, and compilation failure produce none.

## Decision needed

Owner agreement is required on the proposed behavior and test boundary before
implementation. Once agreed, split implementation into a parser/candidate
record slice and a clarification-generation slice; keep each independently
reviewable and preserve the existing explicit-command path.
