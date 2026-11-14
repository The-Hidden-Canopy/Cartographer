# Scene-linear float color and capture

`cartographer_scene_render` can write a `.pfm` capture as well as `.bmp`.
PFM contains scene-linear RGB, IEEE-754 binary32, little-endian, bottom row
first, with unit scale; alpha is omitted. Values above 1 are preserved before
exposure, tone mapping and FXAA. The receipt includes the output digest. BMP
remains a tone-mapped 8-bit sRGB preview.

Strict RGB `.pfm` sources can be imported as content-addressed project textures
and bound to base-color or emissive slots. The source is interpreted as
scene-linear Rec.709 RGB because PFM contains no color-profile metadata. The
caller must provide that interpretation. Float mips are filtered in linear
light and uploaded as RGBA32F, not converted through an 8-bit sRGB path. PFM
carries no alpha here: its alpha is opaque. RGB values above 1 are preserved,
including for base color; such base colors are an artistic HDR choice and do
not represent energy-conserving reflectance. Negative and nonfinite material
values are rejected. Normal, metallic/roughness, and occlusion slots do not
accept float PFM in this path.

For a saved RGBA8 texture, `--hdr-base-color ID file.pfm` or `--hdr-emissive
ID file.pfm` can supply a per-capture float override for the corresponding slot.
The receipt binds source and mip digests. Saved PFM sources should be reimported
instead of overridden. Source identity and semantic are part of the render and
GPU cache identity.

The native project keeps its source blob and regenerates float mips after reload;
this is not an HDR display signal. The glTF writer refuses PFM material textures
rather than silently quantizing them to its RGBA8 PNG path. It supports the
`KHR_materials_emissive_strength` extension for representable emissive factors,
but recipient extension support varies and is not established by this feature.
EXR inspection does not imply pixel decoding or a particular color space.
