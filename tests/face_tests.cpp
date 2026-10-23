#include <carto/assets/blob_store.hpp>
#include <carto/character/controls.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/time.hpp>
#include <carto/face/expression.hpp>
#include <carto/face/lipsync.hpp>
#include <carto/face/phoneme.hpp>
#include <carto/face/receipt.hpp>
#include <carto/face/region.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

using namespace carto;
using namespace carto::face;
namespace character = carto::character;

character::ControlDefinition make_control(
    character::ControlId id,
    std::string control_set,
    std::uint8_t allowed_axes,
    std::size_t limited_axis,
    double minimum,
    double maximum) {
    character::ControlDefinition control;
    control.id = id;
    control.shape = character::ControlShape::locator;
    control.control_set = std::move(control_set);
    control.allowed_axes = allowed_axes;
    control.axis_minimum[limited_axis] = minimum;
    control.axis_maximum[limited_axis] = maximum;
    return control;
}

void region_schema_binds_and_fails_closed_on_stale_digests() {
    character::ControlGraph controls;
    require(static_cast<bool>(controls.insert(make_control(
                character::ControlId{1U}, "face.brow",
                character::kAxisRotateZ, 5U, -1.0, 1.0))),
        "brow control inserts");
    require(static_cast<bool>(controls.insert(make_control(
                character::ControlId{2U}, "face.jaw",
                character::kAxisRotateX, 3U, -0.5, 0.5))),
        "jaw control inserts");

    const std::vector<character::MorphTargetId> morphs{
        character::MorphTargetId{1U}, character::MorphTargetId{2U},
        character::MorphTargetId{3U}};
    const auto graph_digest = control_graph_digest(controls);
    require(static_cast<bool>(graph_digest), "control graph digest computes");
    const auto library_digest = morph_library_digest(morphs);
    require(static_cast<bool>(library_digest), "morph library digest computes");

    FacialRegionSchema schema;
    schema.character_id = character::CharacterId{7U};
    schema.control_graph_digest = graph_digest.value();
    schema.morph_library_digest = library_digest.value();
    require(static_cast<bool>(schema.insert(RegionBinding{
                FacialRegion::brow, FacialSide::bilateral,
                "face.brow", "brow.raise"})),
        "brow binding inserts");
    require(static_cast<bool>(schema.insert(RegionBinding{
                FacialRegion::jaw, FacialSide::bilateral,
                "face.jaw", "jaw.open"})),
        "jaw binding inserts");
    require(static_cast<bool>(schema.insert(RegionBinding{
                FacialRegion::jaw, FacialSide::left,
                "face.jaw", "jaw.shift"})),
        "a different side of a bound region inserts");
    require(!schema.insert(RegionBinding{
                FacialRegion::jaw, FacialSide::bilateral,
                "face.jaw", "jaw.open"}),
        "duplicate (region, side) binding is refused");
    require(static_cast<bool>(validate_region_bindings(schema, controls, morphs)),
        "schema validates against live state");

    const auto schema_digest = schema.canonical_digest();
    require(static_cast<bool>(schema_digest), "schema digest computes");
    const auto roundtrip = FacialRegionSchema::deserialize(schema.serialize());
    require(static_cast<bool>(roundtrip), "schema round-trips");
    const auto roundtrip_digest = roundtrip.value().canonical_digest();
    require(static_cast<bool>(roundtrip_digest) &&
            roundtrip_digest.value() == schema_digest.value(),
        "schema round-trip preserves the canonical digest");

    FacialRegionSchema unknown_set;
    unknown_set.character_id = character::CharacterId{7U};
    unknown_set.control_graph_digest = graph_digest.value();
    unknown_set.morph_library_digest = library_digest.value();
    require(static_cast<bool>(unknown_set.insert(RegionBinding{
                FacialRegion::lip, FacialSide::bilateral,
                "face.missing", "lip.pucker"})),
        "unknown-set binding inserts structurally");
    require(!validate_region_bindings(unknown_set, controls, morphs),
        "binding to a control set the character lacks is an honest refusal");

    const std::vector<character::MorphTargetId> changed_morphs{
        character::MorphTargetId{1U}, character::MorphTargetId{2U},
        character::MorphTargetId{4U}};
    const auto stale = validate_region_bindings(schema, controls, changed_morphs);
    require(!stale, "changed morph population invalidates the schema");
    require(stale.error().code == core::ErrorCode::stale_data,
        "stale schema fails closed with stale_data");

    require(!morph_library_digest(std::vector<character::MorphTargetId>{
                character::MorphTargetId{1U}, character::MorphTargetId{1U}}),
        "duplicate morph identities are refused");
}

