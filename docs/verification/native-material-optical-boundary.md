# Native material optical boundary

The legacy native material constants and compiled material artifact carry the
standard PBR and clearcoat fields, but have no representation for Material V5
IOR, transmission, thickness, attenuation, anisotropy, their rotation, or their
texture maps. Previously, compilation included non-default V5 fields in the
source digest while omitting them from the constants. A matching digest did not
mean that the prepared material retained those fields.

The native packer now returns an unsupported diagnostic for non-default V5
optics. Source revalidation applies the same rule to artifacts compiled before
this check. Default optical values and the existing clearcoat path remain
supported. The authoring viewport uses its separate material packet; this guard
does not add optical support to the legacy native artifact or prepared draw
package. In particular, preparing a glass or anisotropic material for this
legacy path now fails explicitly instead of producing an opaque or isotropic
substitute.

An old artifact consumed without access to its authoritative material source
cannot reveal whether an earlier compiler omitted optical data: its digest
alone is insufficient. Consumers of old publications need source-aware
revalidation or regeneration. A versioned native artifact and corresponding
runtime bindings are still needed to support these materials end to end.

The material artifact regression checks each scalar and optical map, exercises
the default/clearcoat happy path and save/reload, and models an old artifact
whose source digest includes omitted transmission. The preparation-operation,
lookdev and UV material contract tests check supported behavior. These are not
cross-engine visual acceptance or proof of completed glass rendering.
