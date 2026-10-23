#include <carto/biological/authoring.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <map>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::biological {
namespace {

constexpr std::string_view kDocumentMagic = "CARTOGRAPHER_BIOLOGICAL_AUTHORING";
constexpr std::string_view kPublicationMagic =
    "CARTOGRAPHER_BIOLOGICAL_PREPARED_PUBLICATION";
constexpr std::size_t kMaxTextBytes = 256U;
constexpr std::uint64_t kMaxSamples = 16ULL * 1024ULL * 1024ULL;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_digest(std::istream& input, assets::Sha256Digest& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    value = parsed.value();
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool require_end(std::istream& input) {
    std::string token;
    std::string trailing;
    return static_cast<bool>(input >> token) && token == "END" && !(input >> trailing);
}

bool read_optional_digest(std::istream& input, std::optional<assets::Sha256Digest>& value) {
    std::uint64_t present = 0U;
    if (!read_unsigned(input, present) || present > 1U) return false;
    if (present == 0U) {
        value.reset();
        return true;
    }
    assets::Sha256Digest digest;
    if (!read_digest(input, digest)) return false;
    value = digest;
    return true;
}

void write_optional_digest(
    std::ostringstream& output,
    const std::optional<assets::Sha256Digest>& value) {
    output << (value.has_value() ? 1U : 0U);
    if (value.has_value()) output << ' ' << value->hex();
}

template <typename Tag>
bool read_optional_id(std::istream& input, std::optional<Id<Tag>>& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded)) return false;
    if (encoded == 0U) {
        value.reset();
    } else {
        value = Id<Tag>{encoded};
    }
    return true;
}

template <typename Tag>
void write_optional_id(std::ostringstream& output, const std::optional<Id<Tag>>& value) {
    output << (value.has_value() ? value->value : 0U);
}

bool valid_field_kind(FieldKind value) {
    return field_kind_name(value) != "unknown";
}

bool valid_field_value_kind(FieldValueKind value) {
    return field_value_kind_name(value) != "unknown";
}

bool valid_field_representation(FieldRepresentation value) {
    return field_representation_name(value) != "unknown";
}

bool valid_structure_kind(StructureKind value) {
    return structure_kind_name(value) != "unknown";
}

bool valid_relation_kind(RelationKind value) {
    return relation_kind_name(value) != "unknown";
}

bool valid_layer_kind(LayerKind value) {
    return layer_kind_name(value) != "unknown";
}

struct LayerFamilyContract {
    LayerKind layer_kind;
    std::span<const FieldKind> required_fields;
};

LayerFamilyContract family_contract(LayerFamily family) noexcept {
    static constexpr std::array hair{FieldKind::orientation, FieldKind::density,
        FieldKind::length, FieldKind::radius};
    static constexpr std::array fur{FieldKind::orientation, FieldKind::density,
        FieldKind::length, FieldKind::radius, FieldKind::coverage};
    static constexpr std::array feather{FieldKind::orientation, FieldKind::density,
        FieldKind::length, FieldKind::thickness};
    static constexpr std::array weave{FieldKind::orientation, FieldKind::density,
        FieldKind::thickness, FieldKind::compaction};
    static constexpr std::array granular{FieldKind::density, FieldKind::radius,
        FieldKind::compaction};
    static constexpr std::array connective{FieldKind::orientation, FieldKind::thickness,
        FieldKind::compliance};
    static constexpr std::array muscle{FieldKind::orientation, FieldKind::activation,
        FieldKind::mass};
    static constexpr std::array skin{FieldKind::thickness, FieldKind::pigmentation,
        FieldKind::damage};
    static constexpr std::array face{FieldKind::thickness, FieldKind::pigmentation,
        FieldKind::variation};
    static constexpr std::array injury{FieldKind::damage, FieldKind::deformation};
    static constexpr std::array species{FieldKind::growth, FieldKind::mass,
        FieldKind::variation};
    static constexpr std::array biomechanics{FieldKind::compliance,
        FieldKind::activation, FieldKind::mass, FieldKind::inertia, FieldKind::contact};
    static constexpr std::array anatomy{FieldKind::mass, FieldKind::variation};
    static constexpr std::array soft_tissue{FieldKind::thickness, FieldKind::compliance};
    static constexpr std::array adipose{FieldKind::thickness, FieldKind::mass,
        FieldKind::moisture};
    static constexpr std::array organ{FieldKind::mass, FieldKind::variation,
        FieldKind::compliance};
    static constexpr std::array vascular{FieldKind::radius, FieldKind::thickness,
        FieldKind::compliance};
    static constexpr std::array neural{FieldKind::activation, FieldKind::variation};
    static constexpr std::array fluid{FieldKind::density, FieldKind::mass,
        FieldKind::moisture};
    static constexpr std::array growth{FieldKind::growth, FieldKind::variation,
        FieldKind::mass};

    switch (family) {
    case LayerFamily::hair: return {LayerKind::strand, hair};
    case LayerFamily::fur: return {LayerKind::strand, fur};
    case LayerFamily::feather: return {LayerKind::feather, feather};
    case LayerFamily::weave: return {LayerKind::weave, weave};
    case LayerFamily::granular: return {LayerKind::granular, granular};
    case LayerFamily::connective_tissue:
        return {LayerKind::connective_tissue, connective};
    case LayerFamily::muscle: return {LayerKind::muscle, muscle};
    case LayerFamily::skin: return {LayerKind::skin, skin};
    case LayerFamily::face: return {LayerKind::face, face};
    case LayerFamily::injury: return {LayerKind::injury_overlay, injury};
    case LayerFamily::species_morphology:
        return {LayerKind::species_morphology, species};
    case LayerFamily::biomechanics:
        return {LayerKind::biomechanics, biomechanics};
    case LayerFamily::anatomy: return {LayerKind::anatomy, anatomy};
    case LayerFamily::soft_tissue: return {LayerKind::soft_tissue, soft_tissue};
    case LayerFamily::adipose: return {LayerKind::adipose, adipose};
    case LayerFamily::organ: return {LayerKind::organ, organ};
    case LayerFamily::vascular: return {LayerKind::vascular, vascular};
    case LayerFamily::neural: return {LayerKind::neural, neural};
    case LayerFamily::fluid: return {LayerKind::fluid, fluid};
    case LayerFamily::growth: return {LayerKind::growth, growth};
    }
    return {LayerKind::microstructure, {}};
}

bool valid_layer_family(LayerFamily value) noexcept {
    return !family_contract(value).required_fields.empty();
}

bool family_field_value_kind_matches(const FieldRecord& field) noexcept {
    switch (field.kind) {
    case FieldKind::orientation:
        return field.value_kind == FieldValueKind::direction ||
            field.value_kind == FieldValueKind::vector;
    case FieldKind::deformation:
        return field.value_kind == FieldValueKind::vector;
    case FieldKind::contact:
        return field.value_kind == FieldValueKind::scalar ||
            field.value_kind == FieldValueKind::vector;
    case FieldKind::damage:
        return field.value_kind == FieldValueKind::scalar ||
            field.value_kind == FieldValueKind::categorical;
    case FieldKind::custom:
        return false;
    case FieldKind::density:
    case FieldKind::radius:
    case FieldKind::length:
    case FieldKind::thickness:
    case FieldKind::coverage:
    case FieldKind::variation:
    case FieldKind::pigmentation:
    case FieldKind::moisture:
    case FieldKind::compaction:
    case FieldKind::compliance:
    case FieldKind::activation:
    case FieldKind::growth:
    case FieldKind::mass:
    case FieldKind::inertia:
        return field.value_kind == FieldValueKind::scalar;
    }
    return false;
}

