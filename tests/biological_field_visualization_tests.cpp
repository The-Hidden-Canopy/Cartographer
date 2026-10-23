#include <carto/biological/field_visualization.hpp>

#include <bit>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace carto;
using namespace carto::biological;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::span<const std::uint8_t> bytes) {
    return assets::sha256(bytes);
}

void append_u64_le(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
}

void append_f32_le(std::vector<std::uint8_t>& bytes, float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU));
    }
}

std::vector<std::uint8_t> sample_blob(std::initializer_list<float> values) {
    std::vector<std::uint8_t> bytes{'C', 'B', 'F', '3', '2', 'L', 'E', '1'};
    append_u64_le(bytes, values.size());
    for (const float value : values) append_f32_le(bytes, value);
    return bytes;
}

BiologicalAuthoringDocument document_for(
    const std::vector<std::uint8_t>& bytes,
    FieldValueKind value_kind = FieldValueKind::scalar,
    FieldRepresentation representation = FieldRepresentation::statistical_shading,
    double minimum = 0.0,
    double maximum = 1.0) {
    BiologicalAuthoringDocument document;
    document.source = AuthoringSourceIdentity{
        "fixtures/field-preview", 42U, core::Revision{7U}, digest(
            std::span<const std::uint8_t>{bytes})};
    document.organism_family = "fixture";
    require(static_cast<bool>(document.insert_field(FieldRecord{
                .id = FieldId{9U},
                .kind = FieldKind::density,
                .value_kind = value_kind,
                .representation = representation,
                .domain = "surface",
                .coordinate_frame = "organism.local",
                .units = "unit",
                .sample_count = bytes.size() >= 16U ? (bytes.size() - 16U) / 4U : 0U,
                .stable_seed = 77U,
                .coverage_field = std::nullopt,
                .data_digest = digest(std::span<const std::uint8_t>{bytes}),
                .minimum = minimum,
                .maximum = maximum,
            })), "fixture field is accepted");
    return document;
}

void exact_digest_bound_samples_make_labelled_derived_raster() {
    const auto bytes = sample_blob({0.0F, 0.5F, 1.0F});
    const auto document = document_for(bytes);
    const auto raster = render_scalar_field_preview(document, FieldId{9U}, bytes);
    require(static_cast<bool>(raster), "valid exact sample blob renders");
    require(raster.value().width == 3U && raster.value().height == 1U,
        "output is a one-dimensional indexed strip");
    require(raster.value().rgba == std::vector<std::uint8_t>({
                0U, 0U, 0U, 255U, 128U, 128U, 128U, 255U, 255U, 255U, 255U, 255U}),
        "authored scalar values map deterministically to grayscale");
    require(raster.value().visualization_digest.is_zero() == false,
        "derived raster has a content digest");
    require(static_cast<bool>(raster.value().validate_against(document, bytes)),
        "derived raster validates against the exact source and blob");
    require(raster.value().label == DerivedFieldVisualization::kLabel,
        "preview carries an explicit derived label");
}

void stale_malformed_unsupported_and_over_budget_inputs_fail_closed() {
    const auto bytes = sample_blob({0.0F, 0.5F, 1.0F});
    auto document = document_for(bytes);
    auto changed = bytes;
    changed.back() ^= 1U;
    const auto stale = render_scalar_field_preview(document, FieldId{9U}, changed);
    require(!stale && stale.error().code == core::ErrorCode::stale_data,
        "changed sample bytes cannot substitute for the authored digest");
    require(!render_scalar_field_preview(document, FieldId{9U}, bytes, 2U),
        "caller pixel budget is enforced");
    require(!render_scalar_field_preview(document, FieldId{99U}, bytes),
        "missing fields are not synthesized");

    auto wrong_magic = bytes;
    wrong_magic[0] ^= 1U;
    const auto wrong_magic_doc = document_for(wrong_magic);
    require(!render_scalar_field_preview(wrong_magic_doc, FieldId{9U}, wrong_magic),
        "unknown sample formats are not guessed");

    auto truncated = bytes;
    truncated.pop_back();
    require(!render_scalar_field_preview(document, FieldId{9U}, truncated),
        "truncated data is rejected");
    auto non_finite = sample_blob({0.0F, std::numeric_limits<float>::quiet_NaN(), 1.0F});
    const auto non_finite_doc = document_for(non_finite);
    require(!render_scalar_field_preview(non_finite_doc, FieldId{9U}, non_finite),
        "non-finite samples are refused even when the digest matches");
    auto out_of_bounds = sample_blob({0.0F, 0.5F, 1.25F});
    const auto out_of_bounds_doc = document_for(out_of_bounds);
    require(!render_scalar_field_preview(
                out_of_bounds_doc, FieldId{9U}, out_of_bounds),
        "samples outside the authored range are refused");

    const auto vector_doc = document_for(bytes, FieldValueKind::vector);
    require(!render_scalar_field_preview(vector_doc, FieldId{9U}, bytes),
        "unsupported vector data is not flattened to scalar");
    const auto geometry_doc = document_for(bytes, FieldValueKind::scalar,
        FieldRepresentation::geometry);
    require(!render_scalar_field_preview(geometry_doc, FieldId{9U}, bytes),
        "geometry fields are not presented as statistical shading");
}

void preview_does_not_mutate_authored_truth_and_stales_on_revision_change() {
    const auto bytes = sample_blob({0.25F, 0.75F});
    const auto document = document_for(bytes);
    const auto before = document.serialize();
    const auto preview = render_scalar_field_preview(document, FieldId{9U}, bytes);
    require(static_cast<bool>(preview), "fixture preview succeeds");
    require(document.serialize() == before,
        "preview leaves authored document serialization unchanged");

    auto newer = document;
    newer.source.revision = core::Revision{8U};
    newer.source.digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("new source"), 10U});
    require(!preview.value().validate_against(newer, bytes),
        "revision-changed source rejects old preview receipt");
    auto same_revision_replacement = document;
    same_revision_replacement.source.digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("replaced"), 8U});
    require(!preview.value().validate_against(same_revision_replacement, bytes),
        "same-revision source replacement rejects old preview receipt");
}

} // namespace

int main() {
    exact_digest_bound_samples_make_labelled_derived_raster();
    stale_malformed_unsupported_and_over_budget_inputs_fail_closed();
    preview_does_not_mutate_authored_truth_and_stales_on_revision_change();
    std::cout << "PASS biological derived scalar field visualization boundaries\n";
    return EXIT_SUCCESS;
}
