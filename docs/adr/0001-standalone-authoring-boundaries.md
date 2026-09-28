# ADR-0001: Standalone authoring boundaries

## Status

Accepted for the 0.1 foundation.

## Decision

Cartographer is a standalone C++23 application/library family. Authoring
truth lives in scene, project, and geometry layers. Rendering, interchange,
UI, and future simulation consume validated derived or exported data and do not
become alternate sources of truth.

The public repository is built without private VANTA targets and without
RegOS, IDA, HAVEN, or defense-application dependencies. Generic behavior is
reimplemented under Cartographer namespaces. Any future compatibility adapter
must remain outside the core public target graph and carry its own provenance
review.

## Pressure and rejected alternatives

- Straight-copying VANTA would shorten the first implementation but would risk
  private IP leakage and semantic coupling to a non-public revision model.
- Making Vulkan or the desktop shell mandatory would prevent headless geometry
  tests and make a missing SDK look like a modeling capability.
- Storing compiled render data as project truth would make stale cache output
  indistinguishable from authoring state.

## Consequence

The 0.1 build is useful without a GPU or window system. The cost is that the
desktop shell and Vulkan backend must later be added as explicit layers rather
than hidden assumptions.