bool valid_representation_kind(RepresentationKind value) {
    return representation_kind_name(value) != "unknown";
}

template <typename IdType>
bool sorted_unique_ids(const std::vector<IdType>& values) {
    return std::all_of(values.begin(), values.end(), [](const IdType& value) {
               return static_cast<bool>(value);
           }) &&
        std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

template <typename IdType>
bool cycle_in_parent_map(
    const std::map<IdType, std::optional<IdType>>& parents) {
    std::map<IdType, std::uint8_t> state;
    for (const auto& [start, ignored_parent] : parents) {
        static_cast<void>(ignored_parent);
        if (state[start] == 2U) continue;
        std::vector<IdType> path;
        IdType current = start;
        while (true) {
            if (state[current] == 1U) return true;
            if (state[current] == 2U) break;
            state[current] = 1U;
            path.push_back(current);
            const auto found = parents.find(current);
            if (found == parents.end() || !found->second.has_value()) break;
            current = *found->second;
        }
        for (const auto& id : path) state[id] = 2U;
    }
    return false;
}

template <typename IdType>
void write_id_list(std::ostringstream& output, const std::vector<IdType>& values) {
    output << values.size();
    for (const auto& value : values) output << ' ' << value.value;
}

template <typename IdType>
bool read_id_list(
    std::istream& input,
    std::size_t maximum,
    std::vector<IdType>& values) {
    std::size_t count = 0U;
    if (!read_count(input, maximum, count)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint64_t id = 0U;
        if (!read_unsigned(input, id)) return false;
        values.push_back(IdType{id});
    }
    return true;
}

} // namespace

std::string_view field_kind_name(FieldKind value) noexcept {
    switch (value) {
    case FieldKind::orientation: return "orientation";
    case FieldKind::density: return "density";
    case FieldKind::radius: return "radius";
    case FieldKind::length: return "length";
    case FieldKind::thickness: return "thickness";
    case FieldKind::coverage: return "coverage";
    case FieldKind::variation: return "variation";
    case FieldKind::pigmentation: return "pigmentation";
    case FieldKind::moisture: return "moisture";
    case FieldKind::compaction: return "compaction";
    case FieldKind::deformation: return "deformation";
    case FieldKind::compliance: return "compliance";
    case FieldKind::activation: return "activation";
    case FieldKind::damage: return "damage";
    case FieldKind::growth: return "growth";
    case FieldKind::mass: return "mass";
    case FieldKind::inertia: return "inertia";
    case FieldKind::contact: return "contact";
    case FieldKind::custom: return "custom";
    }
    return "unknown";
}

core::Result<FieldKind> field_kind_from_name(std::string_view value) {
    for (const auto candidate : {FieldKind::orientation, FieldKind::density,
             FieldKind::radius, FieldKind::length, FieldKind::thickness,
             FieldKind::coverage, FieldKind::variation, FieldKind::pigmentation,
             FieldKind::moisture, FieldKind::compaction, FieldKind::deformation,
             FieldKind::compliance, FieldKind::activation, FieldKind::damage,
             FieldKind::growth, FieldKind::mass, FieldKind::inertia,
             FieldKind::contact, FieldKind::custom}) {
        if (field_kind_name(candidate) == value) {
            return core::Result<FieldKind>::success(candidate);
        }
    }
    return core::Result<FieldKind>::failure(invalid("biological field kind is unsupported"));
}

std::string_view field_value_kind_name(FieldValueKind value) noexcept {
    switch (value) {
    case FieldValueKind::scalar: return "scalar";
    case FieldValueKind::vector: return "vector";
    case FieldValueKind::direction: return "direction";
    case FieldValueKind::categorical: return "categorical";
    }
    return "unknown";
}

core::Result<FieldValueKind> field_value_kind_from_name(std::string_view value) {
    for (const auto candidate : {FieldValueKind::scalar, FieldValueKind::vector,
             FieldValueKind::direction, FieldValueKind::categorical}) {
        if (field_value_kind_name(candidate) == value) {
            return core::Result<FieldValueKind>::success(candidate);
        }
    }
    return core::Result<FieldValueKind>::failure(
        invalid("biological field value kind is unsupported"));
}

std::string_view field_representation_name(FieldRepresentation value) noexcept {
    switch (value) {
    case FieldRepresentation::geometry: return "geometry";
    case FieldRepresentation::displacement: return "displacement";
    case FieldRepresentation::normal: return "normal";
    case FieldRepresentation::statistical_shading: return "statistical-shading";
    case FieldRepresentation::constraint: return "constraint";
    }
    return "unknown";
}

core::Result<FieldRepresentation> field_representation_from_name(std::string_view value) {
    for (const auto candidate : {FieldRepresentation::geometry,
             FieldRepresentation::displacement, FieldRepresentation::normal,
             FieldRepresentation::statistical_shading, FieldRepresentation::constraint}) {
        if (field_representation_name(candidate) == value) {
            return core::Result<FieldRepresentation>::success(candidate);
        }
    }
    return core::Result<FieldRepresentation>::failure(
        invalid("biological field representation is unsupported"));
}

std::string_view structure_kind_name(StructureKind value) noexcept {
    switch (value) {
    case StructureKind::organism: return "organism";
    case StructureKind::bone: return "bone";
    case StructureKind::cartilage: return "cartilage";
    case StructureKind::joint: return "joint";
    case StructureKind::tendon: return "tendon";
    case StructureKind::ligament: return "ligament";
    case StructureKind::fascia: return "fascia";
    case StructureKind::muscle: return "muscle";
    case StructureKind::soft_tissue: return "soft-tissue";
    case StructureKind::adipose: return "adipose";
    case StructureKind::skin: return "skin";
    case StructureKind::organ: return "organ";
    case StructureKind::vascular: return "vascular";
    case StructureKind::neural: return "neural";
    case StructureKind::fluid: return "fluid";
    case StructureKind::appendage: return "appendage";
    case StructureKind::garment: return "garment";
    case StructureKind::surface: return "surface";
    case StructureKind::landmark: return "landmark";
    }
    return "unknown";
}

core::Result<StructureKind> structure_kind_from_name(std::string_view value) {
    for (const auto candidate : {StructureKind::organism, StructureKind::bone,
             StructureKind::cartilage, StructureKind::joint, StructureKind::tendon,
             StructureKind::ligament, StructureKind::fascia, StructureKind::muscle,
             StructureKind::soft_tissue, StructureKind::adipose, StructureKind::skin,
             StructureKind::organ, StructureKind::vascular, StructureKind::neural,
             StructureKind::fluid, StructureKind::appendage, StructureKind::garment,
             StructureKind::surface, StructureKind::landmark}) {
        if (structure_kind_name(candidate) == value) {
            return core::Result<StructureKind>::success(candidate);
        }
    }
    return core::Result<StructureKind>::failure(
        invalid("biological structure kind is unsupported"));
}

std::string_view relation_kind_name(RelationKind value) noexcept {
    switch (value) {
    case RelationKind::attached_to: return "attached-to";
    case RelationKind::origin_insertion: return "origin-insertion";
    case RelationKind::contains: return "contains";
    case RelationKind::wraps: return "wraps";
    case RelationKind::contacts: return "contacts";
    case RelationKind::drives: return "drives";
    case RelationKind::overlays: return "overlays";
    case RelationKind::continues: return "continues";
    }
    return "unknown";
}

