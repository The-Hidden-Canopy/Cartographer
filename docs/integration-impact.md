# Cross-repository impact trace

Cartographer is intentionally standalone. The following is the impact of this
repository on the four named Hidden Canopy systems; it is not a claim that any
integration exists.

## Direct impact

None. Cartographer adds no import, build target, API call, schema migration, or
runtime dependency to RegOS, IDA_Training, hidden_canopy_bot, or Ask_IDA.

## Indirect impact

Future neutral interchange or project/mesh exports could become input fixtures
for other systems, but the current repository emits only local `.carto` and
OBJ artifacts. No dataset, training run, capability score, or promotion status
is produced.

## Shared contract touched

Only the general engineering boundary is shared: standalone builds, explicit
provenance, failure-visible diagnostics, and separation of authoritative data
from derived output. Cartographer does not adopt RegOS state transitions,
permissions, audit ledgers, or domain-event schemas because it has no governed
organizational mutation surface.

## API/schema, dataset/eval, and deployment risk

- API/schema risk: future consumers must version the `.carto` format and treat
  OBJ warnings as feature-loss evidence; no consumer contract exists yet.
- Dataset/eval risk: no training or evaluation data is generated. A rendered
  image or mesh export must not be promoted to model or capability evidence
  without a separate provenance decision.
- Deployment risk: the current CLI is offline and headless. It does not prove
  desktop packaging, GPU availability, runtime DLL delivery, or cloud
  deployment.

## Required follow-up before integration

Add an adapter-specific contract, fixture provenance, version negotiation,
round-trip/loss tests, and an independent consumer acceptance run. Do not
import Cartographer internals into any of the four systems.