Expression make_smile() {
    Expression smile;
    smile.name = "smile";
    smile.sum_policy = ExpressionSumPolicy::normalized;
    require(static_cast<bool>(smile.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{1U}, 1.0})),
        "smile term 1 inserts");
    require(static_cast<bool>(smile.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{2U}, 1.0})),
        "smile term 2 inserts");
    require(!smile.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{2U}, 0.5}),
        "duplicate morph term is refused");
    return smile;
}

void expression_blend_normalizes_and_detects_conflicts() {
    const Expression smile = make_smile();

    const ExpressionBlendTerm full{&smile, 1.0};
    const auto blended = blend_expressions(std::span{&full, 1U});
    require(static_cast<bool>(blended), "normalized blend succeeds");
    const auto& weights = blended.value().morph_weights;
    require(weights.size() == 2U, "blend covers both targets");
    require(weights.at(character::MorphTargetId{1U}) == 0.5 &&
            weights.at(character::MorphTargetId{2U}) == 0.5,
        "normalized policy rescales terms to a unit sum");
    require(blended.value().total_morph_weight == 1.0,
        "normalized blend total equals the blend weight");

    const auto blended_again = blend_expressions(std::span{&full, 1U});
    require(blended_again.value().serialize() == blended.value().serialize(),
        "blending is deterministic");

    const ExpressionBlendTerm half{&smile, 0.25};
    const auto partial = blend_expressions(std::span{&half, 1U});
    require(partial.value().morph_weights.at(character::MorphTargetId{1U}) == 0.125,
        "blend weight scales the normalized combination");

    Expression wide;
    wide.name = "wide";
    wide.sum_policy = ExpressionSumPolicy::normalized;
    require(static_cast<bool>(wide.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{2U}, 1.0})),
        "wide term inserts");
    require(static_cast<bool>(wide.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{3U}, 2.0})),
        "wide second term inserts");

    const Expression sorted_set[2] = {smile, wide};
    const auto conflicts = detect_expression_conflicts(std::span{sorted_set, 2U});
    require(static_cast<bool>(conflicts), "conflict detection runs");
    require(conflicts.value().conflicts.size() == 1U,
        "one shared target between normalized expressions conflicts");
    require(conflicts.value().conflicts[0U].target == character::MorphTargetId{2U},
        "conflict names the shared morph target");
    require(conflicts.value().conflicts[0U].first_expression == "smile" &&
            conflicts.value().conflicts[0U].second_expression == "wide",
        "conflict records both expression names in canonical order");
    const auto conflicts_again =
        detect_expression_conflicts(std::span{sorted_set, 2U});
    require(conflicts_again.value().serialize() == conflicts.value().serialize(),
        "conflict receipt is digest-stable");

    Expression add_a;
    add_a.name = "add.a";
    add_a.sum_policy = ExpressionSumPolicy::additive;
    require(static_cast<bool>(add_a.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{1U}, 1.0})),
        "additive term inserts");
    Expression add_b;
    add_b.name = "add.b";
    add_b.sum_policy = ExpressionSumPolicy::additive;
    require(static_cast<bool>(add_b.insert_morph_term(
                MorphBlendTerm{character::MorphTargetId{1U}, 1.0})),
        "second additive term inserts");
    const Expression additive_set[2] = {add_a, add_b};
    require(detect_expression_conflicts(std::span{additive_set, 2U})
                .value().conflicts.empty(),
        "pure additive blends commute and produce no conflict");

    const Expression unordered_set[2] = {wide, smile};
    require(!detect_expression_conflicts(std::span{unordered_set, 2U}),
        "unsorted expression sets are refused");

    Expression hostile;
    hostile.name = "hostile";
    require(!hostile.insert_morph_term(MorphBlendTerm{
                character::MorphTargetId{1U},
                std::numeric_limits<double>::quiet_NaN()}),
        "non-finite morph weight is refused");
    require(!hostile.insert_morph_term(MorphBlendTerm{
                character::MorphTargetId{1U}, -0.5}),
        "negative morph weight is refused");
    require(!hostile.insert_morph_term(MorphBlendTerm{
                character::MorphTargetId{1U}, kMaxMorphTermWeight + 1.0}),
        "morph weight above the ceiling is refused");

    Expression nan_name;
    nan_name.name = std::string(1U, '\x01');
    require(!nan_name.validate(), "control-byte expression name is refused");

    const ExpressionBlendTerm bad_weight{&smile, 1.5};
    require(!blend_expressions(std::span{&bad_weight, 1U}),
        "blend weight outside [0, 1] is refused");
    const ExpressionBlendTerm null_term{nullptr, 1.0};
    require(!blend_expressions(std::span{&null_term, 1U}),
        "null expression reference is refused");

    const auto smile_digest = smile.canonical_digest();
    require(static_cast<bool>(smile_digest), "expression digest computes");
    const auto expression_roundtrip = Expression::deserialize(smile.serialize());
    require(static_cast<bool>(expression_roundtrip) &&
            expression_roundtrip.value().canonical_digest().value() == smile_digest.value(),
        "expression round-trip preserves the canonical digest");

    character::ControlGraph controls;
    require(static_cast<bool>(controls.insert(make_control(
                character::ControlId{5U}, "face.jaw",
                character::kAxisRotateX, 3U, -0.5, 0.5))),
        "control for offset admission inserts");
    Expression jaw_drop;
    jaw_drop.name = "jaw.drop";
    ControlOffsetTerm in_domain;
    in_domain.control = character::ControlId{5U};
    in_domain.axes_mask = character::kAxisRotateX;
    in_domain.offsets[3U] = 0.25;
    require(static_cast<bool>(jaw_drop.insert_control_term(in_domain)),
        "in-domain control offset inserts");
    require(static_cast<bool>(validate_expression_controls(jaw_drop, controls)),
        "in-domain control offset admits against the graph");
    in_domain.offsets[3U] = 0.9;
    Expression out_of_domain;
    out_of_domain.name = "jaw.break";
    require(static_cast<bool>(out_of_domain.insert_control_term(in_domain)),
        "structural insert succeeds");
    require(!validate_expression_controls(out_of_domain, controls),
        "offset outside the authored axis domain is refused");
    ControlOffsetTerm wrong_axis = in_domain;
    wrong_axis.axes_mask = character::kAxisTranslateY;
    Expression bad_axis;
    bad_axis.name = "jaw.wrong";
    require(static_cast<bool>(bad_axis.insert_control_term(wrong_axis)),
        "structural insert of wrong-axis term succeeds");
    require(!validate_expression_controls(bad_axis, controls),
        "driving an axis the control does not allow is refused");
}

