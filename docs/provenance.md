# Provenance and open-source boundary

## Source specification

The repository was created from the user-provided 143-page
`Cartographer_Engineering_Specification.pdf`, preserved at
`docs/spec/Cartographer_Engineering_Specification.pdf`.

Recorded source digest:

```text
SHA-256 008C7F5144E7A6E14EAB3D975521D3387563A7E49AAA910A580F981F242A0E8D
```

The specification describes inspected capabilities in private VANTA,
Open-World-Model-Harness, ForgeOfFate, and related repositories. Those
references are treated as architecture evidence and downstream validation
context only. This 0.1 implementation was written as a standalone
reimplementation and does not link or include those repositories.

## Public boundary rules

- No `vanta_*` namespace, private target, private header, model runtime, trace
  pipeline, defense/governance service, or credential is part of the public
  target graph.
- Public code keeps its own `carto_*` namespaces and tests generic behavior.
- Future extraction or compatibility work requires a source map, provenance
  review, declassification decision, and fresh-repository build before it can
  be represented as public code.
- The repository does not claim full compatibility with VANTA merely because
  its generic concepts are similar. It does provide an explicit, bounded glTF
  export profile shaped to VANTA's documented public mesh-import subset; that
  profile is an interchange contract, not a claim of VANTA runtime, material,
  or visual parity.

## Evidence classes

The implementation distinguishes authoring truth from derived data in types and
documentation. A successful build or a compiled mesh proves only the tested
local capability. It is not evidence of Vulkan execution, desktop launch,
performance at production scale, CAD semantics, or downstream engine parity.
