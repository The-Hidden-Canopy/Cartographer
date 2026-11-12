#include <carto/production/material_artifact.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::render::MaterialAsset material_fixture() {
    carto::render::StandardMaterial definition;
    definition.base_color_factor = {0.65, 0.2, 0.08, 1.0};
    definition.metallic = 0.35;
    definition.roughness = 0.28;
    definition.normal_scale = 0.8;
    definition.occlusion_strength = 0.9;
    definition.emissive_factor = {0.05, 0.02, 0.01};
    definition.emissive_strength = 2.0;
    definition.dielectric_f0 = 0.06;
    definition.clearcoat = 0.7;
    definition.clearcoat_roughness = 0.16;
    definition.base_color.asset = 1001U;
    definition.metallic_roughness.asset = 1002U;
    definition.normal.asset = 1003U;
    definition.occlusion.asset = 1004U;
    definition.emissive.asset = 1005U;
    return {71U, "fixture.wet-painted-metal", definition, carto::core::Revision{9U}};
}

void supported_material_compiles_deterministically() {
    using namespace carto::production;
    const auto material = material_fixture();
    const auto first = compile_material_artifact(
        "project/public-fixture/material/71", material);
    const auto second = compile_material_artifact(
        "project/public-fixture/material/71", material);
    require(first && second, "supported Material V2 source compiles");
    require(first.value().artifact_digest == second.value().artifact_digest &&
                first.value().source_digest == second.value().source_digest,
            "material compilation is deterministic");
    require(first.value().feature_mask ==
                (kMaterialFeatureStandardPbr |
                 kMaterialFeatureDielectricF0 |
                 kMaterialFeatureClearcoat),
            "compiled material declares exactly its active supported feature set");
    require(first.value().gpu_constants.emissive_radiance_and_f0[0U] == 0.1F &&
                first.value().gpu_constants.emissive_radiance_and_f0[3U] == 0.06F &&
                first.value().gpu_constants.clearcoat_factors[0U] == 0.7F &&
                first.value().resident_bytes() ==
                    sizeof(carto::render::GpuMaterialConstants),
            "compiled material carries bounded native constants and resident bytes");
    require(static_cast<bool>(first.value().validate()),
            "compiled material artifact validates");
    require(static_cast<bool>(validate_material_artifact_source(
                first.value(), "project/public-fixture/material/71", material)),
            "current authoritative material admits its compiled artifact");
}

void material_artifact_round_trip_and_tamper_rejection() {
    using namespace carto::production;
    const auto compiled = compile_material_artifact(
        "project/public-fixture/material/71", material_fixture());
    require(static_cast<bool>(compiled), "round-trip material compiles");
    const auto serialized = serialize_material_artifact(compiled.value());
    require(static_cast<bool>(serialized), "compiled material serializes");
    const auto reopened = deserialize_material_artifact(serialized.value());
    require(reopened &&
                reopened.value().artifact_digest == compiled.value().artifact_digest &&
                reopened.value().source_digest == compiled.value().source_digest &&
                reopened.value().gpu_constants.texture_asset_ids ==
                    compiled.value().gpu_constants.texture_asset_ids,
            "compiled material survives digest-verified save/reopen");

    auto tampered = serialized.value();
    tampered[tampered.size() / 2U] ^= 1U;
    require(!deserialize_material_artifact(tampered),
            "material artifact rejects tampered constants or metadata");
    auto truncated = serialized.value();
    truncated.pop_back();
    require(!deserialize_material_artifact(truncated),
            "material artifact rejects a truncated digest");
    auto trailing = serialized.value();
    trailing.push_back(0U);
    require(!deserialize_material_artifact(trailing),
            "material artifact rejects trailing unbound bytes");
    auto wrong_magic = serialized.value();
    wrong_magic.front() = 'X';
    require(!deserialize_material_artifact(wrong_magic),
            "material artifact rejects unknown framing");

    auto mutated = compiled.value();
    mutated.gpu_constants.surface_factors[1U] = 0.9F;
    require(!mutated.validate(),
            "in-memory mutation invalidates material artifact evidence");
    mutated = compiled.value();
    mutated.feature_mask |= 1U << 31U;
    require(!mutated.refresh_digest(),
            "unknown material features cannot be legitimized by refreshing a digest");
}

