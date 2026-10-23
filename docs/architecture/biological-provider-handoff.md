# Biological provider handoff contract

The handoff is an artifact boundary, not an authoring command. A request uses
`BiologicalProviderRequest` to bind the operation, input/configuration digests,
and exact `AuthoringSourceIdentity`. Its canonical digest is the request
identity returned by the provider.

A response carries `BiologicalProviderResult`: the exact source identity, the
request digest, an output artifact digest, and the declared artifact byte
count. The caller validates in this order:

1. `validate_biological_provider_result()` checks that the request still names
   the current source revision and digest, that the result names the same
   source and request, and that the declared output size fits the caller and
   hard budgets.
2. After retrieving the artifact by its content digest, the caller passes its
   bytes to `validate_biological_provider_artifact()`. It must match both the
   declared length and SHA-256 digest.
3. A consumer must parse the artifact under a named, bounded schema and
   preserve unsupported content as a refusal or review proposal. This handoff
   contract does not define a provider payload schema.
4. Applying reviewed records requires the existing human-only authoring
   command and normal project transaction/history path. A provider response
   cannot mutate project or scene state.

An unavailable provider, stale source, mismatched request, malformed result,
missing artifact, digest/length mismatch, or budget overflow is a refusal. The
handoff carries no endpoint, credential, organization authority, runtime
handle, or permission to publish. It does not assert that an artifact is
biologically accurate or accepted by an external consumer.

The CPU contract test `cartographer_biological_provider_contract_tests` covers
the valid path, stale revision/digest, request/result mismatch, malformed
operation, output budgets, and fetched-byte digest/length mismatch. Provider
execution, rendered field visualization, payload-schema parsing, and external
consumer acceptance remain separate tasks.
