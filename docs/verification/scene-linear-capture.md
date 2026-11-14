# Scene-linear float capture

`cartographer_scene_render` accepts a new `.pfm` output in addition to the
existing `.bmp`. PFM contains scene-linear RGB, IEEE-754 binary32, little-endian,
bottom scanline first, with unit scale. Alpha is not stored. Values above 1 are
preserved; exposure, display tone mapping, and FXAA do not change this buffer.
The adjacent receipt records the output format and SHA-256 digest. BMP remains
an 8-bit, tone-mapped sRGB preview and retains its existing `bmp_digest` field.

This is an offline scene-render capture, not an HDR display signal, and does
not make material texture inputs HDR: those still use RGBA8 mip chains. EXR
source inspection does not imply EXR pixel decoding. Radiance RGBE environment
lighting follows its existing separate HDR import path.

A bounded strict PFM decoder is staged as an independent float-image building
block. It accepts only RGB, little-endian unit-scale float PFM, rejects
nonfinite values and malformed lengths, and keeps values above one. It is not
yet connected to texture admission, persistence, material slots or the GPU.

The experimental CLI option `--hdr-emissive ID file.pfm` substitutes a strict
scene-linear float PFM for one already-bound emissive texture during capture.
It generates float mips and uploads RGBA32F; the receipt binds source and mip
digests. This is a per-capture override, not a saved project import or EXR
decoder. Other material slots continue to use the legacy RGBA8 path.
