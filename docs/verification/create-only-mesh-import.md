# Create-only mesh import

## Invariant

CLI OBJ, PLY, STL, and glTF/GLB imports create a new project. They must not
replace an existing project, adopt an orphan journal, or overwrite the input
when the same path is supplied as output.

The shared OBJ/PLY/STL import path previously used ordinary Save for its
initial snapshot. An existing project without an application journal could
therefore be replaced. A synthetic OBJ reproduction confirmed a successful
exit with changed target bytes. The glTF path checked only the project file,
leaving an empty orphan journal unprotected.

`SaveProjectAction::require_new_target` now checks both directory entries
while holding the application's existing writer lease, before journal
preparation or project save. Existing files, directories, and dangling links
are refused. An inspection error also fails closed. All four CLI import paths
use this mode for their initial save. Normal Save and Save As retain their
existing behavior.

This coordinates cooperating Cartographer writers. It is not a filesystem
sandbox or protection against an unrelated process ignoring the writer lease.
The lock file may remain after refusal, as with other application saves.
No project schema or journal format changes are required.

## Regression coverage

- `cartographer_create_only_save_tests`: occupied unjournaled target,
  empty/nonempty orphan journals, directory target, conditional dangling-link
  fixture, unchanged unsaved session/history, fresh save/reopen, repeat
  refusal, ordinary Save, and two concurrent creators with exactly one winner.
- `cartographer_mesh_cli_import_tests`: actual OBJ/PLY/STL export/import/reopen
  and re-export, journaled and unjournaled target preservation, orphan journal
  refusal, input/output alias protection, and malformed-input refusal before
  project publication. Paths contain spaces.
- `cartographer_gltf_scene_cli_import_tests`: glTF and GLB orphan-journal
  refusal in addition to existing scene, source, rights, and target tests.

The fixtures are synthetic and use no external assets. Native UI, GPU,
destination-runtime acceptance, and publication to the repository are separate
gates.

The 2026-11-11 configured Release build and focused create-only/CLI tests
passed. The dangling-link fixture was skipped because Windows did not grant
symlink creation. No privilege or system-setting change was made, and this run
does not claim execution evidence for that conditional case.

The expanded headless aggregate passed **218/218 tests** in 172.09 seconds.
Native/D3D12/GPU execution, desktop chrome/input, shader execution/compilation
acceptance, and scene-render CLI tests were excluded. The configured gated
desktop target compiled and linked, but was not launched.