void viseme_mapping_roundtrip_and_unknown_phoneme_refusal() {
    PhonemeInventory inventory;
    inventory.locale = "en-US";
    require(static_cast<bool>(inventory.insert("AA")), "AA inserts");
    require(static_cast<bool>(inventory.insert("M")), "M inserts");
    require(static_cast<bool>(inventory.insert("S")), "S inserts");
    require(!inventory.insert("S"), "duplicate phoneme is refused");
    require(!inventory.insert(std::string(17U, 'x')),
        "oversized phoneme token is refused");
    const auto inventory_digest = inventory.canonical_digest();
    require(static_cast<bool>(inventory_digest), "inventory digest computes");

    VisemeMapping mapping;
    mapping.name = "english-basic-4";
    mapping.locale = "en-US";
    mapping.inventory_digest = inventory_digest.value();
    require(static_cast<bool>(mapping.insert(
                VisemeMappingEntry{"AA", "jaw_open", 1.0})),
        "AA entry inserts");
    require(static_cast<bool>(mapping.insert(
                VisemeMappingEntry{"M", "lip_close", 1.0})),
        "M entry inserts");
    require(static_cast<bool>(mapping.insert(
                VisemeMappingEntry{"S", "lip_seal", 0.8})),
        "S entry inserts");
    require(!mapping.insert(VisemeMappingEntry{"S", "other", 1.0}),
        "duplicate phoneme entry is refused");
    require(static_cast<bool>(mapping.validate_against(inventory)),
        "mapping admits against its inventory");

    const auto mapped = mapping.map("M");
    require(static_cast<bool>(mapped) && mapped.value().viseme == "lip_close" &&
            mapped.value().weight == 1.0,
        "mapped phoneme resolves to its viseme and weight");
    const auto scaled = mapping.map("S");
    require(static_cast<bool>(scaled) && scaled.value().weight == 0.8,
        "mapping weight scales the viseme contribution");
    const auto unknown = mapping.map("ZZ");
    require(!unknown, "unknown phoneme is an honest refusal");
    require(unknown.error().code == core::ErrorCode::unsupported,
        "unmapped phoneme fails with unsupported");

    const auto mapping_digest = mapping.canonical_digest();
    require(static_cast<bool>(mapping_digest), "mapping digest computes");
    const auto roundtrip = VisemeMapping::deserialize(mapping.serialize());
    require(static_cast<bool>(roundtrip) &&
            roundtrip.value().canonical_digest().value() == mapping_digest.value(),
        "mapping round-trip preserves the canonical digest");

    PhonemeInventory other_inventory;
    other_inventory.locale = "en-US";
    require(static_cast<bool>(other_inventory.insert("AA")), "other AA inserts");
    const auto stale = mapping.validate_against(other_inventory);
    require(!stale && stale.error().code == core::ErrorCode::stale_data,
        "mapping bound to a different inventory fails closed");

    VisemeMapping foreign;
    foreign.name = "english-basic-4";
    foreign.locale = "en-US";
    foreign.inventory_digest = inventory_digest.value();
    require(static_cast<bool>(foreign.insert(
                VisemeMappingEntry{"ZZ", "jaw_open", 1.0})),
        "foreign entry inserts structurally");
    const auto foreign_check = foreign.validate_against(inventory);
    require(!foreign_check &&
            foreign_check.error().code == core::ErrorCode::not_found,
        "entry phoneme outside the inventory is refused");

    VisemeMapping bad_weight;
    bad_weight.name = "bad";
    bad_weight.locale = "en-US";
    bad_weight.inventory_digest = inventory_digest.value();
    require(!bad_weight.insert(VisemeMappingEntry{
                "AA", "jaw_open",
                std::numeric_limits<double>::infinity()}),
        "non-finite mapping weight is refused");
}

