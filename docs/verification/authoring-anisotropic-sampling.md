# Authoring anisotropic sampling boundary

The authoring material CPU reference now follows the native viewport's bounded
anisotropic sampler: magnification uses one tap and the authored magnification
filter; minification uses up to eight taps on the longer texture derivative,
with mip level based on the remaining footprint. This follows the local v5
worktree's sampling direction while retaining the viewport shader's separate
magnification boundary.

Both paths reject UVs and tap coordinates outside a size-dependent bound before
conversion to signed GPU texel coordinates. The bound reserves headroom for
bilinear neighbours and is stricter for large textures. This is a safety limit,
not support for arbitrarily large UV coordinates.

A numeric CPU regression covers an elongated footprint, the magnification
boundary, an oversized derivative, and a 4096-wide address overflow boundary.
The native D3D12 viewport regression also compares CPU and GPU pixels for an
elongated transformed texture footprint. Shader compilation and viewport tests
run against the rebuilt source. These synthetic checks do not measure general
texture quality, temporal stability, or the five-prop visual acceptance.
