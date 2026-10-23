# Texture channel packing

`pack_rgba8_channels()` is a bounded derived-product compiler for four
explicit destination channels in R, G, B, A order. Each recipe input names one
typed source channel and exact TGA bytes. The current profile accepts
uncompressed true-color TGA only, decodes through the existing RGBA8 path,
requires identical dimensions, and binds each input's SHA-256 source receipt
and channel selection into the output digest.

Alpha can be selected only when the source TGA declares an authored 8-bit alpha
channel. The decoder's opaque fill for RGB TGA data cannot be treated as source
alpha. Unknown channel values, malformed or trailing input bytes, mismatched
dimensions, unsupported compression, invalid limits, and source/output budget
overruns fail closed. The fixed output order is explicit in the recipe array.

The artifact is derived RGBA8 pixel data, not a mutable source texture or a
claim about material semantics. The API does not infer a packing recipe,
resample mismatched sources, or publish into a project. Artifact validation
re-decodes the exact recipe inputs and compares the deterministic receipt.