SpeechAlignment make_alignment() {
    SpeechAlignment alignment;
    alignment.audio_digest = digest("fake-audio-content");
    alignment.ticks_per_second = 24U;
    alignment.language = "en-US";
    alignment.confidence = 0.95;
    require(static_cast<bool>(alignment.timeline.insert(PhonemeSegment{
                character::RationalTime{0, 24U}, character::RationalTime{12, 24U},
                "AA"})),
        "AA segment inserts");
    require(static_cast<bool>(alignment.timeline.insert(PhonemeSegment{
                character::RationalTime{12, 24U}, character::RationalTime{24, 24U},
                "M"})),
        "M segment inserts");
    require(static_cast<bool>(alignment.timeline.insert(PhonemeSegment{
                character::RationalTime{24, 24U}, character::RationalTime{36, 24U},
                "ZZ"})),
        "unmapped segment inserts");
    require(static_cast<bool>(alignment.timeline.insert(PhonemeSegment{
                character::RationalTime{36, 24U}, character::RationalTime{48, 24U},
                "S"})),
        "S segment inserts");
    require(!alignment.timeline.insert(PhonemeSegment{
                character::RationalTime{30, 24U}, character::RationalTime{40, 24U},
                "AA"}),
        "overlapping segment is refused");
    require(!alignment.timeline.insert(PhonemeSegment{
                character::RationalTime{60, 24U}, character::RationalTime{48, 24U},
                "AA"}),
        "reversed segment is refused");
    return alignment;
}

