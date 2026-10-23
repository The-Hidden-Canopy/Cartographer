#include <carto/condition/vegetation_preview.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double unit_sample(std::uint64_t seed, std::uint64_t stream) noexcept {
    const std::uint64_t bits = mix64(seed ^ mix64(stream));
    return static_cast<double>(bits >> 11U) * (1.0 / 9007199254740992.0);
}

double normalized_value(const spatial_field::ScalarField2D& field, double value) noexcept {
    const double range = field.maximum_value() - field.minimum_value();
    if (!std::isfinite(range) || range <= std::numeric_limits<double>::epsilon()) {
        return 0.0;
    }
    return std::clamp((value - field.minimum_value()) / range, 0.0, 1.0);
}

struct ResolvedField {
    const spatial_field::ScalarField2D* field = nullptr;
    VegetationFieldReceipt receipt;
};

core::Result<ResolvedField> resolve_field(
    const std::map<std::string, spatial_field::ScalarField2D>& fields,
    const FieldSourceBinding& binding,
    spatial_field::FieldSemantic semantic) {
    const auto found = fields.find(binding.field_identity);
    if (found == fields.end()) {
        return core::Result<ResolvedField>::failure(stale(
            "vegetation preview is missing bound field '" + binding.field_identity + "'"));
    }
    const auto& field = found->second;
    if (field.identity() != binding.field_identity || field.semantic() != semantic) {
        return core::Result<ResolvedField>::failure(stale(
            "vegetation preview field identity or semantic no longer matches its binding"));
    }
    if (auto result = field.validate(); !result) {
        return core::Result<ResolvedField>::failure(result.error().with_context(
            "vegetation field '" + binding.field_identity + "'"));
    }
    const auto digest = field.canonical_digest();
    if (!digest) return core::Result<ResolvedField>::failure(digest.error());
    if (field.source_revision() != binding.expected_field_revision ||
        digest.value() != binding.expected_field_digest) {
        return core::Result<ResolvedField>::failure(stale(
            "vegetation preview field evidence is stale for '" + binding.field_identity + "'"));
    }
    return core::Result<ResolvedField>::success(ResolvedField{
        &field,
        VegetationFieldReceipt{field.identity(), field.semantic(),
            field.source_revision(), digest.value()}});
}

std::string preview_canonical(const VegetationPreviewResult& preview) {
    std::ostringstream output;
    output << std::setprecision(17)
           << "CARTOGRAPHER_VEGETATION_PREVIEW 2\n"
           << preview.recipe_id << ' ' << preview.project_revision.value() << ' '
           << preview.authoring_digest.hex() << ' ' << preview.candidate_count << '\n';
    if (preview.plant_source.has_value()) {
        output << "PLANT_SOURCE " << std::quoted(preview.plant_source->identity) << ' '
               << preview.plant_source->source_revision.value() << ' '
               << preview.plant_source->digest.hex() << '\n';
    } else {
        output << "PLANT_SOURCE none\n";
    }
    for (const auto& field : preview.fields) {
        output << std::quoted(field.identity) << ' '
               << spatial_field::field_semantic_name(field.semantic) << ' '
               << field.source_revision.value() << ' ' << field.digest.hex() << '\n';
    }
    for (const auto& instance : preview.instances) {
        output << instance.stable_id << ' ' << instance.recipe_id << ' '
               << instance.species_asset_id << ' ' << instance.grid_u << ' '
               << instance.grid_v << ' ' << instance.position.x << ' '
               << instance.position.y << ' ' << instance.position.z << ' '
               << instance.yaw_radians << ' ' << instance.uniform_scale << ' '
               << instance.suitability << '\n';
    }
    return output.str();
}

} // namespace

core::Result<VegetationPreviewResult> preview_vegetation(
    const AuthoringDocument& authoring,
    const std::map<std::string, spatial_field::ScalarField2D>& fields,
    Id recipe_id) {
    static const std::map<std::string, VegetationPlantSourceReceipt> no_plant_sources;
    return preview_vegetation(authoring, fields, no_plant_sources, recipe_id);
}

