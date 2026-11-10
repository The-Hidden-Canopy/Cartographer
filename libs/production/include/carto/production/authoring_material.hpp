#pragma once

#include <carto/assets/texture_mip.hpp>
#include <carto/project/material_catalog.hpp>

#include <memory>
#include <optional>

namespace carto::production {

[[nodiscard]] assets::TextureSourceLimits authoring_texture_limits();

// Disposable source-pixel/mip cache. Authoritative texture records stay in the
// catalog. Failed admission leaves the previous good source and programs
// intact.
class AuthoringTextureLibrary final {
  public:
    AuthoringTextureLibrary();
    ~AuthoringTextureLibrary();
    [[nodiscard]] core::Result<void> admit(const project::TextureSourceRecord &record,
                                           std::span<const std::uint8_t> encoded);
    [[nodiscard]] core::Result<std::shared_ptr<const assets::Rgba8MipChain>>
    prepare(const project::TextureSourceRecord &record, assets::TextureSemantic semantic,
            assets::ColorSpace color_space) const;
    [[nodiscard]] std::uint64_t resident_bytes() const;
    // Copies admitted records and decoded pixels atomically into a separate
    // library. Derived mip caches are not shared or copied; the snapshot
    // lazily builds its own under the same per-library budgets. No mutable
    // alias to the returned library escapes this operation.
    [[nodiscard]] core::Result<std::shared_ptr<const AuthoringTextureLibrary>> snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AuthoringSurfaceSample {
    core::Vec2d uv, uv_dx, uv_dy;
};
struct AuthoringMaterialSample {
    render::StandardMaterial material;
    core::Vec3d tangent_normal{0, 0, 1};
    bool normal_mapped = false;
    double ambient_occlusion = 1;
};

// Public, disposable shader-facing packet. No runtime handles or project truth.
// RGBA32F words: 4 header texels, 4 per standard-map slot, then 3 per
// instruction (opcode/type/parameter/map, dst/a/b/c, constant). Padded to 32
// texels per row. The bounded native interpreter uses 32 live registers and
// at most 128 instructions; unsupported register pressure fails before upload.
struct AuthoringMaterialGpuPacket {
    render::StandardMaterial material;
    std::array<std::shared_ptr<const assets::Rgba8MipChain>, 5> maps;
    std::vector<std::array<float, 4>> words;
    assets::Sha256Digest digest;
};

// Bounded executable CPU reference, not a shader-compilation receipt. One
// explicit UV set per program; all graph/material bindings must agree. Texture
// graph nodes use a matching authored material binding's sampler and semantics;
// ambiguous or missing bindings fail. uv.u/uv.v read that set (UV0 if no maps).
class AuthoringMaterialProgram final {
  public:
    AuthoringMaterialProgram() = default;
    [[nodiscard]] core::Result<AuthoringMaterialSample>
    evaluate(const AuthoringSurfaceSample &surface) const;
    [[nodiscard]] std::optional<render::UvSetId> uv_set() const;
    [[nodiscard]] bool normal_mapping() const;
    [[nodiscard]] bool per_fragment() const;
    [[nodiscard]] std::uint64_t texture_bytes() const;
    [[nodiscard]] core::Result<AuthoringMaterialGpuPacket> gpu_packet() const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit AuthoringMaterialProgram(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    friend core::Result<AuthoringMaterialProgram>
    compile_authoring_material(const project::MaterialCatalog &, render::MaterialId,
                               std::optional<project::MaterialVariantId>,
                               const AuthoringTextureLibrary *);
};
[[nodiscard]] core::Result<AuthoringMaterialProgram>
compile_authoring_material(const project::MaterialCatalog &catalog, render::MaterialId material,
                           std::optional<project::MaterialVariantId> variant = std::nullopt,
                           const AuthoringTextureLibrary *textures = nullptr);

// Constant-only compatibility entry. Texture/UV-dependent graphs fail here;
// callers requiring surface evaluation use compile_authoring_material instead.
[[nodiscard]] core::Result<render::StandardMaterial>
resolve_authoring_material(const project::MaterialCatalog &catalog, render::MaterialId material,
                           std::optional<project::MaterialVariantId> variant = std::nullopt);

} // namespace carto::production