core::Result<RelationKind> relation_kind_from_name(std::string_view value) {
    for (const auto candidate : {RelationKind::attached_to,
             RelationKind::origin_insertion, RelationKind::contains,
             RelationKind::wraps, RelationKind::contacts, RelationKind::drives,
             RelationKind::overlays, RelationKind::continues}) {
        if (relation_kind_name(candidate) == value) {
            return core::Result<RelationKind>::success(candidate);
        }
    }
    return core::Result<RelationKind>::failure(
        invalid("biological relation kind is unsupported"));
}

std::string_view layer_kind_name(LayerKind value) noexcept {
    switch (value) {
    case LayerKind::microstructure: return "microstructure";
    case LayerKind::strand: return "strand";
    case LayerKind::weave: return "weave";
    case LayerKind::feather: return "feather";
    case LayerKind::granular: return "granular";
    case LayerKind::anatomy: return "anatomy";
    case LayerKind::connective_tissue: return "connective-tissue";
    case LayerKind::muscle: return "muscle";
    case LayerKind::soft_tissue: return "soft-tissue";
    case LayerKind::adipose: return "adipose";
    case LayerKind::skin: return "skin";
    case LayerKind::face: return "face";
    case LayerKind::organ: return "organ";
    case LayerKind::vascular: return "vascular";
    case LayerKind::neural: return "neural";
    case LayerKind::fluid: return "fluid";
    case LayerKind::injury_overlay: return "injury-overlay";
    case LayerKind::growth: return "growth";
    case LayerKind::species_morphology: return "species-morphology";
    case LayerKind::biomechanics: return "biomechanics";
    }
    return "unknown";
}

core::Result<LayerKind> layer_kind_from_name(std::string_view value) {
    for (const auto candidate : {LayerKind::microstructure, LayerKind::strand,
             LayerKind::weave, LayerKind::feather, LayerKind::granular,
             LayerKind::anatomy, LayerKind::connective_tissue, LayerKind::muscle,
             LayerKind::soft_tissue, LayerKind::adipose, LayerKind::skin,
             LayerKind::face, LayerKind::organ, LayerKind::vascular,
             LayerKind::neural, LayerKind::fluid, LayerKind::injury_overlay,
             LayerKind::growth, LayerKind::species_morphology,
             LayerKind::biomechanics}) {
        if (layer_kind_name(candidate) == value) {
            return core::Result<LayerKind>::success(candidate);
        }
    }
    return core::Result<LayerKind>::failure(
        invalid("biological layer kind is unsupported"));
}

std::string_view layer_family_name(LayerFamily value) noexcept {
    switch (value) {
    case LayerFamily::hair: return "hair";
    case LayerFamily::fur: return "fur";
    case LayerFamily::feather: return "feather";
    case LayerFamily::weave: return "weave";
    case LayerFamily::granular: return "granular";
    case LayerFamily::connective_tissue: return "connective-tissue";
    case LayerFamily::muscle: return "muscle";
    case LayerFamily::skin: return "skin";
    case LayerFamily::face: return "face";
    case LayerFamily::injury: return "injury";
    case LayerFamily::species_morphology: return "species-morphology";
    case LayerFamily::biomechanics: return "biomechanics";
    case LayerFamily::anatomy: return "anatomy";
    case LayerFamily::soft_tissue: return "soft-tissue";
    case LayerFamily::adipose: return "adipose";
    case LayerFamily::organ: return "organ";
    case LayerFamily::vascular: return "vascular";
    case LayerFamily::neural: return "neural";
    case LayerFamily::fluid: return "fluid";
    case LayerFamily::growth: return "growth";
    }
    return "unknown";
}

core::Result<LayerFamily> layer_family_from_name(std::string_view value) {
    for (const auto candidate : {LayerFamily::hair, LayerFamily::fur,
             LayerFamily::feather, LayerFamily::weave, LayerFamily::granular,
             LayerFamily::connective_tissue, LayerFamily::muscle, LayerFamily::skin,
             LayerFamily::face, LayerFamily::injury, LayerFamily::species_morphology,
             LayerFamily::biomechanics, LayerFamily::anatomy, LayerFamily::soft_tissue,
             LayerFamily::adipose, LayerFamily::organ, LayerFamily::vascular,
             LayerFamily::neural, LayerFamily::fluid, LayerFamily::growth}) {
        if (layer_family_name(candidate) == value) {
            return core::Result<LayerFamily>::success(candidate);
        }
    }
    return core::Result<LayerFamily>::failure(
        invalid("biological layer family is unsupported"));
}

std::string_view representation_kind_name(RepresentationKind value) noexcept {
    switch (value) {
    case RepresentationKind::hero: return "hero";
    case RepresentationKind::close: return "close";
    case RepresentationKind::gameplay: return "gameplay";
    case RepresentationKind::crowd: return "crowd";
    case RepresentationKind::distant: return "distant";
    }
    return "unknown";
}

core::Result<RepresentationKind> representation_kind_from_name(std::string_view value) {
    for (const auto candidate : {RepresentationKind::hero, RepresentationKind::close,
             RepresentationKind::gameplay, RepresentationKind::crowd,
             RepresentationKind::distant}) {
        if (representation_kind_name(candidate) == value) {
            return core::Result<RepresentationKind>::success(candidate);
        }
    }
    return core::Result<RepresentationKind>::failure(
        invalid("biological representation kind is unsupported"));
}

core::Result<void> AuthoringSourceIdentity::validate() const {
    if (!safe_text(source_namespace, 256U)) {
        return core::Result<void>::failure(invalid(
            "biological source namespace is empty, unsafe, or exceeds its bound"));
    }
    if (asset_id == 0U) {
        return core::Result<void>::failure(invalid(
            "biological source asset identity must be non-zero"));
    }
    if (revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "biological source revision is exhausted"));
    }
    if (digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "biological source digest must be bound"));
    }
    return core::Result<void>::success();
}

core::Result<void> FieldRecord::validate() const {
    if (!id) return core::Result<void>::failure(invalid("biological field id is zero"));
    if (!valid_field_kind(kind) || !valid_field_value_kind(value_kind) ||
        !valid_field_representation(representation)) {
        return core::Result<void>::failure(invalid("biological field enum is invalid"));
    }
    if (!safe_text(domain, kMaxSemanticBytes) ||
        !safe_text(coordinate_frame, kMaxSemanticBytes) ||
        !safe_text(units, kMaxSemanticBytes, true)) {
        return core::Result<void>::failure(invalid(
            "biological field domain, coordinate frame, or units are unsafe"));
    }
    if (sample_count == 0U || sample_count > kMaxSamples || stable_seed == 0U) {
        return core::Result<void>::failure(validation(
            "biological field sample count or stable seed is outside its bound"));
    }
    if (coverage_field.has_value() &&
        (coverage_field->value == 0U || *coverage_field == id)) {
        return core::Result<void>::failure(validation(
            "biological field coverage reference is invalid"));
    }
    if (data_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "biological field data digest must be bound"));
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
        return core::Result<void>::failure(validation(
            "biological field value bounds are non-finite or reversed"));
    }
    return core::Result<void>::success();
}

core::Result<void> EllipsoidProxyProfile::validate() const {
    if (!center.finite() || !radii.finite() || radii.x <= 0.0 || radii.y <= 0.0 ||
        radii.z <= 0.0 || radii.x > 1'000'000.0 || radii.y > 1'000'000.0 ||
        radii.z > 1'000'000.0 || std::abs(center.x) > 1'000'000'000.0 ||
        std::abs(center.y) > 1'000'000'000.0 || std::abs(center.z) > 1'000'000'000.0 ||
        longitude_segments < kMinLongitudeSegments ||
        longitude_segments > kMaxLongitudeSegments ||
        latitude_segments < kMinLatitudeSegments ||
        latitude_segments > kMaxLatitudeSegments) {
        return core::Result<void>::failure(validation(
            "ellipsoid proxy center, radii, or tessellation are outside their bounds"));
    }
    const std::uint64_t triangles =
        static_cast<std::uint64_t>(longitude_segments) * 2ULL *
        (static_cast<std::uint64_t>(latitude_segments) - 1ULL);
    if (triangles > kMaxTriangles) {
        return core::Result<void>::failure(validation(
            "ellipsoid proxy triangle budget is exceeded"));
    }
    return core::Result<void>::success();
}

