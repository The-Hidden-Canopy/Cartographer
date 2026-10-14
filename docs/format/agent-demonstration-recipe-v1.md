# Agent Demonstration Recipe v1

An Agent Demonstration Recipe is a named container around an
`AgentDemonstrationTrace` v1. It can be inspected and rebound to current scene
targets; it is not an executable plan or an authorization record.

## Encoding

- Magic: `CARTOGRAPHER_AGENT_DEMONSTRATION_RECIPE_V1\n`
- Payload version: unsigned integer `1`
- Name: length-prefixed text, 1–128 bytes; control bytes below `0x20` and
  `0x7f` are rejected
- Trace: length-prefixed serialized Agent Demonstration Trace v1, at most 1 MiB
- Integrity: an outer SHA-256 digest covers the recipe payload; the embedded
  trace has its own checksum
- Maximum complete file size: 2 MiB

The format stores no scene object or asset IDs, component IDs, project bytes,
target bindings, filesystem paths, task authority, acceptance, prompts, or
validation claims. The user-selected name is retained. Digests detect
accidental corruption; they do not authenticate authorship.

Saving is immutable and fails if the destination already exists. Loading does
not mutate a project or create an AgentTask. Applying a recipe requires fresh
explicit target bindings, a bounded speculative preview, review of its gates
and diff, and human acceptance.
