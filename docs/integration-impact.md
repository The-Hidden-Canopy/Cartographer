# Cross-repository impact trace

Cartographer is intentionally standalone. The following is the impact of this
repository on the four named Hidden Canopy systems; it is not a claim that any
integration exists. The render, blob, and journal foundations remain local and
dependency-free.

## Direct impact

None. Cartographer adds no import, build target, API call, schema migration, or
runtime dependency to RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA.

## Indirect impact

Future neutral interchange or project/mesh exports could become input fixtures
for other systems. Cartographer now emits a bounded, derived glTF profile in
addition to local `.carto` and OBJ artifacts; it does not emit a VANTA project,
runtime state, or promotion receipt. No dataset, training run, capability
score, or promotion status is produced.

## Shared contract touched

Only the general engineering boundary is shared: standalone builds, explicit
provenance, failure-visible diagnostics, hash-checked lineage, and separation
of authoritative data from derived output. Cartographer does not adopt RegOS
state transitions, permissions, audit ledgers, or domain-event schemas because
it has no governed organizational mutation surface.

## API/schema, dataset/eval, and deployment risk

- API/schema risk: consumers must version the `.carto` format and the glTF
  profile separately, and treat the export warnings as feature-loss evidence;
  no direct VANTA project contract exists.
- Dataset/eval risk: no training or evaluation data is generated. A rendered
  image or mesh export must not be promoted to model or capability evidence
  without a separate provenance decision.
- Deployment risk: the default CLI and tests are offline and headless. The
  optional native source path does not prove desktop packaging, GPU
  availability, runtime DLL delivery, or cloud deployment until its SDK,
  pinned ImGui dependency, and Windows runtime are independently accepted.

## Required follow-up before integration

For any direct adapter, add adapter-specific version negotiation, fixture
provenance, round-trip/loss tests, and an independent consumer acceptance run.
Do not import Cartographer internals into any of the four systems. The current
glTF exporter keeps the artifact neutral and source-revision bound, but it does
not prove VANTA render parity.
