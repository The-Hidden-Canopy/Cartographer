# Agent Demonstration Recipe v1

An Agent Demonstration Recipe is a named container around an
`AgentDemonstrationTrace` v2. It can be inspected and rebound to current scene
targets; it is not an executable plan or an authorization record. Trace v1
remains readable but does not carry correspondence evidence.

## Encoding

- Magic: `CARTOGRAPHER_AGENT_DEMONSTRATION_RECIPE_V1\n`
- Payload version: unsigned integer `1`
- Name: length-prefixed text, 1–128 bytes; control bytes below `0x20` and
  `0x7f` are rejected
- Trace: length-prefixed serialized Agent Demonstration Trace v2, at most 1 MiB
- Integrity: an outer SHA-256 digest covers the recipe payload; the embedded
  trace has its own checksum
- Maximum complete file size: 2 MiB

The format stores no scene object or asset IDs, component IDs, project bytes,
target bindings, filesystem paths, task authority, acceptance, prompts, raw
geometry, coordinates, transforms, or validation claims. Trace v2 may include
SHA-256 structural signatures derived from mesh counts, face arity, and local
incidence. The signatures contain no scene IDs or names, but may still be
matched against known structural inputs. They are not a privacy guarantee or
proof of authorship. The user-selected recipe name is retained.

Saving is immutable and fails if the destination already exists. Loading does
not mutate a project or create an AgentTask. “Apply to similar” supports one
captured command with structural-signature evidence: select the completed exemplar
object, and the workbench finds other visible, unlocked objects with one unique
signature match. Ambiguous objects are skipped; face and vertex transfers also
skip scene instances that share a mesh asset. It does not infer semantic,
prefab, material, socket, or spatial-role correspondence. Applying any recipe
still requires a bounded speculative preview, review of its gates and diff,
and separate human acceptance.