core::Result<VegetationPreviewResult> preview_vegetation(
    const AuthoringDocument& authoring,
    const std::map<std::string, spatial_field::ScalarField2D>& fields,
    const std::map<std::string, VegetationPlantSourceReceipt>& plant_sources,
    Id recipe_id) {
    if (auto result = authoring.validate(); !result) {
        return core::Result<VegetationPreviewResult>::failure(result.error().with_context(
            "vegetation preview authoring"));
    }
    const auto recipe_found = std::find_if(authoring.vegetation_scatters.begin(),
        authoring.vegetation_scatters.end(), [recipe_id](const VegetationScatterRecipe& recipe) {
            return recipe.id == recipe_id;
        });
    if (recipe_found == authoring.vegetation_scatters.end()) {
        return core::Result<VegetationPreviewResult>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "vegetation preview recipe is not present in CONDITION authoring"));
    }
    const auto authoring_digest = authoring.canonical_digest();
    if (!authoring_digest) {
        return core::Result<VegetationPreviewResult>::failure(authoring_digest.error());
    }

    VegetationPreviewResult preview;
    preview.recipe_id = recipe_id;
    preview.project_revision = authoring.project_revision;
    preview.authoring_digest = authoring_digest.value();

    if (recipe_found->plant_source.has_value()) {
        const auto& binding = *recipe_found->plant_source;
        const auto source_found = plant_sources.find(binding.source_identity);
        if (source_found == plant_sources.end()) {
            return core::Result<VegetationPreviewResult>::failure(stale(
                "vegetation preview is missing its bound plant source '" +
                binding.source_identity + "'"));
        }
        const auto& receipt = source_found->second;
        if (receipt.identity != binding.source_identity) {
            return core::Result<VegetationPreviewResult>::failure(stale(
                "vegetation preview plant source identity no longer matches its binding"));
        }
        if (receipt.source_revision != binding.expected_source_revision ||
            receipt.digest != binding.expected_source_digest) {
            return core::Result<VegetationPreviewResult>::failure(stale(
                "vegetation preview plant source evidence is stale for '" +
                binding.source_identity + "'"));
        }
        preview.plant_source = receipt;
    }

    const std::array<std::pair<const FieldSourceBinding*, spatial_field::FieldSemantic>, 5>
        requested_fields{{
            {&recipe_found->moisture, spatial_field::FieldSemantic::moisture},
            {&recipe_found->shade, spatial_field::FieldSemantic::shade},
            {&recipe_found->disturbance, spatial_field::FieldSemantic::disturbance},
            {&recipe_found->suitability, spatial_field::FieldSemantic::vegetation_suitability},
            {&recipe_found->elevation, spatial_field::FieldSemantic::terrain_elevation},
        }};
    std::array<ResolvedField, 5> resolved{};
    for (std::size_t index = 0U; index < requested_fields.size(); ++index) {
        const auto field = resolve_field(fields, *requested_fields[index].first,
            requested_fields[index].second);
        if (!field) return core::Result<VegetationPreviewResult>::failure(field.error());
        resolved[index] = field.value();
        preview.fields.push_back(field.value().receipt);
    }

    const auto& moisture = *resolved[0U].field;
    const double width_extent = static_cast<double>(moisture.width() - 1U) * moisture.cell_size();
    const double height_extent = static_cast<double>(moisture.height() - 1U) * moisture.cell_size();
    const double spacing = recipe_found->cell_spacing;
    const double axis_count_u = std::floor(width_extent / spacing) + 1.0;
    const double axis_count_v = std::floor(height_extent / spacing) + 1.0;
    if (!std::isfinite(axis_count_u) || !std::isfinite(axis_count_v) ||
        axis_count_u < 1.0 || axis_count_v < 1.0 ||
        axis_count_u > static_cast<double>(VegetationPreviewResult::kMaxCandidates) ||
        axis_count_v > static_cast<double>(VegetationPreviewResult::kMaxCandidates) ||
        axis_count_u * axis_count_v >
            static_cast<double>(VegetationPreviewResult::kMaxCandidates)) {
        return core::Result<VegetationPreviewResult>::failure(invalid(
            "vegetation scatter grid exceeds the bounded candidate limit"));
    }
    const auto count_u = static_cast<std::uint32_t>(axis_count_u);
    const auto count_v = static_cast<std::uint32_t>(axis_count_v);
    preview.candidate_count = static_cast<std::size_t>(count_u) * count_v;

    if (recipe_found->enabled) {
        preview.instances.reserve(std::min<std::size_t>(
            preview.candidate_count, recipe_found->maximum_instances));
        for (std::uint32_t grid_v = 0U; grid_v < count_v; ++grid_v) {
            for (std::uint32_t grid_u = 0U; grid_u < count_u; ++grid_u) {
                const core::Vec2d uv{
                    moisture.origin_uv().x + static_cast<double>(grid_u) * spacing,
                    moisture.origin_uv().y + static_cast<double>(grid_v) * spacing};
                std::array<double, 5> samples{};
                for (std::size_t field_index = 0U; field_index < resolved.size(); ++field_index) {
                    const auto sample = resolved[field_index].field->sample_bilinear(uv);
                    if (!sample) {
                        return core::Result<VegetationPreviewResult>::failure(sample.error().with_context(
                            "vegetation preview requires complete field coverage at every candidate"));
                    }
                    samples[field_index] = sample.value();
                }
                const double moisture_value = normalized_value(*resolved[0U].field, samples[0U]);
                const double shade_value = normalized_value(*resolved[1U].field, samples[1U]);
                const double disturbance_value = normalized_value(*resolved[2U].field, samples[2U]);
                const double suitability_value = normalized_value(*resolved[3U].field, samples[3U]);
                const double score = std::clamp(moisture_value * shade_value *
                    (1.0 - disturbance_value) * suitability_value, 0.0, 1.0);
                if (score < recipe_found->minimum_suitability) continue;
                const std::uint64_t cell_key = recipe_found->deterministic_seed ^
                    mix64(recipe_found->id) ^
                    mix64((static_cast<std::uint64_t>(grid_v) << 32U) | grid_u);
                const double probability = std::clamp(
                    recipe_found->density_per_square_meter * spacing * spacing * score,
                    0.0, 1.0);
                if (unit_sample(cell_key, 0U) >= probability) continue;
                if (preview.instances.size() >= recipe_found->maximum_instances) {
                    return core::Result<VegetationPreviewResult>::failure(invalid(
                        "vegetation preview exceeds the recipe instance bound; reduce density or tighten suitability"));
                }
                const std::uint64_t stable_id = std::max<std::uint64_t>(1U,
                    mix64(cell_key ^ 0xa0761d6478bd642fULL));
                const double yaw = unit_sample(cell_key, 1U) *
                    6.28318530717958647692;
                const double scale = recipe_found->minimum_scale +
                    (recipe_found->maximum_scale - recipe_found->minimum_scale) *
                    unit_sample(cell_key, 2U);
                preview.instances.push_back(VegetationInstancePreview{
                    stable_id,
                    recipe_found->id,
                    recipe_found->species_asset_id,
                    grid_u,
                    grid_v,
                    core::Vec3d{uv.x, samples[4U], uv.y},
                    yaw,
                    scale,
                    score});
            }
        }
    }

    const std::string canonical = preview_canonical(preview);
    preview.preview_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    if (preview.preview_digest.is_zero()) {
        return core::Result<VegetationPreviewResult>::failure(invalid(
            "vegetation preview produced an invalid digest"));
    }
    return core::Result<VegetationPreviewResult>::success(std::move(preview));
}

} // namespace carto::condition
