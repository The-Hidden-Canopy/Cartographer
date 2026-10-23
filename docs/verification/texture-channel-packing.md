# Texture channel packing verification

`cartographer_texture_channel_pack_tests` verifies explicit source-to-output
channel mapping, deterministic output digests, and validation against exact
source bytes. Boundary cases cover unknown channel enums, dimension mismatch,
alpha requested from a source with no authored alpha, truncated and trailing
data, and a caller pixel budget below the source extent.

`cartographer_texture_source_tests` remains the lower-level TGA inspection and
decode regression suite. These tests cover only uncompressed TGA RGBA8 channel
packing; PNG/JPEG/DDS/EXR pixel decode, resampling, project UI integration, and
GPU texture upload are outside this capability.
