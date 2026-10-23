# Surface visibility closure reference

Cartographer now has an explicit CPU geometric visibility closure for comparing
the authored source surface with a constrained QEM candidate.

## Contract

`evaluate_surface_visibility_closure` builds a bounded set of deterministic
hemisphere directions per triangle, offsets the ray origin by an explicit
epsilon, and tests those rays against the rest of the same source or candidate
surface. Area-weighted visibility ratios are binned into equal-support
distributions and compared with the existing distribution metrics. The receipt
also records source/candidate average visibility and their absolute difference.

This is a geometric occlusion reference. It is useful for closing the gap
between a candidate and directional visibility evidence, but it is not a full
ambient-occlusion solver, SH fit, horizon map, PBR implementation, or downstream consumer
render path.

## Safety and bounds

Triangle count, direction count, histogram bins, ray epsilon, ray distance, and
metric iterations are explicit and capped. Stale source domains, invalid QEM
outputs, degenerate triangles, invalid options, non-finite receipts, and forged
digests fail closed. A candidate is never admitted or applied by this API.

The metric remains correlated evidence: visibility histograms and average
visibility derive from the same bounded rays and must not be double-counted as
independent quality votes.

## Next bounded layer

Add protected material/UV/hard-normal seam semantics, horizon/SH descriptors,
and dense-corpus calibration before changing the public closure compiler's
baseline or claiming renderer parity.