VisemeMapping make_mapping() {
    PhonemeInventory inventory;
    inventory.locale = "en-US";
    require(static_cast<bool>(inventory.insert("AA")), "inventory AA inserts");
    require(static_cast<bool>(inventory.insert("M")), "inventory M inserts");
    require(static_cast<bool>(inventory.insert("S")), "inventory S inserts");
    VisemeMapping mapping;
    mapping.name = "english-basic-4";
    mapping.locale = "en-US";
    mapping.inventory_digest = inventory.canonical_digest().value();
    require(static_cast<bool>(mapping.insert(
                VisemeMappingEntry{"AA", "jaw_open", 1.0})),
        "mapping AA inserts");
    require(static_cast<bool>(mapping.insert(
                VisemeMappingEntry{"M", "lip_close", 1.0})),
        "mapping M inserts");
    require(static_cast<bool>(mapping.insert(
                VisemeMappingEntry{"S", "lip_seal", 0.8})),
        "mapping S inserts");
    return mapping;
}

void lipsync_two_second_grid_is_deterministic() {
    const SpeechAlignment alignment = make_alignment();
    const VisemeMapping mapping = make_mapping();

    std::vector<character::RationalTime> grid;
    for (std::int64_t tick = 0; tick <= 48; ++tick) {
        grid.push_back(character::RationalTime{tick, 24U});
    }
    const auto evaluation = evaluate_lipsync(alignment, mapping, std::span{grid});
    require(static_cast<bool>(evaluation), "lipsync evaluation succeeds");
    require(evaluation.value().samples.size() == grid.size(),
        "one sample per grid time");
    require(evaluation.value().unmapped_phonemes.size() == 1U &&
            evaluation.value().unmapped_phonemes[0U] == "ZZ",
        "unmapped phoneme is reported exactly once");

    const auto& samples = evaluation.value().samples;
    require(samples[0U].weights.size() == 1U &&
            samples[0U].weights[0U].first == "jaw_open" &&
            samples[0U].weights[0U].second == 1.0,
        "grid start holds the first viseme");
    require(samples[11U].weights[0U].first == "jaw_open",
        "constant hold inside the first segment");
    require(samples[12U].weights[0U].first == "lip_close",
        "half-open boundary activates the next segment");
    require(samples[23U].weights[0U].first == "lip_close",
        "constant hold inside the second segment");
    require(samples[24U].weights.empty(),
        "unmapped phoneme produces the rest pose");
    require(samples[35U].weights.empty(),
        "rest pose holds through the unmapped segment");
    require(samples[36U].weights.size() == 1U &&
            samples[36U].weights[0U].first == "lip_seal" &&
            samples[36U].weights[0U].second == 0.8,
        "mapping weight scales the evaluated viseme");
    require(samples[48U].weights.empty(),
        "grid end past the last segment end rests");

    const auto again = evaluate_lipsync(alignment, mapping, std::span{grid});
    require(again.value().serialize() == evaluation.value().serialize(),
        "lipsync evaluation is deterministic across runs");
    require(again.value().canonical_digest().value() ==
            evaluation.value().canonical_digest().value(),
        "lipsync digest is stable across runs");

    const auto alignment_roundtrip =
        SpeechAlignment::deserialize(alignment.serialize());
    require(static_cast<bool>(alignment_roundtrip) &&
            alignment_roundtrip.value().canonical_digest().value() ==
                alignment.canonical_digest().value(),
        "speech alignment round-trips with a stable digest");

    std::vector<character::RationalTime> wrong_timebase{
        character::RationalTime{0, 25U}};
    require(!evaluate_lipsync(alignment, mapping, std::span{wrong_timebase}),
        "sample time in a foreign timebase is refused");
    std::vector<character::RationalTime> non_monotonic{
        character::RationalTime{12, 24U}, character::RationalTime{12, 24U}};
    require(!evaluate_lipsync(alignment, mapping, std::span{non_monotonic}),
        "non-increasing sample times are refused");

    SpeechAlignment bad_confidence = alignment;
    bad_confidence.confidence = 1.5;
    require(!bad_confidence.validate(),
        "confidence outside [0, 1] is refused");
    SpeechAlignment zero_digest = alignment;
    zero_digest.audio_digest = assets::Sha256Digest{};
    require(!zero_digest.validate(),
        "zero audio digest is refused");
}

