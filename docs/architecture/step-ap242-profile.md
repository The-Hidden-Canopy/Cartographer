# Public STEP/AP242 probe profile

## Status and scope

This document describes the current public `StepAp242Provider` boundary. It is
a bounded header probe, not a CAD importer. The provider currently recognizes
some Part 21 header syntax and classifies the declared schema name; it does not
return an assembly or produce engineering topology.

## Supported probe surface

The probe reads at most the first 64 KiB of the supplied bytes. It recognizes a
file only when the prefix begins with the Part 21 marker, contains a `HEADER`
section, and contains a syntactically bounded `FILE_SCHEMA` record before the
header's `ENDSEC`. The schema record must contain one quoted, non-empty value of
at most 128 bytes. Schema classification is case-insensitive and accepts names
whose normalized value begins with `AP242`.

The probe may report the provider identity and version, the extracted schema
name, and diagnostics. The `units` field remains empty. `recognized` means the
bounded header syntax and schema declaration were read; `supported` remains
false even for a recognized AP242 header.

## Entity support

No DATA-section entity instances are supported. The probe does not parse or
validate the DATA section, build a product or assembly graph, extract B-Rep or
tessellated geometry, bind persistent topology identities, read PMI, infer
units, or establish correspondence between display geometry and source
topology. These capabilities are outside the current profile; the provider
must not imply that any of them were imported.

`read_assembly` always returns `unsupported` in this public build. A recognized
header therefore remains probe evidence only and cannot be used as a geometry
source or as a successful import receipt.

## Evidence and extension gate

The implementation is in `libs/engineering/src/cad_provider.cpp`. The
`step_boundary_never_fakes_a_cad_import` test in
`tests/engineering_tests.cpp` covers recognized AP242 headers, non-AP242 and
lookalike schema names, markers outside the header, malformed and oversized
headers, and explicit refusal to read an assembly.

Any future non-empty entity allowlist needs a connected provider/kernel,
fixtures for each admitted entity family, persistent source identity and
topology correspondence, loss-aware reimport behavior, and tests showing that
unsupported or ambiguous data remains visible and blocks publication. Header
recognition alone does not satisfy that gate.
