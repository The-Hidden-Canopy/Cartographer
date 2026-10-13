#include <carto/condition/authoring.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <istream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::condition {
namespace {

constexpr std::string_view kAuthoringMagic = "CARTOGRAPHER_CONDITION_AUTHORING";
constexpr std::string_view kPackageMagic = "CARTOGRAPHER_CONDITION_PACKAGE";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
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

bool safe_locator(std::string_view value) {
    if (!safe_text(value, 512U)) return false;
    if (value.front() == '/' || value.front() == '\\' || value.find("..") != std::string_view::npos ||
        (value.size() >= 2U && value[1] == ':')) {
        return false;
    }
    return value.find('\\') == std::string_view::npos;
}

bool safe_namespace(std::string_view value) {
    return !value.empty() && value.size() <= 128U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
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

template <typename Enum>
bool read_named(
    std::istream& input,
    std::initializer_list<std::pair<Enum, std::string_view>> values,
    Enum& result) {
    std::string token;
    if (!(input >> token)) return false;
    for (const auto& [value, name] : values) {
        if (token == name) {
            result = value;
            return true;
        }
    }
    return false;
}

template <typename Enum>
void write_named(std::ostringstream& output, Enum value, std::string_view name) {
    output << name;
    static_cast<void>(value);
}

template <typename IdType>
bool sorted_unique_ids(const std::vector<IdType>& ids) {
    return std::all_of(ids.begin(), ids.end(), [](IdType id) { return id != 0U; }) &&
        std::is_sorted(ids.begin(), ids.end()) &&
        std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}

bool sorted_unique_strings(const std::vector<std::string>& values) {
    return std::is_sorted(values.begin(), values.end()) &&
        std::adjacent_find(values.begin(), values.end()) == values.end();
}

template <typename Record>
core::Result<void> validate_record_vector(const std::vector<Record>& records, std::string_view label) {
    Id previous = 0U;
    for (const auto& record : records) {
        if (record.id == 0U || (previous != 0U && record.id <= previous)) {
            return core::Result<void>::failure(validation(
                std::string(label) + " identities must be sorted, unique, and nonzero"));
        }
        previous = record.id;
        if (auto result = record.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(std::string(label)));
        }
    }
    return core::Result<void>::success();
}

bool finite_between(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool finite_positive_vec(core::Vec3d value) {
    return value.finite() && value.x > 0.0 && value.y > 0.0 && value.z > 0.0;
}

void write_transform(std::ostringstream& output, const core::Transform& transform) {
    output << std::setprecision(17)
           << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(input >> transform.translation.x >> transform.translation.y >>
        transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
        transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
        transform.scale.y >> transform.scale.z);
}

template <typename T>
void write_ids(std::ostringstream& output, const std::vector<T>& values) {
    output << values.size();
    for (const auto value : values) output << ' ' << value;
}

bool read_ids(std::istream& input, std::vector<Id>& values) {
    std::size_t count = 0U;
    if (!read_count(input, AuthoringDocument::kMaxRecordsPerKind, count)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        Id value = 0U;
        if (!read_unsigned(input, value)) return false;
        values.push_back(value);
    }
    return true;
}

void write_strings(std::ostringstream& output, const std::vector<std::string>& values) {
    output << values.size();
    for (const auto& value : values) output << ' ' << std::quoted(value);
}

bool read_strings(std::istream& input, std::vector<std::string>& values) {
    std::size_t count = 0U;
    if (!read_count(input, AuthoringDocument::kMaxRecordsPerKind, count)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::string value;
        if (!(input >> std::quoted(value))) return false;
        values.push_back(std::move(value));
    }
    return true;
}

template <typename Enum>
void write_enums(std::ostringstream& output, const std::vector<Enum>& values,
                 std::string_view (*name)(Enum) noexcept) {
    output << values.size();
    for (const auto value : values) output << ' ' << name(value);
}

template <typename Enum>
bool read_enums(std::istream& input, std::vector<Enum>& values,
                std::initializer_list<std::pair<Enum, std::string_view>> names) {
    std::size_t count = 0U;
    if (!read_count(input, AuthoringDocument::kMaxRecordsPerKind, count)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        Enum value{};
        if (!read_named(input, names, value)) return false;
        values.push_back(value);
    }
    return true;
}

template <typename Enum>
bool valid_enum(Enum value, std::string_view (*name)(Enum) noexcept) {
    return name(value) != "unknown";
}

} // namespace

std::string_view source_kind_name(SourceKind value) noexcept {
    switch (value) {
    case SourceKind::authored_reference: return "authored-reference";
    case SourceKind::imported_asset: return "imported-asset";
    case SourceKind::procedural_recipe: return "procedural-recipe";
    case SourceKind::material_reference: return "material-reference";
    case SourceKind::authored_fixture: return "authored-fixture";
    }
    return "unknown";
}

std::string_view asset_class_name(AssetClass value) noexcept {
    switch (value) {
    case AssetClass::architecture: return "architecture";
    case AssetClass::prop: return "prop";
    case AssetClass::vehicle: return "vehicle";
    case AssetClass::vegetation: return "vegetation";
    case AssetClass::terrain: return "terrain";
    case AssetClass::water: return "water";
    case AssetClass::fixture: return "fixture";
    case AssetClass::character: return "character";
    case AssetClass::wildlife: return "wildlife";
    case AssetClass::material: return "material";
    }
    return "unknown";
}

std::string_view fidelity_class_name(FidelityClass value) noexcept {
    switch (value) {
    case FidelityClass::hero: return "hero";
    case FidelityClass::familiarity_critical: return "familiarity-critical";
    case FidelityClass::standard: return "standard";
    case FidelityClass::background: return "background";
    }
    return "unknown";
}

std::string_view variant_kind_name(VariantKind value) noexcept {
    switch (value) {
    case VariantKind::morph: return "morph";
    case VariantKind::presentation: return "presentation";
    }
    return "unknown";
}

std::string_view temporal_phase_name(TemporalPhase value) noexcept {
    switch (value) {
    case TemporalPhase::day: return "day";
    case TemporalPhase::late_day: return "late-day";
    case TemporalPhase::dusk: return "dusk";
    case TemporalPhase::blue_hour: return "blue-hour";
    case TemporalPhase::night: return "night";
    case TemporalPhase::pre_dawn: return "pre-dawn";
    case TemporalPhase::dawn: return "dawn";
    }
    return "unknown";
}

std::string_view occupancy_state_name(OccupancyState value) noexcept {
    switch (value) {
    case OccupancyState::empty: return "empty";
    case OccupancyState::recently_used: return "recently-used";
    case OccupancyState::actively_occupied: return "actively-occupied";
    case OccupancyState::long_term_occupied: return "long-term-occupied";
    case OccupancyState::abandoned: return "abandoned";
    }
    return "unknown";
}

std::string_view maintenance_state_name(MaintenanceState value) noexcept {
    switch (value) {
    case MaintenanceState::maintained: return "maintained";
    case MaintenanceState::aging: return "aging";
    case MaintenanceState::neglected: return "neglected";
    case MaintenanceState::improvised_repair: return "improvised-repair";
    case MaintenanceState::partially_renovated: return "partially-renovated";
    case MaintenanceState::condemned: return "condemned";
    }
    return "unknown";
}

std::string_view damage_state_name(DamageState value) noexcept {
    switch (value) {
    case DamageState::intact: return "intact";
    case DamageState::broken_window: return "broken-window";
    case DamageState::damaged_door: return "damaged-door";
    case DamageState::dented_sign: return "dented-sign";
    case DamageState::collapsed_shelf: return "collapsed-shelf";
    case DamageState::cracked_wall: return "cracked-wall";
    case DamageState::damaged_railing: return "damaged-railing";
    case DamageState::roof_damage: return "roof-damage";
    case DamageState::collision_damage: return "collision-damage";
    case DamageState::burned: return "burned";
    }
    return "unknown";
}

std::string_view anchor_kind_name(AnchorKind value) noexcept {
    switch (value) {
    case AnchorKind::interaction: return "interaction";
    case AnchorKind::approach: return "approach";
    case AnchorKind::seat: return "seat";
    case AnchorKind::sleep: return "sleep";
    case AnchorKind::traversal: return "traversal";
    case AnchorKind::audio_source: return "audio-source";
    }
    return "unknown";
}

core::Result<void> SourceEvidence::validate() const {
    if (id == 0U || !valid_enum(kind, source_kind_name) || !safe_text(label, 256U) ||
        !safe_locator(logical_locator) || !safe_text(revision, 128U) || digest.is_zero() ||
        !safe_text(producer_identity, 128U) || !safe_text(producer_version, 128U) ||
        import_settings_digest.is_zero() || diagnostics.size() > 256U ||
        !std::all_of(diagnostics.begin(), diagnostics.end(), [](const std::string& value) {
            return safe_text(value, 512U);
        })) {
        return core::Result<void>::failure(validation("condition source evidence is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SourceAsset::validate() const {
    if (id == 0U || !safe_text(stable_name, 256U) || !valid_enum(asset_class, asset_class_name) ||
        !valid_enum(fidelity, fidelity_class_name) || source_id == 0U ||
        !safe_text(source_revision, 128U) || source_digest.is_zero() ||
        geometry_digest.is_zero() || material_digest.is_zero() || tags.size() > 256U ||
        protected_features.size() > 256U || !sorted_unique_strings(tags) ||
        !sorted_unique_strings(protected_features)) {
        return core::Result<void>::failure(validation("condition source asset is invalid"));
    }
    for (const auto& tag : tags) {
        if (!safe_text(tag, 128U)) {
            return core::Result<void>::failure(validation("condition asset tag is invalid"));
        }
    }
    for (const auto& feature : protected_features) {
        if (!safe_text(feature, 128U)) {
            return core::Result<void>::failure(validation("condition protected feature is invalid"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SafePlaceProfile::validate() const {
    if (id == 0U || asset_id == 0U || !finite_between(privacy, 0.0, 1.0) ||
        !finite_between(human_proximity, 0.0, 1.0) ||
        !finite_between(controllable_entries, 0.0, 1.0) ||
        !finite_between(supply_access, 0.0, 1.0) ||
        !finite_between(ambient_noise, 0.0, 1.0) ||
        !finite_between(visibility_outward, 0.0, 1.0) ||
        !finite_between(visibility_inward, 0.0, 1.0) ||
        !finite_between(isolation, 0.0, 1.0)) {
        return core::Result<void>::failure(validation("condition Safe Place profile is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> FamiliarityRecord::validate() const {
    if (id == 0U || asset_id == 0U || !safe_text(semantic_id, 256U) ||
        !finite_between(importance, 0.0, 1.0) || !baseline_transform.finite() ||
        !sorted_unique_ids(expected_neighbors) || expected_neighbors.size() > 4096U ||
        !safe_text(expected_state, 256U, true)) {
        return core::Result<void>::failure(validation("condition familiarity record is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ViewCondition::validate() const {
    if (id == 0U || asset_id == 0U || !finite_between(minimum_distance, 0.0, 1.0e9) ||
        !finite_between(maximum_distance, minimum_distance, 1.0e9) ||
        !finite_between(minimum_elevation_degrees, -90.0, 90.0) ||
        !finite_between(maximum_elevation_degrees, minimum_elevation_degrees, 90.0) ||
        !finite_between(cone_degrees, 0.0, 180.0) ||
        !finite_between(minimum_visibility_fraction, 0.0, 1.0) ||
        !safe_text(lighting_condition, 128U, true)) {
        return core::Result<void>::failure(validation("condition view condition is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> VariantCandidate::validate() const {
    if (id == 0U || asset_id == 0U || !safe_text(name, 256U) ||
        !valid_enum(kind, variant_kind_name) || !std::isfinite(maximum_transform_delta) ||
        maximum_transform_delta < 0.0 || !safe_text(semantic_id, 256U, true)) {
        return core::Result<void>::failure(validation("condition variant candidate is invalid"));
    }
    if (kind == VariantKind::presentation &&
        (!projection_only || collision_impact_declared || navigation_impact_declared ||
         interaction_impact_declared)) {
        return core::Result<void>::failure(validation(
            "presentation variant must be projection-only and cannot declare physical impact"));
    }
    if (kind == VariantKind::morph && projection_only) {
        return core::Result<void>::failure(validation(
            "morph variant cannot be marked projection-only"));
    }
    return core::Result<void>::success();
}

core::Result<void> TemporalVisualPackage::validate() const {
    if (id == 0U || asset_id == 0U || !valid_enum(phase, temporal_phase_name) ||
        !sorted_unique_ids(variant_ids) || variant_ids.size() > 4096U ||
        !safe_text(material_variant, 256U, true) || !safe_text(source_alternate, 256U, true)) {
        return core::Result<void>::failure(validation("condition temporal visual package is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceStateRecord::validate() const {
    if (id == 0U || asset_id == 0U || !valid_enum(occupancy, occupancy_state_name) ||
        !valid_enum(maintenance, maintenance_state_name) || !valid_enum(damage, damage_state_name) ||
        !finite_between(wetness, 0.0, 1.0) || wear_tags.size() > 256U ||
        repair_tags.size() > 256U || !sorted_unique_strings(wear_tags) ||
        !sorted_unique_strings(repair_tags) || !safe_text(provenance_note, 1024U, true)) {
        return core::Result<void>::failure(validation("condition surface state is invalid"));
    }
    for (const auto& value : wear_tags) {
        if (!safe_text(value, 128U)) {
            return core::Result<void>::failure(validation("condition wear tag is invalid"));
        }
    }
    for (const auto& value : repair_tags) {
        if (!safe_text(value, 128U)) {
            return core::Result<void>::failure(validation("condition repair tag is invalid"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> InteractionAnchor::validate() const {
    if (id == 0U || asset_id == 0U || !valid_enum(kind, anchor_kind_name) ||
        !safe_text(interaction_class, 128U) || !transform.finite() ||
        !approach_direction.finite() || approach_direction.length_squared() <= 1.0e-12 ||
        !finite_positive_vec(clearance)) {
        return core::Result<void>::failure(validation("condition interaction anchor is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> CollectionRecord::validate() const {
    if (id == 0U || !safe_text(name, 256U) || !sorted_unique_ids(asset_ids) ||
        !sorted_unique_ids(child_collection_ids) || child_collection_ids.size() > 4096U ||
        asset_ids.size() > 65'536U || !safe_text(semantic_class, 128U, true)) {
        return core::Result<void>::failure(validation("condition collection is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PrefabRecord::validate() const {
    if (id == 0U || !safe_text(name, 256U) || !sorted_unique_ids(asset_ids) ||
        !sorted_unique_strings(sockets) || !sorted_unique_ids(variant_ids) ||
        asset_ids.size() > 65'536U || sockets.size() > 4096U || variant_ids.size() > 65'536U) {
        return core::Result<void>::failure(validation("condition prefab is invalid"));
    }
    for (const auto& socket : sockets) {
        if (!safe_text(socket, 128U)) {
            return core::Result<void>::failure(validation("condition prefab socket is invalid"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> RegionKitManifest::validate() const {
    if (id == 0U || !safe_text(name, 256U) || !sorted_unique_ids(asset_ids) ||
        !sorted_unique_ids(prefab_ids) || asset_ids.size() > 65'536U ||
        prefab_ids.size() > 65'536U || !std::is_sorted(required_classes.begin(), required_classes.end()) ||
        std::adjacent_find(required_classes.begin(), required_classes.end()) != required_classes.end() ||
        !std::all_of(required_classes.begin(), required_classes.end(),
            [](AssetClass value) { return valid_enum(value, asset_class_name); }) ||
        !finite_between(interaction_coverage, 0.0, 1.0) ||
        !finite_between(familiarity_coverage, 0.0, 1.0)) {
        return core::Result<void>::failure(validation("condition region kit manifest is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> AuthoringDocument::validate() const {
    if (!safe_namespace(project_namespace) || project_revision.exhausted()) {
        return core::Result<void>::failure(validation("condition authoring project identity is invalid"));
    }
    const auto validate_count = [](std::size_t count, std::string_view label) -> core::Result<void> {
        if (count > kMaxRecordsPerKind) {
            return core::Result<void>::failure(validation(
                std::string(label) + " exceeds the bounded source-authoring limit"));
        }
        return core::Result<void>::success();
    };
    if (auto result = validate_count(sources.size(), "condition sources"); !result) return result;
    if (auto result = validate_count(assets.size(), "condition assets"); !result) return result;
    if (auto result = validate_count(safe_places.size(), "condition Safe Places"); !result) return result;
    if (auto result = validate_count(familiarity.size(), "condition familiarity records"); !result) return result;
    if (auto result = validate_count(view_conditions.size(), "condition view conditions"); !result) return result;
    if (auto result = validate_count(variants.size(), "condition variants"); !result) return result;
    if (auto result = validate_count(temporal_packages.size(), "condition temporal packages"); !result) return result;
    if (auto result = validate_count(surface_states.size(), "condition surface states"); !result) return result;
    if (auto result = validate_count(interaction_anchors.size(), "condition anchors"); !result) return result;
    if (auto result = validate_count(collections.size(), "condition collections"); !result) return result;
    if (auto result = validate_count(prefabs.size(), "condition prefabs"); !result) return result;
    if (auto result = validate_count(region_kits.size(), "condition region kits"); !result) return result;

    if (auto result = validate_record_vector(sources, "condition source evidence"); !result) return result;
    if (auto result = validate_record_vector(assets, "condition source assets"); !result) return result;
    if (auto result = validate_record_vector(safe_places, "condition Safe Place profiles"); !result) return result;
    if (auto result = validate_record_vector(familiarity, "condition familiarity records"); !result) return result;
    if (auto result = validate_record_vector(view_conditions, "condition view conditions"); !result) return result;
    if (auto result = validate_record_vector(variants, "condition variants"); !result) return result;
    if (auto result = validate_record_vector(temporal_packages, "condition temporal packages"); !result) return result;
    if (auto result = validate_record_vector(surface_states, "condition surface states"); !result) return result;
    if (auto result = validate_record_vector(interaction_anchors, "condition interaction anchors"); !result) return result;
    if (auto result = validate_record_vector(collections, "condition collections"); !result) return result;
    if (auto result = validate_record_vector(prefabs, "condition prefabs"); !result) return result;
    if (auto result = validate_record_vector(region_kits, "condition region kits"); !result) return result;

    std::map<Id, const SourceEvidence*> source_by_id;
    std::map<Id, const SourceAsset*> asset_by_id;
    std::map<Id, const VariantCandidate*> variant_by_id;
    std::map<Id, const PrefabRecord*> prefab_by_id;
    std::map<Id, const CollectionRecord*> collection_by_id;
    for (const auto& source : sources) source_by_id.emplace(source.id, &source);
    for (const auto& asset : assets) asset_by_id.emplace(asset.id, &asset);
    for (const auto& variant : variants) variant_by_id.emplace(variant.id, &variant);
    for (const auto& prefab : prefabs) prefab_by_id.emplace(prefab.id, &prefab);
    for (const auto& collection : collections) collection_by_id.emplace(collection.id, &collection);

    for (const auto& asset : assets) {
        const auto source = source_by_id.find(asset.source_id);
        if (source == source_by_id.end()) {
            return core::Result<void>::failure(validation("condition asset references a missing source"));
        }
    }
    const auto require_asset = [&asset_by_id](Id asset_id, std::string_view label) -> core::Result<void> {
        if (!asset_by_id.contains(asset_id)) {
            return core::Result<void>::failure(validation(
                "condition " + std::string(label) + " references a missing asset"));
        }
        return core::Result<void>::success();
    };
    for (const auto& profile : safe_places) if (auto result = require_asset(profile.asset_id, "Safe Place"); !result) return result;
    for (const auto& record : familiarity) {
        if (auto result = require_asset(record.asset_id, "familiarity record"); !result) return result;
        for (const Id neighbor : record.expected_neighbors) if (auto result = require_asset(neighbor, "familiarity neighbor"); !result) return result;
    }
    for (const auto& condition : view_conditions) if (auto result = require_asset(condition.asset_id, "view condition"); !result) return result;
    for (const auto& variant : variants) if (auto result = require_asset(variant.asset_id, "variant"); !result) return result;
    for (const auto& package : temporal_packages) {
        if (auto result = require_asset(package.asset_id, "temporal package"); !result) return result;
        for (const Id variant_id : package.variant_ids) if (!variant_by_id.contains(variant_id)) return core::Result<void>::failure(validation("condition temporal package references a missing variant"));
    }
    for (const auto& state : surface_states) if (auto result = require_asset(state.asset_id, "surface state"); !result) return result;
    for (const auto& anchor : interaction_anchors) if (auto result = require_asset(anchor.asset_id, "interaction anchor"); !result) return result;
    for (const auto& collection : collections) {
        for (const Id asset_id : collection.asset_ids) if (auto result = require_asset(asset_id, "collection"); !result) return result;
        for (const Id child_id : collection.child_collection_ids) {
            if (!collection_by_id.contains(child_id) || child_id == collection.id) return core::Result<void>::failure(validation("condition collection hierarchy is invalid"));
        }
    }
    for (const auto& prefab : prefabs) {
        for (const Id asset_id : prefab.asset_ids) if (auto result = require_asset(asset_id, "prefab"); !result) return result;
        for (const Id variant_id : prefab.variant_ids) if (!variant_by_id.contains(variant_id)) return core::Result<void>::failure(validation("condition prefab references a missing variant"));
    }
    for (const auto& kit : region_kits) {
        for (const Id asset_id : kit.asset_ids) if (auto result = require_asset(asset_id, "region kit"); !result) return result;
        for (const Id prefab_id : kit.prefab_ids) if (!prefab_by_id.contains(prefab_id)) return core::Result<void>::failure(validation("condition region kit references a missing prefab"));
    }
    return core::Result<void>::success();
}

bool AuthoringDocument::has_stale_sources() const noexcept {
    std::map<Id, const SourceEvidence*> source_by_id;
    for (const auto& source : sources) source_by_id.emplace(source.id, &source);
    for (const auto& source : sources) if (!source.current) return true;
    for (const auto& asset : assets) {
        const auto found = source_by_id.find(asset.source_id);
        if (found == source_by_id.end() || !found->second->current ||
            found->second->revision != asset.source_revision ||
            found->second->digest != asset.source_digest) return true;
    }
    for (const auto& variant : variants) {
        if (variant.base_revision > project_revision) return true;
    }
    return false;
}

core::Result<void> AuthoringDocument::validate_for_publication() const {
    if (auto result = validate(); !result) return result;
    if (has_stale_sources()) {
        return core::Result<void>::failure(stale(
            "condition publication is blocked by stale source evidence or dependent assets"));
    }
    for (const auto& source : sources) {
        if (!source.reviewed) {
            return core::Result<void>::failure(validation(
                "condition publication requires every source evidence record to be reviewed"));
        }
    }
    return core::Result<void>::success();
}

std::string AuthoringDocument::serialize() const {
    std::ostringstream output;
    output << kAuthoringMagic << ' ' << kSchemaVersion << '\n';
    output << "PROJECT " << std::quoted(project_namespace) << ' ' << project_revision.value() << '\n';
    output << std::setprecision(17);
    output << "SOURCES " << sources.size() << '\n';
    for (const auto& source : sources) {
        output << "SOURCE " << source.id << ' ' << source_kind_name(source.kind) << ' '
               << std::quoted(source.label) << ' ' << std::quoted(source.logical_locator) << ' '
               << std::quoted(source.revision) << ' ' << source.digest.hex() << ' '
               << std::quoted(source.producer_identity) << ' ' << std::quoted(source.producer_version)
               << ' ' << source.import_settings_digest.hex() << ' ' << (source.reviewed ? 1U : 0U)
               << ' ' << (source.current ? 1U : 0U) << ' ';
        write_strings(output, source.diagnostics);
        output << '\n';
    }
    output << "ASSETS " << assets.size() << '\n';
    for (const auto& asset : assets) {
        output << "ASSET " << asset.id << ' ' << std::quoted(asset.stable_name) << ' '
               << asset_class_name(asset.asset_class) << ' ' << fidelity_class_name(asset.fidelity) << ' '
               << asset.source_id << ' ' << std::quoted(asset.source_revision) << ' '
               << asset.source_digest.hex() << ' ' << asset.geometry_digest.hex() << ' '
               << asset.material_digest.hex() << ' ' << asset.authored_object_id << ' ';
        write_strings(output, asset.tags);
        output << ' ';
        write_strings(output, asset.protected_features);
        output << '\n';
    }
    output << "SAFE_PLACES " << safe_places.size() << '\n';
    for (const auto& profile : safe_places) {
        output << "SAFE_PLACE " << profile.id << ' ' << profile.asset_id << ' '
               << profile.privacy << ' ' << profile.human_proximity << ' '
               << profile.controllable_entries << ' ' << profile.supply_access << ' '
               << profile.ambient_noise << ' ' << profile.visibility_outward << ' '
               << profile.visibility_inward << ' ' << profile.isolation << '\n';
    }
    output << "FAMILIARITY " << familiarity.size() << '\n';
    for (const auto& record : familiarity) {
        output << "FAMILIAR " << record.id << ' ' << record.asset_id << ' '
               << std::quoted(record.semantic_id) << ' ' << record.importance << ' ';
        write_transform(output, record.baseline_transform);
        output << ' ';
        write_ids(output, record.expected_neighbors);
        output << ' ' << std::quoted(record.expected_state) << ' '
               << (record.may_change_physically ? 1U : 0U) << '\n';
    }
    output << "VIEW_CONDITIONS " << view_conditions.size() << '\n';
    for (const auto& condition : view_conditions) {
        output << "VIEW " << condition.id << ' ' << condition.asset_id << ' '
               << condition.minimum_distance << ' ' << condition.maximum_distance << ' '
               << condition.minimum_elevation_degrees << ' ' << condition.maximum_elevation_degrees
               << ' ' << condition.cone_degrees << ' ' << condition.minimum_visibility_fraction
               << ' ' << (condition.requires_occlusion ? 1U : 0U) << ' '
               << std::quoted(condition.lighting_condition) << '\n';
    }
    output << "VARIANTS " << variants.size() << '\n';
    for (const auto& variant : variants) {
        output << "VARIANT " << variant.id << ' ' << variant.asset_id << ' '
               << std::quoted(variant.name) << ' ' << variant_kind_name(variant.kind) << ' '
               << variant.base_revision.value() << ' ' << variant.maximum_transform_delta << ' '
               << (variant.collision_impact_declared ? 1U : 0U) << ' '
               << (variant.navigation_impact_declared ? 1U : 0U) << ' '
               << (variant.interaction_impact_declared ? 1U : 0U) << ' '
               << (variant.reversible ? 1U : 0U) << ' ' << (variant.projection_only ? 1U : 0U)
               << ' ' << std::quoted(variant.semantic_id) << '\n';
    }
    output << "TEMPORAL " << temporal_packages.size() << '\n';
    for (const auto& package : temporal_packages) {
        output << "TEMPORAL_PACKAGE " << package.id << ' ' << package.asset_id << ' '
               << temporal_phase_name(package.phase) << ' ';
        write_ids(output, package.variant_ids);
        output << ' ' << std::quoted(package.material_variant) << ' '
               << std::quoted(package.source_alternate) << ' ' << (package.visible ? 1U : 0U) << '\n';
    }
    output << "SURFACE_STATES " << surface_states.size() << '\n';
    for (const auto& state : surface_states) {
        output << "SURFACE_STATE " << state.id << ' ' << state.asset_id << ' '
               << occupancy_state_name(state.occupancy) << ' ' << maintenance_state_name(state.maintenance)
               << ' ' << damage_state_name(state.damage) << ' ' << state.wetness << ' ';
        write_strings(output, state.wear_tags);
        output << ' ';
        write_strings(output, state.repair_tags);
        output << ' ' << std::quoted(state.provenance_note) << '\n';
    }
    output << "ANCHORS " << interaction_anchors.size() << '\n';
    for (const auto& anchor : interaction_anchors) {
        output << "ANCHOR " << anchor.id << ' ' << anchor.asset_id << ' '
               << anchor_kind_name(anchor.kind) << ' ' << std::quoted(anchor.interaction_class) << ' ';
        write_transform(output, anchor.transform);
        output << ' ' << anchor.approach_direction.x << ' ' << anchor.approach_direction.y << ' '
               << anchor.approach_direction.z << ' ' << anchor.clearance.x << ' '
               << anchor.clearance.y << ' ' << anchor.clearance.z << ' ' << (anchor.enabled ? 1U : 0U)
               << '\n';
    }
    output << "COLLECTIONS " << collections.size() << '\n';
    for (const auto& collection : collections) {
        output << "COLLECTION " << collection.id << ' ' << std::quoted(collection.name) << ' ';
        write_ids(output, collection.asset_ids);
        output << ' ';
        write_ids(output, collection.child_collection_ids);
        output << ' ' << std::quoted(collection.semantic_class) << '\n';
    }
    output << "PREFABS " << prefabs.size() << '\n';
    for (const auto& prefab : prefabs) {
        output << "PREFAB " << prefab.id << ' ' << std::quoted(prefab.name) << ' ';
        write_ids(output, prefab.asset_ids);
        output << ' ';
        write_strings(output, prefab.sockets);
        output << ' ';
        write_ids(output, prefab.variant_ids);
        output << ' ' << prefab.source_revision.value() << '\n';
    }
    output << "REGION_KITS " << region_kits.size() << '\n';
    for (const auto& kit : region_kits) {
        output << "REGION_KIT " << kit.id << ' ' << std::quoted(kit.name) << ' ';
        write_ids(output, kit.asset_ids);
        output << ' ';
        write_ids(output, kit.prefab_ids);
        output << ' ';
        write_enums(output, kit.required_classes, asset_class_name);
        output << ' ' << kit.interaction_coverage << ' ' << kit.familiarity_coverage << ' '
               << (kit.collision_validation_required ? 1U : 0U) << ' '
               << (kit.semantic_validation_required ? 1U : 0U) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<AuthoringDocument> AuthoringDocument::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<AuthoringDocument>::failure(parse_error(
            "condition authoring bytes are empty or exceed the serialized limit"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic >> version) || magic != kAuthoringMagic || version != kSchemaVersion ||
        !require_token(input, "PROJECT")) {
        return core::Result<AuthoringDocument>::failure(parse_error(
            "condition authoring header is invalid"));
    }
    AuthoringDocument result;
    if (!(input >> std::quoted(result.project_namespace)) || !read_unsigned(input, version)) {
        return core::Result<AuthoringDocument>::failure(parse_error(
            "condition authoring project record is invalid"));
    }
    result.project_revision = core::Revision(version);
    const auto read_header_count = [&input](std::string_view token, std::size_t& count) {
        return require_token(input, token) && read_count(input, AuthoringDocument::kMaxRecordsPerKind, count);
    };
    std::size_t count = 0U;
    if (!read_header_count("SOURCES", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition source count is invalid"));
    result.sources.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        SourceEvidence source;
        std::uint64_t reviewed = 0U;
        std::uint64_t current = 0U;
        if (!require_token(input, "SOURCE") || !read_unsigned(input, source.id) ||
            !read_named(input, {{SourceKind::authored_reference, "authored-reference"}, {SourceKind::imported_asset, "imported-asset"}, {SourceKind::procedural_recipe, "procedural-recipe"}, {SourceKind::material_reference, "material-reference"}, {SourceKind::authored_fixture, "authored-fixture"}}, source.kind) ||
            !(input >> std::quoted(source.label) >> std::quoted(source.logical_locator) >> std::quoted(source.revision)) ||
            !read_digest(input, source.digest) || !(input >> std::quoted(source.producer_identity) >> std::quoted(source.producer_version)) ||
            !read_digest(input, source.import_settings_digest) || !read_unsigned(input, reviewed) ||
            !read_unsigned(input, current) || reviewed > 1U || current > 1U || !read_strings(input, source.diagnostics)) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition source record is invalid"));
        }
        source.reviewed = reviewed != 0U;
        source.current = current != 0U;
        result.sources.push_back(std::move(source));
    }
    if (!read_header_count("ASSETS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition asset count is invalid"));
    result.assets.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        SourceAsset asset;
        if (!require_token(input, "ASSET") || !read_unsigned(input, asset.id) ||
            !(input >> std::quoted(asset.stable_name)) ||
            !read_named(input, {{AssetClass::architecture, "architecture"}, {AssetClass::prop, "prop"}, {AssetClass::vehicle, "vehicle"}, {AssetClass::vegetation, "vegetation"}, {AssetClass::terrain, "terrain"}, {AssetClass::water, "water"}, {AssetClass::fixture, "fixture"}, {AssetClass::character, "character"}, {AssetClass::wildlife, "wildlife"}, {AssetClass::material, "material"}}, asset.asset_class) ||
            !read_named(input, {{FidelityClass::hero, "hero"}, {FidelityClass::familiarity_critical, "familiarity-critical"}, {FidelityClass::standard, "standard"}, {FidelityClass::background, "background"}}, asset.fidelity) ||
            !read_unsigned(input, asset.source_id) || !(input >> std::quoted(asset.source_revision)) ||
            !read_digest(input, asset.source_digest) || !read_digest(input, asset.geometry_digest) ||
            !read_digest(input, asset.material_digest) || !read_unsigned(input, asset.authored_object_id) ||
            !read_strings(input, asset.tags) || !read_strings(input, asset.protected_features)) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition source asset record is invalid"));
        }
        result.assets.push_back(std::move(asset));
    }
    if (!read_header_count("SAFE_PLACES", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition Safe Place count is invalid"));
    result.safe_places.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        SafePlaceProfile profile;
        if (!require_token(input, "SAFE_PLACE") || !read_unsigned(input, profile.id) || !read_unsigned(input, profile.asset_id) ||
            !(input >> profile.privacy >> profile.human_proximity >> profile.controllable_entries >> profile.supply_access >>
              profile.ambient_noise >> profile.visibility_outward >> profile.visibility_inward >> profile.isolation)) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition Safe Place record is invalid"));
        }
        result.safe_places.push_back(std::move(profile));
    }
    if (!read_header_count("FAMILIARITY", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition familiarity count is invalid"));
    result.familiarity.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        FamiliarityRecord record;
        std::uint64_t mutable_value = 0U;
        if (!require_token(input, "FAMILIAR") || !read_unsigned(input, record.id) || !read_unsigned(input, record.asset_id) ||
            !(input >> std::quoted(record.semantic_id) >> record.importance) || !read_transform(input, record.baseline_transform) ||
            !read_ids(input, record.expected_neighbors) || !(input >> std::quoted(record.expected_state)) ||
            !read_unsigned(input, mutable_value) || mutable_value > 1U) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition familiarity record is invalid"));
        }
        record.may_change_physically = mutable_value != 0U;
        result.familiarity.push_back(std::move(record));
    }
    if (!read_header_count("VIEW_CONDITIONS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition view count is invalid"));
    result.view_conditions.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        ViewCondition view;
        std::uint64_t occlusion = 0U;
        if (!require_token(input, "VIEW") || !read_unsigned(input, view.id) || !read_unsigned(input, view.asset_id) ||
            !(input >> view.minimum_distance >> view.maximum_distance >> view.minimum_elevation_degrees >>
              view.maximum_elevation_degrees >> view.cone_degrees >> view.minimum_visibility_fraction) ||
            !read_unsigned(input, occlusion) || occlusion > 1U || !(input >> std::quoted(view.lighting_condition))) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition view record is invalid"));
        }
        view.requires_occlusion = occlusion != 0U;
        result.view_conditions.push_back(std::move(view));
    }
    if (!read_header_count("VARIANTS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition variant count is invalid"));
    result.variants.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        VariantCandidate variant;
        std::uint64_t collision = 0U;
        std::uint64_t navigation = 0U;
        std::uint64_t interaction = 0U;
        std::uint64_t reversible = 0U;
        std::uint64_t projection = 0U;
        std::uint64_t revision = 0U;
        if (!require_token(input, "VARIANT") || !read_unsigned(input, variant.id) || !read_unsigned(input, variant.asset_id) ||
            !(input >> std::quoted(variant.name)) ||
            !read_named(input, {{VariantKind::morph, "morph"}, {VariantKind::presentation, "presentation"}}, variant.kind) ||
            !read_unsigned(input, revision) || !(input >> variant.maximum_transform_delta) ||
            !read_unsigned(input, collision) || !read_unsigned(input, navigation) || !read_unsigned(input, interaction) ||
            !read_unsigned(input, reversible) || !read_unsigned(input, projection) ||
            collision > 1U || navigation > 1U || interaction > 1U || reversible > 1U || projection > 1U ||
            !(input >> std::quoted(variant.semantic_id))) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition variant record is invalid"));
        }
        variant.base_revision = core::Revision(revision);
        variant.collision_impact_declared = collision != 0U;
        variant.navigation_impact_declared = navigation != 0U;
        variant.interaction_impact_declared = interaction != 0U;
        variant.reversible = reversible != 0U;
        variant.projection_only = projection != 0U;
        result.variants.push_back(std::move(variant));
    }
    if (!read_header_count("TEMPORAL", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition temporal count is invalid"));
    result.temporal_packages.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        TemporalVisualPackage package;
        std::uint64_t visible = 0U;
        if (!require_token(input, "TEMPORAL_PACKAGE") || !read_unsigned(input, package.id) || !read_unsigned(input, package.asset_id) ||
            !read_named(input, {{TemporalPhase::day, "day"}, {TemporalPhase::late_day, "late-day"}, {TemporalPhase::dusk, "dusk"}, {TemporalPhase::blue_hour, "blue-hour"}, {TemporalPhase::night, "night"}, {TemporalPhase::pre_dawn, "pre-dawn"}, {TemporalPhase::dawn, "dawn"}}, package.phase) ||
            !read_ids(input, package.variant_ids) || !(input >> std::quoted(package.material_variant) >> std::quoted(package.source_alternate)) ||
            !read_unsigned(input, visible) || visible > 1U) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition temporal package is invalid"));
        }
        package.visible = visible != 0U;
        result.temporal_packages.push_back(std::move(package));
    }
    if (!read_header_count("SURFACE_STATES", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition surface state count is invalid"));
    result.surface_states.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        SurfaceStateRecord state;
        std::uint64_t wetness = 0U;
        if (!require_token(input, "SURFACE_STATE") || !read_unsigned(input, state.id) || !read_unsigned(input, state.asset_id) ||
            !read_named(input, {{OccupancyState::empty, "empty"}, {OccupancyState::recently_used, "recently-used"}, {OccupancyState::actively_occupied, "actively-occupied"}, {OccupancyState::long_term_occupied, "long-term-occupied"}, {OccupancyState::abandoned, "abandoned"}}, state.occupancy) ||
            !read_named(input, {{MaintenanceState::maintained, "maintained"}, {MaintenanceState::aging, "aging"}, {MaintenanceState::neglected, "neglected"}, {MaintenanceState::improvised_repair, "improvised-repair"}, {MaintenanceState::partially_renovated, "partially-renovated"}, {MaintenanceState::condemned, "condemned"}}, state.maintenance) ||
            !read_named(input, {{DamageState::intact, "intact"}, {DamageState::broken_window, "broken-window"}, {DamageState::damaged_door, "damaged-door"}, {DamageState::dented_sign, "dented-sign"}, {DamageState::collapsed_shelf, "collapsed-shelf"}, {DamageState::cracked_wall, "cracked-wall"}, {DamageState::damaged_railing, "damaged-railing"}, {DamageState::roof_damage, "roof-damage"}, {DamageState::collision_damage, "collision-damage"}, {DamageState::burned, "burned"}}, state.damage) ||
            !(input >> state.wetness) || !read_strings(input, state.wear_tags) || !read_strings(input, state.repair_tags) ||
            !(input >> std::quoted(state.provenance_note))) {
            static_cast<void>(wetness);
            return core::Result<AuthoringDocument>::failure(parse_error("condition surface state is invalid"));
        }
        result.surface_states.push_back(std::move(state));
    }
    if (!read_header_count("ANCHORS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition anchor count is invalid"));
    result.interaction_anchors.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        InteractionAnchor anchor;
        std::uint64_t enabled = 0U;
        if (!require_token(input, "ANCHOR") || !read_unsigned(input, anchor.id) || !read_unsigned(input, anchor.asset_id) ||
            !read_named(input, {{AnchorKind::interaction, "interaction"}, {AnchorKind::approach, "approach"}, {AnchorKind::seat, "seat"}, {AnchorKind::sleep, "sleep"}, {AnchorKind::traversal, "traversal"}, {AnchorKind::audio_source, "audio-source"}}, anchor.kind) ||
            !(input >> std::quoted(anchor.interaction_class)) || !read_transform(input, anchor.transform) ||
            !(input >> anchor.approach_direction.x >> anchor.approach_direction.y >> anchor.approach_direction.z >>
              anchor.clearance.x >> anchor.clearance.y >> anchor.clearance.z) || !read_unsigned(input, enabled) || enabled > 1U) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition anchor record is invalid"));
        }
        anchor.enabled = enabled != 0U;
        result.interaction_anchors.push_back(std::move(anchor));
    }
    if (!read_header_count("COLLECTIONS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition collection count is invalid"));
    result.collections.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        CollectionRecord collection;
        if (!require_token(input, "COLLECTION") || !read_unsigned(input, collection.id) || !(input >> std::quoted(collection.name)) ||
            !read_ids(input, collection.asset_ids) || !read_ids(input, collection.child_collection_ids) ||
            !(input >> std::quoted(collection.semantic_class))) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition collection record is invalid"));
        }
        result.collections.push_back(std::move(collection));
    }
    if (!read_header_count("PREFABS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition prefab count is invalid"));
    result.prefabs.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PrefabRecord prefab;
        std::uint64_t revision = 0U;
        if (!require_token(input, "PREFAB") || !read_unsigned(input, prefab.id) || !(input >> std::quoted(prefab.name)) ||
            !read_ids(input, prefab.asset_ids) || !read_strings(input, prefab.sockets) || !read_ids(input, prefab.variant_ids) ||
            !read_unsigned(input, revision)) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition prefab record is invalid"));
        }
        prefab.source_revision = core::Revision(revision);
        result.prefabs.push_back(std::move(prefab));
    }
    if (!read_header_count("REGION_KITS", count)) return core::Result<AuthoringDocument>::failure(parse_error("condition region kit count is invalid"));
    result.region_kits.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        RegionKitManifest kit;
        std::uint64_t collision = 0U;
        std::uint64_t semantic = 0U;
        if (!require_token(input, "REGION_KIT") || !read_unsigned(input, kit.id) || !(input >> std::quoted(kit.name)) ||
            !read_ids(input, kit.asset_ids) || !read_ids(input, kit.prefab_ids) ||
            !read_enums(input, kit.required_classes, {{AssetClass::architecture, "architecture"}, {AssetClass::prop, "prop"}, {AssetClass::vehicle, "vehicle"}, {AssetClass::vegetation, "vegetation"}, {AssetClass::terrain, "terrain"}, {AssetClass::water, "water"}, {AssetClass::fixture, "fixture"}, {AssetClass::character, "character"}, {AssetClass::wildlife, "wildlife"}, {AssetClass::material, "material"}}) ||
            !(input >> kit.interaction_coverage >> kit.familiarity_coverage) || !read_unsigned(input, collision) ||
            !read_unsigned(input, semantic) || collision > 1U || semantic > 1U) {
            return core::Result<AuthoringDocument>::failure(parse_error("condition region kit record is invalid"));
        }
        kit.collision_validation_required = collision != 0U;
        kit.semantic_validation_required = semantic != 0U;
        result.region_kits.push_back(std::move(kit));
    }
    if (!require_end(input)) return core::Result<AuthoringDocument>::failure(parse_error("condition authoring END record is invalid"));
    if (auto valid_result = result.validate(); !valid_result) return core::Result<AuthoringDocument>::failure(valid_result.error());
    return core::Result<AuthoringDocument>::success(std::move(result));
}

core::Result<assets::Sha256Digest> AuthoringDocument::canonical_digest() const {
    if (auto result = validate(); !result) return core::Result<assets::Sha256Digest>::failure(result.error());
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<void> AuthoringDocument::invalidate_source(
    Id source_id, std::string new_revision, assets::Sha256Digest new_digest) {
    if (source_id == 0U || !safe_text(new_revision, 128U) || new_digest.is_zero()) {
        return core::Result<void>::failure(invalid("condition source invalidation requires a source, revision, and digest"));
    }
    if (auto result = validate(); !result) return result;
    const auto found = std::find_if(sources.begin(), sources.end(), [source_id](const SourceEvidence& source) {
        return source.id == source_id;
    });
    if (found == sources.end()) return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::not_found, "condition source does not exist"));
    found->revision = std::move(new_revision);
    found->digest = new_digest;
    found->current = false;
    found->reviewed = false;
    const std::string message = "source invalidated; dependent records require revalidation";
    if (std::find(found->diagnostics.begin(), found->diagnostics.end(), message) == found->diagnostics.end()) {
        found->diagnostics.push_back(message);
        std::sort(found->diagnostics.begin(), found->diagnostics.end());
    }
    return core::Result<void>::success();
}

core::Result<void> Package::validate() const {
    if (!safe_namespace(project_namespace) || authoring_bytes.empty() ||
        authoring_bytes.size() > AuthoringDocument::kMaxSerializedBytes || authoring_digest.is_zero() ||
        !safe_text(publication_profile, 128U) || publication_digest.is_zero()) {
        return core::Result<void>::failure(validation("condition package identity or bytes are invalid"));
    }
    const auto decoded = AuthoringDocument::deserialize(authoring_bytes);
    if (!decoded) return core::Result<void>::failure(decoded.error().with_context("condition package authoring"));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(authoring_bytes.data());
    if (assets::sha256(std::span<const std::uint8_t>(bytes, authoring_bytes.size())) != authoring_digest) {
        return core::Result<void>::failure(validation("condition package authoring digest does not bind its bytes"));
    }
    if (canonical_without_digest().empty() || digest_text(canonical_without_digest()) != publication_digest) {
        return core::Result<void>::failure(validation("condition package publication digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string Package::canonical_without_digest() const {
    std::ostringstream output;
    output << kPackageMagic << ' ' << kSchemaVersion << '\n'
           << "PROJECT " << std::quoted(project_namespace) << ' ' << project_revision.value() << '\n'
           << "AUTHORING " << authoring_digest.hex() << ' ' << std::quoted(authoring_bytes) << '\n'
           << "PROFILE " << std::quoted(publication_profile) << '\n';
    return output.str();
}

core::Result<void> Package::refresh_digest() {
    const auto digest = digest_text(canonical_without_digest());
    if (digest.is_zero()) return core::Result<void>::failure(validation("condition package digest is zero"));
    publication_digest = digest;
    return core::Result<void>::success();
}

std::string Package::serialize() const {
    return canonical_without_digest() + "PUBLICATION " + publication_digest.hex() + "\nEND\n";
}

core::Result<Package> Package::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) return core::Result<Package>::failure(parse_error("condition package bytes are outside the supported limit"));
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    Package result;
    if (!(input >> magic >> version) || magic != kPackageMagic || version != kSchemaVersion ||
        !require_token(input, "PROJECT") || !(input >> std::quoted(result.project_namespace)) ||
        !read_unsigned(input, version) || !require_token(input, "AUTHORING") ||
        !read_digest(input, result.authoring_digest) || !(input >> std::quoted(result.authoring_bytes)) ||
        !require_token(input, "PROFILE") || !(input >> std::quoted(result.publication_profile)) ||
        !require_token(input, "PUBLICATION") || !read_digest(input, result.publication_digest) ||
        !require_end(input)) {
        return core::Result<Package>::failure(parse_error("condition package framing is invalid"));
    }
    result.project_revision = core::Revision(version);
    if (auto valid_result = result.validate(); !valid_result) return core::Result<Package>::failure(valid_result.error());
    return core::Result<Package>::success(std::move(result));
}

core::Result<Package> Package::publish(
    const AuthoringDocument& authoring, core::Revision project_revision, std::string project_namespace) {
    if (auto result = authoring.validate_for_publication(); !result) return core::Result<Package>::failure(result.error());
    Package result;
    result.project_namespace = std::move(project_namespace);
    result.project_revision = project_revision;
    result.authoring_bytes = authoring.serialize();
    const auto authoring_digest = authoring.canonical_digest();
    if (!authoring_digest) return core::Result<Package>::failure(authoring_digest.error());
    result.authoring_digest = authoring_digest.value();
    if (auto refresh = result.refresh_digest(); !refresh) return core::Result<Package>::failure(refresh.error());
    return core::Result<Package>::success(std::move(result));
}

core::Result<void> AdmissionResult::validate() const {
    if (status == PackageAdmissionStatus::admit &&
        (admitted_publication_digest.is_zero() || admitted_authoring_digest.is_zero() || !reasons.empty())) {
        return core::Result<void>::failure(validation("admitted condition package result is invalid"));
    }
    if (status == PackageAdmissionStatus::refuse && reasons.empty()) {
        return core::Result<void>::failure(validation("refused condition package result requires reasons"));
    }
    return core::Result<void>::success();
}

core::Result<AdmissionResult> admit_for_consumer(const Package& package) {
    if (auto result = package.validate(); !result) return core::Result<AdmissionResult>::failure(result.error());
    const auto authoring = AuthoringDocument::deserialize(package.authoring_bytes);
    if (!authoring) return core::Result<AdmissionResult>::failure(authoring.error());
    if (auto publication = authoring.value().validate_for_publication(); !publication) {
        AdmissionResult refused;
        refused.status = PackageAdmissionStatus::refuse;
        refused.reasons.push_back(publication.error().message);
        return core::Result<AdmissionResult>::success(std::move(refused));
    }
    AdmissionResult admitted;
    admitted.status = PackageAdmissionStatus::admit;
    admitted.admitted_publication_digest = package.publication_digest;
    admitted.admitted_authoring_digest = package.authoring_digest;
    if (auto result = admitted.validate(); !result) return core::Result<AdmissionResult>::failure(result.error());
    return core::Result<AdmissionResult>::success(std::move(admitted));
}

core::Result<AuthoringDocument> make_condition_fixture() {
    const auto digest = [](std::string_view value) { return digest_text(value); };
    AuthoringDocument result;
    result.project_namespace = "condition.fixture.motel8";
    result.project_revision = core::Revision(0U);
    result.sources.push_back(SourceEvidence{
        1U, SourceKind::authored_fixture, "Motel 8 source-authoring fixture",
        "fixture://condition/motel-8", "fixture-1", digest("condition-motel8-source-v1"),
        "Cartographer", "condition-fixture-v1", digest("condition-motel8-import-v1"), true, true, {}});
    const auto source_digest = result.sources.front().digest;
    result.assets.push_back(SourceAsset{
        1U, "motel_8_exterior", AssetClass::architecture, FidelityClass::familiarity_critical,
        1U, "fixture-1", source_digest, digest("motel8-exterior-geometry"),
        digest("motel8-exterior-material"), 1U, {"building", "safe-place"}, {"entry-door", "silhouette"}});
    result.assets.push_back(SourceAsset{
        2U, "motel_8_sign", AssetClass::prop, FidelityClass::familiarity_critical,
        1U, "fixture-1", source_digest, digest("motel8-sign-geometry"),
        digest("motel8-sign-material"), 2U, {"familiarity", "sign"}, {"missing-letter", "silhouette"}});
    result.assets.push_back(SourceAsset{
        3U, "motel_8_room", AssetClass::architecture, FidelityClass::hero,
        1U, "fixture-1", source_digest, digest("motel8-room-geometry"),
        digest("motel8-room-material"), 3U, {"interior", "room"}, {"bed", "entry-door"}});
    result.safe_places.push_back(SafePlaceProfile{1U, 1U, 0.8, 0.25, 0.75, 0.6, 0.35, 0.8, 0.25, 0.7});
    result.familiarity.push_back(FamiliarityRecord{
        1U, 2U, "SAFEPLACE.MOTEL8.SIGN.MISSING_LETTER_R", 0.95,
        core::Transform::identity(), {1U}, "day-baseline", false});
    result.view_conditions.push_back(ViewCondition{1U, 2U, 4.0, 80.0, -20.0, 35.0, 55.0, 0.65, false, "day-or-dusk"});
    result.variants.push_back(VariantCandidate{
        1U, 2U, "missing-letter", VariantKind::morph, core::Revision(0U), 0.02,
        true, false, false, true, false, "SAFEPLACE.MOTEL8.SIGN.MISSING_LETTER_R"});
    result.variants.push_back(VariantCandidate{
        2U, 1U, "dusk-presentation", VariantKind::presentation, core::Revision(0U), 0.01,
        false, false, false, true, true, "SAFEPLACE.MOTEL8.PRESENTATION.DUSK"});
    result.temporal_packages.push_back(TemporalVisualPackage{1U, 1U, TemporalPhase::day, {}, "maintained-day", "", true});
    result.temporal_packages.push_back(TemporalVisualPackage{2U, 1U, TemporalPhase::dusk, {2U}, "dusk-wet", "", true});
    result.surface_states.push_back(SurfaceStateRecord{
        1U, 1U, OccupancyState::recently_used, MaintenanceState::aging, DamageState::intact, 0.35,
        {"edge-fade", "rain-streak"}, {"patched-siding"}, "fixture-authored surface history"});
    result.interaction_anchors.push_back(InteractionAnchor{
        1U, 1U, AnchorKind::interaction, "entry", core::Transform::identity(),
        {0.0, 0.0, 1.0}, {1.2, 2.0, 1.2}, true});
    result.interaction_anchors.push_back(InteractionAnchor{
        2U, 3U, AnchorKind::sleep, "bed", core::Transform::identity(),
        {0.0, 0.0, 1.0}, {1.0, 1.0, 2.0}, true});
    result.collections.push_back(CollectionRecord{1U, "Motel 8", {1U, 2U, 3U}, {}, "safe-place"});
    result.prefabs.push_back(PrefabRecord{1U, "Motel 8 Room", {3U}, {"room.bed", "room.door.entry", "room.wall.north"}, {}, core::Revision(0U)});
    result.region_kits.push_back(RegionKitManifest{
        1U, "Motel Kit", {1U, 2U, 3U}, {1U}, {AssetClass::architecture, AssetClass::prop},
        0.75, 0.75, true, true});
    if (auto valid_result = result.validate(); !valid_result) return core::Result<AuthoringDocument>::failure(valid_result.error());
    return core::Result<AuthoringDocument>::success(std::move(result));
}

} // namespace carto::condition