void receipt_digest_is_stable_and_bounds_checked() {
    const Expression smile = make_smile();
    const ExpressionBlendTerm full{&smile, 1.0};
    const auto blend = blend_expressions(std::span{&full, 1U});
    require(static_cast<bool>(blend), "blend for receipt succeeds");

    const SpeechAlignment alignment = make_alignment();
    const VisemeMapping mapping = make_mapping();
    std::vector<character::RationalTime> grid;
    for (std::int64_t tick = 0; tick <= 48; ++tick) {
        grid.push_back(character::RationalTime{tick, 24U});
    }
    const auto evaluation = evaluate_lipsync(alignment, mapping, std::span{grid});
    require(static_cast<bool>(evaluation), "lipsync for receipt succeeds");

    const std::vector<assets::Sha256Digest> digests{
        smile.canonical_digest().value()};
    const auto receipt = build_facial_evaluation_receipt(
        character::CharacterId{7U}, "cpu_reference/face_eval/v1",
        std::span{digests}, blend.value(), evaluation.value());
    require(static_cast<bool>(receipt), "receipt builds");
    require(receipt.value().expression_digests.size() == 1U,
        "receipt records the expression digest");
    require(receipt.value().max_observed_morph_weight == 0.5,
        "receipt measures the maximum blended morph weight");
    require(receipt.value().clamped_terms == 0U,
        "canonical builder records zero clamped terms");

    const auto again = build_facial_evaluation_receipt(
        character::CharacterId{7U}, "cpu_reference/face_eval/v1",
        std::span{digests}, blend.value(), evaluation.value());
    require(again.value().canonical_digest().value() ==
            receipt.value().canonical_digest().value(),
        "receipt digest is stable across identical builds");

    const auto roundtrip =
        FacialEvaluationReceipt::deserialize(receipt.value().serialize());
    require(static_cast<bool>(roundtrip) &&
            roundtrip.value().canonical_digest().value() ==
                receipt.value().canonical_digest().value(),
        "receipt round-trips with a stable digest");

    const auto changed = build_facial_evaluation_receipt(
        character::CharacterId{8U}, "cpu_reference/face_eval/v1",
        std::span{digests}, blend.value(), evaluation.value());
    require(changed.value().canonical_digest().value() !=
            receipt.value().canonical_digest().value(),
        "a different character yields a different receipt digest");

    FacialEvaluationReceipt hostile = receipt.value();
    hostile.max_observed_morph_weight =
        std::numeric_limits<double>::quiet_NaN();
    require(!hostile.validate(),
        "non-finite bounds summary is refused");
    hostile = receipt.value();
    hostile.max_observed_morph_weight = kMaxObservedMorphWeight + 1.0;
    require(!hostile.validate(),
        "bounds summary above the ceiling is refused");
    hostile = receipt.value();
    hostile.evaluation_identity = std::string(300U, 'x');
    require(!hostile.validate(),
        "oversized evaluator identity is refused");
    hostile = receipt.value();
    hostile.expression_digests.push_back(hostile.expression_digests.front());
    require(!hostile.validate(),
        "duplicate expression digests are refused");
    hostile = receipt.value();
    hostile.lipsync.samples[0U].weights[0U].second =
        std::numeric_limits<double>::infinity();
    require(!hostile.validate(),
        "non-finite viseme weight inside the receipt is refused");
}

} // namespace

int main() {
    region_schema_binds_and_fails_closed_on_stale_digests();
    expression_blend_normalizes_and_detects_conflicts();
    viseme_mapping_roundtrip_and_unknown_phoneme_refusal();
    lipsync_two_second_grid_is_deterministic();
    receipt_digest_is_stable_and_bounds_checked();
    std::cout << "face tests passed\n";
    return EXIT_SUCCESS;
}
