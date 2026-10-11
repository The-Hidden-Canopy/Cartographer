# Public proposal boundary

Cartographer's public AI-shaped surface is a provider-neutral proposal seam,
not a model runtime. A local or Hub-side provider may produce a bounded
proposal, study, or export request, but Cartographer owns revision checking,
preview, geometry validation, and final authoring application.

```text
revision-bound context
    -> typed proposal
    -> local validation
    -> local preview
    -> explicit or policy-authorized local apply
```

The public seam does not contain a provider, model, prompt, weight, routing
policy, account authority, telemetry sink, or remote mutation path. A proposal
cannot carry a mutable project pointer and cannot advance a project revision by
itself. The current `carto_ai` implementation is deterministic and bounded;
it is not a trained model, autonomous agent, remote inference client, or
general natural-language interpreter.
