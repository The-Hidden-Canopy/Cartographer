# Open-boundary edge selection

In Modify, switch to edge mode and select at least one edge on each mesh to inspect. **Select all open boundary edges** replaces each owner's edge selection with every edge incident to only one face. It works across the selected owners independently; a closed mesh contributes no edges. This is different from **Boundary**, which traverses from the selected boundary seed.

This is selection-only and does not repair holes or modify the project. The selection is ephemeral. It retains the existing mesh-revision and project-snapshot checks. Shared mesh instances are inspected as separate selected owners, without changing their shared asset.

Implementation status: source wiring is present but this increment has not been built or run, and native click-through acceptance has not been performed.