core::Result<void> StructureRecord::validate() const {
    if (!id) return core::Result<void>::failure(invalid("biological structure id is zero"));
    if (!valid_structure_kind(kind)) {
        return core::Result<void>::failure(invalid("biological structure kind is invalid"));
    }
    if (parent.has_value() && (!*parent || *parent == id)) {
        return core::Result<void>::failure(validation(
            "biological structure parent is zero or self-referential"));
    }
    if (!safe_text(semantic_type, kMaxSemanticBytes) ||
        !safe_text(coordinate_frame, kMaxSemanticBytes)) {
        return core::Result<void>::failure(invalid(
            "biological structure semantic type or coordinate frame is unsafe"));
    }
    if (kinematic_joint.has_value() != kinematic_graph_digest.has_value()) {
        return core::Result<void>::failure(validation(
            "kinematic correspondence must bind both a joint and graph digest"));
    }
    if (kinematic_joint.has_value() &&
        (!*kinematic_joint || kinematic_graph_digest->is_zero())) {
        return core::Result<void>::failure(validation(
            "kinematic correspondence is not digest-bound"));
    }
    if (geometry_digest.has_value() && geometry_digest->is_zero()) {
        return core::Result<void>::failure(validation(
            "biological structure geometry digest cannot be zero"));
    }
    if (proxy_surface.has_value()) {
        if (kind != StructureKind::organism && kind != StructureKind::bone &&
            kind != StructureKind::cartilage && kind != StructureKind::joint &&
            kind != StructureKind::tendon && kind != StructureKind::ligament &&
            kind != StructureKind::fascia && kind != StructureKind::muscle &&
            kind != StructureKind::soft_tissue && kind != StructureKind::adipose &&
            kind != StructureKind::skin && kind != StructureKind::organ &&
            kind != StructureKind::vascular && kind != StructureKind::neural &&
            kind != StructureKind::fluid && kind != StructureKind::appendage &&
            kind != StructureKind::garment && kind != StructureKind::surface) {
            return core::Result<void>::failure(validation(
                "ellipsoid proxy geometry is unsupported for this structure kind"));
        }
        if (auto result = proxy_surface->validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> AttachmentRecord::validate() const {
    if (!id || !first_structure || !second_structure || first_structure == second_structure) {
        return core::Result<void>::failure(invalid(
            "biological attachment identities are zero or self-referential"));
    }
    if (!valid_relation_kind(relation)) {
        return core::Result<void>::failure(invalid("biological attachment relation is invalid"));
    }
    if (!safe_text(first_anchor, kMaxAnchorBytes, true) ||
        !safe_text(second_anchor, kMaxAnchorBytes, true)) {
        return core::Result<void>::failure(invalid(
            "biological attachment anchors are unsafe or exceed their bound"));
    }
    if (driver_field.has_value() && !*driver_field) {
        return core::Result<void>::failure(validation(
            "biological attachment driver field is zero"));
    }
    if (evidence_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "biological attachment evidence digest must be bound"));
    }
    return core::Result<void>::success();
}

core::Result<void> LayerRecord::validate() const {
    if (!id || !owner) {
        return core::Result<void>::failure(invalid(
            "biological layer identity or owner is zero"));
    }
    if (!valid_layer_kind(kind)) {
        return core::Result<void>::failure(invalid("biological layer kind is invalid"));
    }
    if (parent.has_value() && (!*parent || *parent == id)) {
        return core::Result<void>::failure(validation(
            "biological layer parent is zero or self-referential"));
    }
    if (!safe_text(semantic_type, kMaxSemanticBytes) || fields.empty() ||
        fields.size() > kMaxFields || !sorted_unique_ids(fields)) {
        return core::Result<void>::failure(validation(
            "biological layer semantic type or field list is invalid"));
    }
    if (family.has_value()) {
        const auto contract = family_contract(*family);
        if (!valid_layer_family(*family) || kind != contract.layer_kind ||
            fields.size() > kMaxFamilyFields || !uncertainty_digest.has_value() ||
            uncertainty_digest->is_zero() ||
            !safe_text(uncertainty_statement, kMaxUncertaintyBytes)) {
            return core::Result<void>::failure(validation(
                "biological family layer lacks its bounded fields or uncertainty evidence"));
        }
    } else if (uncertainty_digest.has_value() || !uncertainty_statement.empty()) {
        return core::Result<void>::failure(validation(
            "biological layer uncertainty must be attached to an explicit family"));
    }
    return core::Result<void>::success();
}

core::Result<void> StageRecord::validate() const {
    if (!id || !safe_text(name, kMaxTextBytes) ||
        !std::isfinite(normalized_position) || normalized_position < 0.0 ||
        normalized_position > 1.0 || field_overrides.size() > kMaxOverrides ||
        !sorted_unique_ids(field_overrides)) {
        return core::Result<void>::failure(validation(
            "biological stage is malformed or outside its bounds"));
    }
    return core::Result<void>::success();
}

core::Result<void> RepresentationVariant::validate() const {
    if (!id || !valid_representation_kind(kind) || !continuity) {
        return core::Result<void>::failure(invalid(
            "biological representation identity, kind, or continuity is invalid"));
    }
    if (artifact_digest.is_zero() || error_envelope_digest.is_zero() ||
        correspondence_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "biological representation must bind artifact, error, and correspondence digests"));
    }
    if (layers.empty() || layers.size() > kMaxLayers || !sorted_unique_ids(layers)) {
        return core::Result<void>::failure(validation(
            "biological representation layer correspondence is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> RepresentationFamily::upsert_variant(RepresentationVariant value) {
    if (auto result = value.validate(); !result) return result;
    if (id_ == 0U) {
        return core::Result<void>::failure(invalid(
            "biological representation family identity must be non-zero"));
    }
    const auto found = std::lower_bound(
        variants_.begin(), variants_.end(), value.id,
        [](const RepresentationVariant& entry, RepresentationId id) {
            return entry.id < id;
        });
    if (found != variants_.end() && found->id == value.id) {
        if (found->continuity != value.continuity || found->kind != value.kind) {
            return core::Result<void>::failure(validation(
                "biological representation replacement must preserve kind and continuity"));
        }
        *found = std::move(value);
        return core::Result<void>::success();
    }
    if (std::any_of(variants_.begin(), variants_.end(), [&](const auto& entry) {
            return entry.kind == value.kind || entry.continuity == value.continuity;
        })) {
        return core::Result<void>::failure(invalid(
            "biological representation kind or continuity is duplicated"));
    }
    if (variants_.size() >= kMaxVariants) {
        return core::Result<void>::failure(validation(
            "biological representation family has reached its bounded capacity"));
    }
    variants_.insert(found, std::move(value));
    return core::Result<void>::success();
}

const RepresentationVariant* RepresentationFamily::find(RepresentationId id) const noexcept {
    const auto found = std::lower_bound(
        variants_.begin(), variants_.end(), id,
        [](const RepresentationVariant& entry, RepresentationId value) {
            return entry.id < value;
        });
    return found != variants_.end() && found->id == id ? &*found : nullptr;
}

core::Result<void> RepresentationFamily::validate() const {
    if (id_ == 0U) {
        return core::Result<void>::failure(invalid(
            "biological representation family identity must be non-zero"));
    }
    if (variants_.empty() || variants_.size() > kMaxVariants ||
        !std::is_sorted(variants_.begin(), variants_.end(),
                        [](const auto& left, const auto& right) {
                            return left.id < right.id;
                        })) {
        return core::Result<void>::failure(validation(
            "biological representation family variants are empty, oversized, or unsorted"));
    }
    for (std::size_t index = 0U; index < variants_.size(); ++index) {
        if (auto result = variants_[index].validate(); !result) return result;
        bool duplicate = false;
        for (std::size_t previous_index = 0U; previous_index < index; ++previous_index) {
            const auto& previous = variants_[previous_index];
            if (previous.id == variants_[index].id ||
                previous.kind == variants_[index].kind ||
                previous.continuity == variants_[index].continuity) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            return core::Result<void>::failure(validation(
                "biological representation family has duplicate identity, kind, or continuity"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedPublication::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (authoring_digest.is_zero() || prepared_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "biological prepared publication must bind authoring and product digests"));
    }
    if (!safe_text(target_profile, kMaxTextBytes) ||
        !safe_text(compiler, kMaxTextBytes) || representations.empty() ||
        representations.size() > kMaxRepresentations ||
        !sorted_unique_ids(representations)) {
        return core::Result<void>::failure(validation(
            "biological prepared publication target or representation list is invalid"));
    }
    if (receipt_digest.is_zero() || receipt_digest != digest_text(canonical_without_receipt())) {
        return core::Result<void>::failure(validation(
            "biological prepared publication receipt digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string PreparedPublication::canonical_without_receipt() const {
    std::ostringstream output;
    output << "biological-prepared-publication-v1{namespace="
           << std::quoted(source.source_namespace) << ";asset=" << source.asset_id
           << ";revision=" << source.revision.value()
           << ";source=" << source.digest.hex() << ";authoring=" << authoring_digest.hex()
           << ";prepared=" << prepared_digest.hex() << ";target="
           << std::quoted(target_profile) << ";compiler=" << std::quoted(compiler)
           << ";representations=";
    for (const auto id : representations) output << id.value << ',';
    output << '}';
    return output.str();
}

core::Result<void> PreparedPublication::refresh_receipt_digest() {
    if (!source.validate() || authoring_digest.is_zero() || prepared_digest.is_zero() ||
        !safe_text(target_profile, kMaxTextBytes) || !safe_text(compiler, kMaxTextBytes) ||
        representations.empty() || representations.size() > kMaxRepresentations ||
        !sorted_unique_ids(representations)) {
        return core::Result<void>::failure(validation(
            "biological prepared publication cannot refresh an incomplete receipt"));
    }
    receipt_digest = digest_text(canonical_without_receipt());
    return core::Result<void>::success();
}

std::string PreparedPublication::serialize() const {
    std::ostringstream output;
    output << kPublicationMagic << ' ' << 1U << '\n';
    output << "SOURCE " << std::quoted(source.source_namespace) << ' ' << source.asset_id << ' '
           << source.revision.value() << ' ' << source.digest.hex() << '\n';
    output << "AUTHORING " << authoring_digest.hex() << '\n';
    output << "PREPARED " << prepared_digest.hex() << '\n';
    output << "TARGET " << std::quoted(target_profile) << '\n';
    output << "COMPILER " << std::quoted(compiler) << '\n';
    output << "REPRESENTATIONS ";
    write_id_list(output, representations);
    output << '\n';
    output << "RECEIPT " << receipt_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<PreparedPublication> PreparedPublication::deserialize(std::string_view text) {
    if (text.empty() || text.size() > 4U * 1024U * 1024U) {
        return core::Result<PreparedPublication>::failure(parse_error(
            "biological prepared publication text is empty or exceeds its bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kPublicationMagic || !read_unsigned(input, version)) {
        return core::Result<PreparedPublication>::failure(parse_error(
            "biological prepared publication framing is malformed"));
    }
    if (version != 1U) {
        return core::Result<PreparedPublication>::failure(version_mismatch(
            "biological prepared publication schema version is unsupported"));
    }
    PreparedPublication value;
    std::uint64_t revision = 0U;
    if (!require_token(input, "SOURCE") || !(input >> std::quoted(value.source.source_namespace)) ||
        !read_unsigned(input, value.source.asset_id) || !read_unsigned(input, revision) ||
        !read_digest(input, value.source.digest) || !require_token(input, "AUTHORING") ||
        !read_digest(input, value.authoring_digest) || !require_token(input, "PREPARED") ||
        !read_digest(input, value.prepared_digest) || !require_token(input, "TARGET") ||
        !(input >> std::quoted(value.target_profile)) || !require_token(input, "COMPILER") ||
        !(input >> std::quoted(value.compiler)) || !require_token(input, "REPRESENTATIONS") ||
        !read_id_list(input, PreparedPublication::kMaxRepresentations, value.representations) ||
        !require_token(input, "RECEIPT") || !read_digest(input, value.receipt_digest) ||
        !require_end(input)) {
        return core::Result<PreparedPublication>::failure(parse_error(
            "biological prepared publication record is malformed"));
    }
    value.source.revision = core::Revision{revision};
    if (auto result = value.validate(); !result) {
        return core::Result<PreparedPublication>::failure(result.error());
    }
    return core::Result<PreparedPublication>::success(std::move(value));
}

core::Result<void> BiologicalAuthoringDocument::insert_structure(StructureRecord value) {
    if (auto result = value.validate(); !result) return result;
    if (structures_.size() >= kMaxStructures || structures_.contains(value.id)) {
        return core::Result<void>::failure(validation(
            "biological structure capacity is exceeded or identity is duplicated"));
    }
    structures_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::insert_field(FieldRecord value) {
    if (auto result = value.validate(); !result) return result;
    if (fields_.size() >= kMaxFields || fields_.contains(value.id)) {
        return core::Result<void>::failure(validation(
            "biological field capacity is exceeded or identity is duplicated"));
    }
    fields_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::insert_attachment(AttachmentRecord value) {
    if (auto result = value.validate(); !result) return result;
    if (attachments_.size() >= kMaxAttachments || attachments_.contains(value.id)) {
        return core::Result<void>::failure(validation(
            "biological attachment capacity is exceeded or identity is duplicated"));
    }
    attachments_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::insert_layer(LayerRecord value) {
    if (auto result = value.validate(); !result) return result;
    if (layers_.size() >= kMaxLayers || layers_.contains(value.id)) {
        return core::Result<void>::failure(validation(
            "biological layer capacity is exceeded or identity is duplicated"));
    }
    layers_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::insert_stage(StageRecord value) {
    if (auto result = value.validate(); !result) return result;
    if (stages_.size() >= kMaxStages || stages_.contains(value.id)) {
        return core::Result<void>::failure(validation(
            "biological stage capacity is exceeded or identity is duplicated"));
    }
    stages_.emplace(value.id, std::move(value));
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::set_representation_family(
    RepresentationFamily value) {
    if (auto result = value.validate(); !result) return result;
    representations_ = std::move(value);
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!safe_text(organism_family, kMaxOrganismFamilyBytes)) {
        return core::Result<void>::failure(invalid(
            "biological organism family is empty, unsafe, or exceeds its bound"));
    }
    if (structures_.size() > kMaxStructures || fields_.size() > kMaxFields ||
        attachments_.size() > kMaxAttachments || layers_.size() > kMaxLayers ||
        stages_.size() > kMaxStages) {
        return core::Result<void>::failure(validation(
            "biological document exceeds a bounded record capacity"));
    }
    for (const auto& [id, record] : structures_) {
        if (id != record.id) {
            return core::Result<void>::failure(validation(
                "biological structure map key does not match record identity"));
        }
        if (auto result = record.validate(); !result) return result;
        if (record.parent.has_value() && !structures_.contains(*record.parent)) {
            return core::Result<void>::failure(validation(
                "biological structure parent reference is unresolved"));
        }
    }
    std::map<StructureId, std::optional<StructureId>> structure_parents;
    for (const auto& [id, record] : structures_) structure_parents.emplace(id, record.parent);
    if (cycle_in_parent_map(structure_parents)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "biological structure hierarchy contains a cycle"));
    }

    for (const auto& [id, record] : fields_) {
        if (id != record.id) {
            return core::Result<void>::failure(validation(
                "biological field map key does not match record identity"));
        }
        if (auto result = record.validate(); !result) return result;
        if (record.coverage_field.has_value() && !fields_.contains(*record.coverage_field)) {
            return core::Result<void>::failure(validation(
                "biological field coverage reference is unresolved"));
        }
    }
    std::map<FieldId, std::optional<FieldId>> field_coverage;
    for (const auto& [id, record] : fields_) {
        field_coverage.emplace(id, record.coverage_field);
    }
    if (cycle_in_parent_map(field_coverage)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "biological field coverage contains a cycle"));
    }

    for (const auto& [id, record] : attachments_) {
        if (id != record.id) {
            return core::Result<void>::failure(validation(
                "biological attachment map key does not match record identity"));
        }
        if (auto result = record.validate(); !result) return result;
        if (!structures_.contains(record.first_structure) ||
            !structures_.contains(record.second_structure)) {
            return core::Result<void>::failure(validation(
                "biological attachment references an unresolved structure"));
        }
        if (record.driver_field.has_value() && !fields_.contains(*record.driver_field)) {
            return core::Result<void>::failure(validation(
                "biological attachment driver field is unresolved"));
        }
    }

    for (const auto& [id, record] : layers_) {
        if (id != record.id) {
            return core::Result<void>::failure(validation(
                "biological layer map key does not match record identity"));
        }
        if (auto result = record.validate(); !result) return result;
        if (!structures_.contains(record.owner)) {
            return core::Result<void>::failure(validation(
                "biological layer owner is unresolved"));
        }
        if (record.parent.has_value() && !layers_.contains(*record.parent)) {
            return core::Result<void>::failure(validation(
                "biological layer parent is unresolved"));
        }
        if (std::any_of(record.fields.begin(), record.fields.end(), [&](FieldId field) {
                return !fields_.contains(field);
            })) {
            return core::Result<void>::failure(validation(
                "biological layer field reference is unresolved"));
        }
        if (record.family.has_value()) {
            const auto contract = family_contract(*record.family);
            for (const FieldKind required_kind : contract.required_fields) {
                const bool present = std::any_of(record.fields.begin(), record.fields.end(),
                    [&](FieldId field_id) {
                        return fields_.at(field_id).kind == required_kind;
                    });
                if (!present) {
                    return core::Result<void>::failure(validation(
                        "biological family layer is missing a required source field kind"));
                }
            }
            if (std::any_of(record.fields.begin(), record.fields.end(),
                    [&](FieldId field_id) {
                        const auto& field = fields_.at(field_id);
                        return !family_field_value_kind_matches(field) ||
                            field.data_digest == *record.uncertainty_digest;
                    })) {
                return core::Result<void>::failure(validation(
                    "biological family field type or uncertainty lineage is invalid"));
            }
        }
    }
    std::map<LayerId, std::optional<LayerId>> layer_parents;
    for (const auto& [id, record] : layers_) layer_parents.emplace(id, record.parent);
    if (cycle_in_parent_map(layer_parents)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::cycle_detected, "biological layer hierarchy contains a cycle"));
    }

    for (const auto& [id, record] : stages_) {
        if (id != record.id) {
            return core::Result<void>::failure(validation(
                "biological stage map key does not match record identity"));
        }
        if (auto result = record.validate(); !result) return result;
        if (std::any_of(record.field_overrides.begin(), record.field_overrides.end(),
                        [&](FieldId field) { return !fields_.contains(field); })) {
            return core::Result<void>::failure(validation(
                "biological stage field override is unresolved"));
        }
    }

    if (representations_.has_value()) {
        if (auto result = representations_->validate(); !result) return result;
        for (const auto& variant : representations_->variants()) {
            if (std::any_of(variant.layers.begin(), variant.layers.end(),
                            [&](LayerId layer) { return !layers_.contains(layer); })) {
                return core::Result<void>::failure(validation(
                    "biological representation layer correspondence is unresolved"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> BiologicalAuthoringDocument::validate_for_publication() const {
    if (auto result = validate(); !result) return result;
    if (!representations_.has_value()) {
        return core::Result<void>::failure(validation(
            "biological publication requires a representation family"));
    }
    if (representations_->variants().empty()) {
        return core::Result<void>::failure(validation(
            "biological publication requires at least one representation variant"));
    }
    return core::Result<void>::success();
}

std::string BiologicalAuthoringDocument::serialize() const {
    const bool has_family_records = std::any_of(layers_.begin(), layers_.end(),
        [](const auto& entry) { return entry.second.family.has_value(); });
    // Preserve the exact v2 canonical bytes for legacy/generic authoring
    // documents so their prepared-product digests do not change merely because
    // the optional family contract was added.
    const std::uint32_t encoded_version = has_family_records ? 3U : 2U;
    std::ostringstream output;
    output << kDocumentMagic << ' ' << encoded_version << '\n';
    output << "SOURCE " << std::quoted(source.source_namespace) << ' ' << source.asset_id << ' '
           << source.revision.value() << ' ' << source.digest.hex() << '\n';
    output << "ORGANISM " << std::quoted(organism_family) << '\n';
    output << "STRUCTURES " << structures_.size() << '\n';
    for (const auto& [ignored, record] : structures_) {
        static_cast<void>(ignored);
        output << "STRUCTURE " << record.id.value << ' ' << structure_kind_name(record.kind)
               << ' ';
        write_optional_id(output, record.parent);
        output << ' ' << std::quoted(record.semantic_type) << ' '
               << std::quoted(record.coordinate_frame) << ' '
               << (record.kinematic_joint.has_value() ? record.kinematic_joint->value : 0U)
               << ' ';
        write_optional_digest(output, record.kinematic_graph_digest);
        output << ' ';
        write_optional_digest(output, record.geometry_digest);
        output << ' ' << (record.proxy_surface.has_value() ? 1U : 0U);
        if (record.proxy_surface.has_value()) {
            const auto& profile = *record.proxy_surface;
            output << ' ' << std::setprecision(17)
                   << profile.center.x << ' ' << profile.center.y << ' '
                   << profile.center.z << ' ' << profile.radii.x << ' '
                   << profile.radii.y << ' ' << profile.radii.z << ' '
                   << profile.longitude_segments << ' ' << profile.latitude_segments;
        }
        output << '\n';
    }
    output << "FIELDS " << fields_.size() << '\n';
    for (const auto& [ignored, record] : fields_) {
        static_cast<void>(ignored);
        output << "FIELD " << record.id.value << ' ' << field_kind_name(record.kind) << ' '
               << field_value_kind_name(record.value_kind) << ' '
               << field_representation_name(record.representation) << ' '
               << std::quoted(record.domain) << ' ' << std::quoted(record.coordinate_frame)
               << ' ' << std::quoted(record.units) << ' ' << record.sample_count << ' '
               << record.stable_seed << ' ';
        write_optional_id(output, record.coverage_field);
        output << ' ' << record.data_digest.hex() << ' ' << std::setprecision(17)
               << record.minimum << ' ' << record.maximum << '\n';
    }
    output << "ATTACHMENTS " << attachments_.size() << '\n';
    for (const auto& [ignored, record] : attachments_) {
        static_cast<void>(ignored);
        output << "ATTACHMENT " << record.id.value << ' ' << record.first_structure.value << ' '
               << record.second_structure.value << ' ' << relation_kind_name(record.relation)
               << ' ' << std::quoted(record.first_anchor) << ' '
               << std::quoted(record.second_anchor) << ' ';
        write_optional_id(output, record.driver_field);
        output << ' ' << record.evidence_digest.hex() << '\n';
    }
    output << "LAYERS " << layers_.size() << '\n';
    for (const auto& [ignored, record] : layers_) {
        static_cast<void>(ignored);
        output << "LAYER " << record.id.value << ' ' << layer_kind_name(record.kind) << ' '
               << record.owner.value << ' ';
        write_optional_id(output, record.parent);
        output << ' ' << record.order << ' ' << std::quoted(record.semantic_type) << ' ';
        write_id_list(output, record.fields);
        if (encoded_version >= 3U) {
            output << ' ';
            if (record.family.has_value()) {
                output << layer_family_name(*record.family);
            } else {
                output << "none";
            }
            output << ' ';
            write_optional_digest(output, record.uncertainty_digest);
            output << ' ' << std::quoted(record.uncertainty_statement);
        }
        output << '\n';
    }
    output << "STAGES " << stages_.size() << '\n';
    for (const auto& [ignored, record] : stages_) {
        static_cast<void>(ignored);
        output << "STAGE " << record.id.value << ' ' << std::quoted(record.name) << ' '
               << std::setprecision(17) << record.normalized_position << ' ';
        write_id_list(output, record.field_overrides);
        output << '\n';
    }
    output << "REPRESENTATIONS " << (representations_.has_value() ? 1U : 0U) << '\n';
    if (representations_.has_value()) {
        output << "FAMILY " << representations_->id() << ' '
               << representations_->variants().size() << '\n';
        for (const auto& record : representations_->variants()) {
            output << "VARIANT " << record.id.value << ' '
                   << representation_kind_name(record.kind) << ' '
                   << record.artifact_digest.hex() << ' ' << record.error_envelope_digest.hex()
                   << ' ' << record.correspondence_digest.hex() << ' ' << record.resident_bytes
                   << ' ' << record.continuity.value << ' ';
            write_id_list(output, record.layers);
            output << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> BiologicalAuthoringDocument::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<BiologicalAuthoringDocument> BiologicalAuthoringDocument::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological authoring text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kDocumentMagic || !read_unsigned(input, version)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological authoring framing is malformed"));
    }
    if (version == 0U || version > kSchemaVersion) {
        return core::Result<BiologicalAuthoringDocument>::failure(version_mismatch(
            "biological authoring schema version is unsupported"));
    }
    BiologicalAuthoringDocument document;
    std::uint64_t revision = 0U;
    if (!require_token(input, "SOURCE") || !(input >> std::quoted(document.source.source_namespace)) ||
        !read_unsigned(input, document.source.asset_id) || !read_unsigned(input, revision) ||
        !read_digest(input, document.source.digest) || !require_token(input, "ORGANISM") ||
        !(input >> std::quoted(document.organism_family))) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological authoring source record is malformed"));
    }
    document.source.revision = core::Revision{revision};

    std::size_t count = 0U;
    if (!require_token(input, "STRUCTURES") ||
        !read_count(input, kMaxStructures, count)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological structure count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        StructureRecord record;
        std::string kind;
        std::uint64_t kinematic_joint = 0U;
        if (!require_token(input, "STRUCTURE") || !read_unsigned(input, record.id.value) ||
            !(input >> kind) || !read_optional_id(input, record.parent) ||
            !(input >> std::quoted(record.semantic_type)) ||
            !(input >> std::quoted(record.coordinate_frame)) ||
            !read_unsigned(input, kinematic_joint) ||
            !read_optional_digest(input, record.kinematic_graph_digest) ||
            !read_optional_digest(input, record.geometry_digest)) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological structure record is malformed"));
        }
        if (version >= 2U) {
            unsigned has_proxy_surface = 0U;
            if (!read_unsigned(input, has_proxy_surface) || has_proxy_surface > 1U) {
                return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                    "biological structure proxy-profile marker is malformed"));
            }
            if (has_proxy_surface != 0U) {
                EllipsoidProxyProfile profile;
                if (!(input >> profile.center.x >> profile.center.y >> profile.center.z
                      >> profile.radii.x >> profile.radii.y >> profile.radii.z) ||
                    !read_unsigned(input, profile.longitude_segments) ||
                    !read_unsigned(input, profile.latitude_segments)) {
                    return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                        "biological structure proxy profile is malformed"));
                }
                record.proxy_surface = profile;
            }
        }
        const auto parsed_kind = structure_kind_from_name(kind);
        if (!parsed_kind) return core::Result<BiologicalAuthoringDocument>::failure(parsed_kind.error());
        record.kind = parsed_kind.value();
        if (kinematic_joint != 0U) record.kinematic_joint = kinematic::JointId{kinematic_joint};
        if (auto result = document.insert_structure(std::move(record)); !result) {
            return core::Result<BiologicalAuthoringDocument>::failure(result.error());
        }
    }

    if (!require_token(input, "FIELDS") || !read_count(input, kMaxFields, count)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological field count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        FieldRecord record;
        std::string field_kind;
        std::string value_kind;
        std::string representation;
        if (!require_token(input, "FIELD") || !read_unsigned(input, record.id.value) ||
            !(input >> field_kind >> value_kind >> representation) ||
            !(input >> std::quoted(record.domain)) ||
            !(input >> std::quoted(record.coordinate_frame)) ||
            !(input >> std::quoted(record.units)) || !read_unsigned(input, record.sample_count) ||
            !read_unsigned(input, record.stable_seed) ||
            !read_optional_id(input, record.coverage_field) ||
            !read_digest(input, record.data_digest) || !(input >> record.minimum >> record.maximum)) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological field record is malformed"));
        }
        const auto parsed_field_kind = field_kind_from_name(field_kind);
        const auto parsed_value_kind = field_value_kind_from_name(value_kind);
        const auto parsed_representation = field_representation_from_name(representation);
        if (!parsed_field_kind || !parsed_value_kind || !parsed_representation) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological field enum is malformed"));
        }
        record.kind = parsed_field_kind.value();
        record.value_kind = parsed_value_kind.value();
        record.representation = parsed_representation.value();
        if (auto result = document.insert_field(std::move(record)); !result) {
            return core::Result<BiologicalAuthoringDocument>::failure(result.error());
        }
    }

    if (!require_token(input, "ATTACHMENTS") ||
        !read_count(input, kMaxAttachments, count)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological attachment count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        AttachmentRecord record;
        std::string relation;
        if (!require_token(input, "ATTACHMENT") || !read_unsigned(input, record.id.value) ||
            !read_unsigned(input, record.first_structure.value) ||
            !read_unsigned(input, record.second_structure.value) || !(input >> relation) ||
            !(input >> std::quoted(record.first_anchor)) ||
            !(input >> std::quoted(record.second_anchor)) ||
            !read_optional_id(input, record.driver_field) ||
            !read_digest(input, record.evidence_digest)) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological attachment record is malformed"));
        }
        const auto parsed_relation = relation_kind_from_name(relation);
        if (!parsed_relation) return core::Result<BiologicalAuthoringDocument>::failure(parsed_relation.error());
        record.relation = parsed_relation.value();
        if (auto result = document.insert_attachment(std::move(record)); !result) {
            return core::Result<BiologicalAuthoringDocument>::failure(result.error());
        }
    }

    if (!require_token(input, "LAYERS") || !read_count(input, kMaxLayers, count)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological layer count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        LayerRecord record;
        std::string kind;
        if (!require_token(input, "LAYER") || !read_unsigned(input, record.id.value) ||
            !(input >> kind) || !read_unsigned(input, record.owner.value) ||
            !read_optional_id(input, record.parent) || !read_unsigned(input, record.order) ||
            !(input >> std::quoted(record.semantic_type)) ||
            !read_id_list(input, LayerRecord::kMaxFields, record.fields)) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological layer record is malformed"));
        }
        if (version >= 3U) {
            std::string family;
            if (!(input >> family) ||
                !read_optional_digest(input, record.uncertainty_digest) ||
                !(input >> std::quoted(record.uncertainty_statement))) {
                return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                    "biological layer family uncertainty record is malformed"));
            }
            if (family != "none") {
                const auto parsed_family = layer_family_from_name(family);
                if (!parsed_family) {
                    return core::Result<BiologicalAuthoringDocument>::failure(
                        parsed_family.error());
                }
                record.family = parsed_family.value();
            }
        }
        const auto parsed_kind = layer_kind_from_name(kind);
        if (!parsed_kind) return core::Result<BiologicalAuthoringDocument>::failure(parsed_kind.error());
        record.kind = parsed_kind.value();
        if (auto result = document.insert_layer(std::move(record)); !result) {
            return core::Result<BiologicalAuthoringDocument>::failure(result.error());
        }
    }

    if (!require_token(input, "STAGES") || !read_count(input, kMaxStages, count)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological stage count is malformed"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        StageRecord record;
        if (!require_token(input, "STAGE") || !read_unsigned(input, record.id.value) ||
            !(input >> std::quoted(record.name)) || !(input >> record.normalized_position) ||
            !read_id_list(input, StageRecord::kMaxOverrides, record.field_overrides)) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological stage record is malformed"));
        }
        if (auto result = document.insert_stage(std::move(record)); !result) {
            return core::Result<BiologicalAuthoringDocument>::failure(result.error());
        }
    }

    std::size_t representation_section = 0U;
    if (!require_token(input, "REPRESENTATIONS") ||
        !read_count(input, 1U, representation_section)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological representation section is malformed"));
    }
    if (representation_section == 1U) {
        RepresentationFamily family;
        std::size_t variant_count = 0U;
        if (!require_token(input, "FAMILY") || !read_unsigned(input, family.id_) ||
            !read_count(input, RepresentationFamily::kMaxVariants, variant_count)) {
            return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                "biological representation family header is malformed"));
        }
        for (std::size_t index = 0U; index < variant_count; ++index) {
            RepresentationVariant record;
            std::string kind;
            if (!require_token(input, "VARIANT") || !read_unsigned(input, record.id.value) ||
                !(input >> kind) || !read_digest(input, record.artifact_digest) ||
                !read_digest(input, record.error_envelope_digest) ||
                !read_digest(input, record.correspondence_digest) ||
                !read_unsigned(input, record.resident_bytes) ||
                !read_unsigned(input, record.continuity.value) ||
                !read_id_list(input, RepresentationVariant::kMaxLayers, record.layers)) {
                return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
                    "biological representation variant is malformed"));
            }
            const auto parsed_kind = representation_kind_from_name(kind);
            if (!parsed_kind) return core::Result<BiologicalAuthoringDocument>::failure(parsed_kind.error());
            record.kind = parsed_kind.value();
            if (auto result = family.upsert_variant(std::move(record)); !result) {
                return core::Result<BiologicalAuthoringDocument>::failure(result.error());
            }
        }
        if (auto result = document.set_representation_family(std::move(family)); !result) {
            return core::Result<BiologicalAuthoringDocument>::failure(result.error());
        }
    }

    if (!require_end(input)) {
        return core::Result<BiologicalAuthoringDocument>::failure(parse_error(
            "biological authoring document is missing END or contains trailing data"));
    }
    if (auto result = document.validate(); !result) {
        return core::Result<BiologicalAuthoringDocument>::failure(result.error());
    }
    return core::Result<BiologicalAuthoringDocument>::success(std::move(document));
}

