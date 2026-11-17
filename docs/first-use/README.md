# Cartographer alpha — first-use package

> Historical handoff from 2026-10-28, recovered during worktree consolidation. Workstation paths use placeholders. UI labels, source line references and unimplemented proposals describe that dated candidate. They are not verification of the current application. See [the current texturing verification](../verification/texturing-workflow.md) and [integration ledger](../verification/worktree-integration-20261116.md).


Everything a first-time alpha user needs to go from a blank viewport to a saved,
material-assigned project and a clean OBJ export. Start with the
[startup guide](startup-guide.md); keep the other documents as reference while
you work.

## Read in this order

1. **[Startup guide](startup-guide.md)** — the guided first-session walkthrough:
   launch the Vulkan workbench, import the bundled example, create and assign a
   material, save, reopen, and export.
2. **[First Block example](../../examples/first-block/README.md)** — the small
   practice asset the walkthrough uses (one block, region 1 on every face, no
   material), with rebuild instructions.

## Reference while you work

- **[Controls](controls.md)** — every shortcut, mouse binding, and menu label,
  with the code lines each was verified against.
- **[Import/export](import-export.md)** — what the app and the CLI import and
  emit, the clean-save export gate, and what each format loses.
- **[Troubleshooting and recovery](troubleshooting.md)** — exact error strings
  mapped to fixes, file locations on disk, and the journal-snapshot recovery
  procedure.

## For integrators

- **[In-app help content](in-app-help.md)** — ready-to-wire help copy with
  stable topic IDs. **None of it ships in the app yet**: there is no Help menu,
  no F1 route, and no onboarding surface in this build.
- **[Sol integration handoff](sol-integration-handoff.md)** — where the help
  surfaces must be wired in the desktop shell, the required tests, and the
  candidate acceptance sequence.

## Verification status

- **Content and example: complete.** This package and the First Block example
  were authored against source at `fae4f7d95259ba38e79edc94ae12275df4161cd4`.
- **UI wiring: a handoff, not shipped.** The in-app help copy and integration
  plan above are proposals; nothing has been wired into the shell.
- **Candidate native journey: untested.** No candidate build of `fae4f7d` was
  launched; no GUI step in this package is runtime-accepted. Every UI claim is
  code-derived and labeled as such.
- **Pre-existing CLI headless checks: passed.** The example was generated and
  validated with the older, pre-existing CLI 0.1.0 (schema 34), which is not a
  candidate GUI acceptance result. See the
  [evidence checklist](evidence.md) and
  [headless example validation](example-validation.md) for exactly what was run
  and what remains open.

## Known gaps

- **[Alpha blockers](alpha-blockers.md)** — release-blocking wiring and workflow
  defects (no in-app Help/onboarding, Focus flow hides creation, primitives have
  no material region, no save-and-close choice, recovery not communicated, no
  end-to-end candidate acceptance) plus the observed-pass release checklist.