void stale_and_unsupported_sources_fail_closed() {
    using namespace carto::production;
    auto material = material_fixture();
    const auto compiled = compile_material_artifact(
        "project/public-fixture/material/71", material);
    require(static_cast<bool>(compiled), "stale-source fixture compiles");

    auto stale_revision = material;
    stale_revision.revision = carto::core::Revision{10U};
    const auto revision_result = validate_material_artifact_source(
        compiled.value(), "project/public-fixture/material/71", stale_revision);
    require(!revision_result &&
                revision_result.error().code == carto::core::ErrorCode::stale_data,
            "material artifact rejects a stale source revision");

    auto stale_content = material;
    stale_content.definition.roughness = 0.8;
    const auto content_result = validate_material_artifact_source(
        compiled.value(), "project/public-fixture/material/71", stale_content);
    require(!content_result &&
                content_result.error().code == carto::core::ErrorCode::stale_data,
            "material artifact rejects changed content at a reused revision");
    require(!validate_material_artifact_source(
                compiled.value(), "project/other/material/71", material),
            "material artifact rejects a source-identity swap");

    auto unsupported = material;
    unsupported.definition.base_color.transform.offset = {0.5, 0.0};
    require(static_cast<bool>(unsupported.validate()),
            "authored transformed texture binding remains valid project data");
    const auto unsupported_compile = compile_material_artifact(
        "project/public-fixture/material/71", unsupported);
    require(!unsupported_compile &&
                unsupported_compile.error().code == carto::core::ErrorCode::unsupported,
            "native compiler visibly rejects unsupported texture transforms");

    auto invalid = material;
    invalid.definition.clearcoat = std::numeric_limits<double>::quiet_NaN();
    require(!compile_material_artifact("project/public-fixture/material/71", invalid),
            "non-finite material source cannot compile");
    require(!compile_material_artifact("../private/material/71", material),
            "unsafe material source identity is rejected");
}

// Imported glTF bindings: an explicit catalog sampler with default parameters
// and an OpenGL tag on non-normal slots. Only provably default samplers pack;
// identity stays bound to the authored material and normal maps stay strict.
void imported_default_sampler_bindings_pack_without_changing_identity() {
    using namespace carto::production;
    auto material = material_fixture();
    for (carto::render::TextureRef* binding : {
             &material.definition.base_color, &material.definition.metallic_roughness,
             &material.definition.occlusion, &material.definition.emissive}) {
        binding->sampler = 5U;
        binding->normal_convention = carto::render::NormalConvention::opengl_y_plus;
    }
    material.definition.normal.sampler = 5U;
    const DefaultSamplerEquivalence sampler_five_is_default = [](carto::render::SamplerId id) {
        return id == 5U;
    };
    require(!compile_material_artifact("project/public-fixture/material/71", material),
            "without equivalence evidence a non-zero sampler stays unsupported");
    const auto packed = compile_material_artifact(
        "project/public-fixture/material/71", material, sampler_five_is_default);
    require(static_cast<bool>(packed), "a default-equivalent sampler packs as the reserved default sampler");
    const auto authored_digest = material_source_digest(material);
    require(authored_digest && packed.value().source_digest == authored_digest.value() &&
                packed.value().source_revision == material.revision &&
                static_cast<bool>(validate_material_artifact_source(
                    packed.value(), "project/public-fixture/material/71", material)),
            "packed artifact still binds the authored material's digest and revision");
    require(packed.value().gpu_constants.texture_asset_ids[0U] == 1001U,
            "packed artifact keeps the authored base-color texture asset");
    require(packed.value().gpu_constants.alpha_factors[2U] == 0.0F,
            "a DirectX normal map keeps the DirectX decode flag");
    require(!compile_material_artifact("project/public-fixture/material/71", material,
                [](carto::render::SamplerId) { return false; }),
            "a sampler not proven default stays unsupported");
    auto opengl_normal = material;
    opengl_normal.definition.normal.normal_convention = carto::render::NormalConvention::opengl_y_plus;
    require(!compile_material_artifact("project/public-fixture/material/71", opengl_normal,
                sampler_five_is_default),
            "an OpenGL normal map stays unsupported by generic packing");
}

} // namespace

int main() {
    supported_material_compiles_deterministically();
    material_artifact_round_trip_and_tamper_rejection();
    stale_and_unsupported_sources_fail_closed();
    imported_default_sampler_bindings_pack_without_changing_identity();
    std::cout << "material artifact tests passed\n";
    return EXIT_SUCCESS;
}