core::Result<void> verify_reimport_continuity(
    const BiologicalAuthoringDocument& before,
    const BiologicalAuthoringDocument& after) {
    if (auto result = before.validate(); !result) return result;
    if (auto result = after.validate(); !result) return result;
    if (before.source.source_namespace != after.source.source_namespace ||
        before.source.asset_id != after.source.asset_id) {
        return core::Result<void>::failure(stale(
            "biological reimport changed the source namespace or asset identity"));
    }
    if (after.source.revision < before.source.revision ||
        (after.source.revision == before.source.revision &&
         after.source.digest != before.source.digest)) {
        return core::Result<void>::failure(stale(
            "biological reimport revision or digest regressed"));
    }
    for (const auto& [id, ignored] : before.structures()) {
        static_cast<void>(ignored);
        if (!after.structures().contains(id)) {
            return core::Result<void>::failure(stale(
                "biological reimport removed a previously authored structure identity"));
        }
    }
    for (const auto& [id, ignored] : before.fields()) {
        static_cast<void>(ignored);
        if (!after.fields().contains(id)) {
            return core::Result<void>::failure(stale(
                "biological reimport removed a previously authored field identity"));
        }
    }
    for (const auto& [id, ignored] : before.layers()) {
        static_cast<void>(ignored);
        if (!after.layers().contains(id)) {
            return core::Result<void>::failure(stale(
                "biological reimport removed a previously authored layer identity"));
        }
    }
    for (const auto& [id, ignored] : before.attachments()) {
        static_cast<void>(ignored);
        if (!after.attachments().contains(id)) {
            return core::Result<void>::failure(stale(
                "biological reimport removed a previously authored attachment identity"));
        }
    }
    for (const auto& [id, ignored] : before.stages()) {
        static_cast<void>(ignored);
        if (!after.stages().contains(id)) {
            return core::Result<void>::failure(stale(
                "biological reimport removed a previously authored stage identity"));
        }
    }
    if (before.representations().has_value()) {
        if (!after.representations().has_value()) {
            return core::Result<void>::failure(stale(
                "biological reimport removed the representation family"));
        }
        for (const auto& variant : before.representations()->variants()) {
            const auto replacement = after.representations()->find(variant.id);
            if (replacement == nullptr || replacement->kind != variant.kind ||
                replacement->continuity != variant.continuity) {
                return core::Result<void>::failure(stale(
                    "biological reimport changed representation continuity"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_prepared_publication(
    const BiologicalAuthoringDocument& document,
    const PreparedPublication& publication) {
    if (auto result = document.validate_for_publication(); !result) return result;
    if (auto result = publication.validate(); !result) return result;
    if (publication.source != document.source) {
        return core::Result<void>::failure(stale(
            "biological prepared publication source identity is stale"));
    }
    const auto digest = document.canonical_digest();
    if (!digest || publication.authoring_digest != digest.value()) {
        return core::Result<void>::failure(stale(
            "biological prepared publication authoring digest is stale"));
    }
    for (const auto id : publication.representations) {
        if (document.representations()->find(id) == nullptr) {
            return core::Result<void>::failure(validation(
                "biological prepared publication references an unknown representation"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::biological
