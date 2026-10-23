#include <carto/engineering/authoring.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::engineering {
namespace {

constexpr std::string_view kAuthoringMagic = "CARTOGRAPHER_ENGINEERING_AUTHORING";
constexpr std::string_view kPackageMagic = "CARTOGRAPHER_DV012_INSPECTION_PACKAGE";
constexpr std::size_t kMaxText = 4096U;
constexpr std::size_t kMaxDiagnostics = 256U;

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

bool safe_text(std::string_view value, bool allow_empty = false, std::size_t maximum = kMaxText) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\r' && byte != '\n';
    });
}

bool contains_local_path_reference(std::string_view value) {
    if (value.size() >= 5U && value[4U] == ':' &&
        std::tolower(static_cast<unsigned char>(value[0U])) == 'f' &&
        std::tolower(static_cast<unsigned char>(value[1U])) == 'i' &&
        std::tolower(static_cast<unsigned char>(value[2U])) == 'l' &&
        std::tolower(static_cast<unsigned char>(value[3U])) == 'e') return true;
    const auto scheme_end = value.find("://");
    if (scheme_end != std::string_view::npos) return false;
    if (value.find_first_of("/\\") != std::string_view::npos) return true;
    return value.size() >= 2U &&
        std::isalpha(static_cast<unsigned char>(value[0U])) != 0 && value[1U] == ':';
}

bool contains_local_path_reference(const AuthoringDocument& authoring) {
    for (const auto& source : authoring.sources) {
        if (contains_local_path_reference(source.locator)) return true;
    }
    if (!authoring.workcell.has_value()) return false;
    const auto& workcell = *authoring.workcell;
    if (contains_local_path_reference(workcell.robot_source_identity) ||
        contains_local_path_reference(workcell.robot_model_identity)) return true;
    for (const auto& entity : workcell.entities) {
        if (contains_local_path_reference(entity.source)) return true;
    }
    for (const auto& keep_out : workcell.keep_outs) {
        if (contains_local_path_reference(keep_out.source)) return true;
    }
    for (const auto& frame : workcell.frames) {
        if (contains_local_path_reference(frame.source)) return true;
    }
    return false;
}

bool valid_utf8(std::string_view value) {
    const auto byte_at = [&value](std::size_t index) {
        return static_cast<unsigned char>(value[index]);
    };
    const auto continuation = [&byte_at, &value](std::size_t index) {
        return index < value.size() && byte_at(index) >= 0x80U && byte_at(index) <= 0xbfU;
    };
    for (std::size_t index = 0U; index < value.size();) {
        const unsigned char first = byte_at(index);
        if (first <= 0x7fU) {
            ++index;
        } else if (first >= 0xc2U && first <= 0xdfU && continuation(index + 1U)) {
            index += 2U;
        } else if (first == 0xe0U && index + 2U < value.size() &&
                   byte_at(index + 1U) >= 0xa0U && byte_at(index + 1U) <= 0xbfU &&
                   continuation(index + 2U)) {
            index += 3U;
        } else if (((first >= 0xe1U && first <= 0xecU) ||
                    (first >= 0xeeU && first <= 0xefU)) &&
                   continuation(index + 1U) && continuation(index + 2U)) {
            index += 3U;
        } else if (first == 0xedU && index + 2U < value.size() &&
                   byte_at(index + 1U) >= 0x80U && byte_at(index + 1U) <= 0x9fU &&
                   continuation(index + 2U)) {
            index += 3U;
        } else if (first == 0xf0U && index + 3U < value.size() &&
                   byte_at(index + 1U) >= 0x90U && byte_at(index + 1U) <= 0xbfU &&
                   continuation(index + 2U) && continuation(index + 3U)) {
            index += 4U;
        } else if (first >= 0xf1U && first <= 0xf3U &&
                   continuation(index + 1U) && continuation(index + 2U) &&
                   continuation(index + 3U)) {
            index += 4U;
        } else if (first == 0xf4U && index + 3U < value.size() &&
                   byte_at(index + 1U) >= 0x80U && byte_at(index + 1U) <= 0x8fU &&
                   continuation(index + 2U) && continuation(index + 3U)) {
            index += 4U;
        } else {
            return false;
        }
    }
    return true;
}

core::Result<std::vector<std::vector<std::string>>> parse_csv_rows(
    std::string_view bytes, std::size_t max_bytes, std::string_view label) {
    if (bytes.empty() || bytes.size() > max_bytes || !valid_utf8(bytes)) {
        return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
            std::string(label) + " must be non-empty, bounded UTF-8 text"));
    }
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3U);

    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    bool quote_closed = false;
    bool row_has_content = false;
    const auto finish_field = [&]() {
        fields.push_back(std::move(field));
        field.clear();
        quote_closed = false;
    };
    const auto finish_row = [&]() {
        finish_field();
        if (fields.size() == 1U && fields.front().empty()) return false;
        rows.push_back(std::move(fields));
        fields.clear();
        row_has_content = false;
        return rows.size() <= AuthoringDocument::kMaxCollectionEntries + 1U;
    };

    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const char byte = bytes[index];
        if (quoted) {
            if (byte == '"') {
                if (index + 1U < bytes.size() && bytes[index + 1U] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                    quote_closed = true;
                }
            } else if (byte == '\r' || byte == '\n') {
                return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
                    std::string(label) + " fields cannot contain line breaks"));
            } else {
                field.push_back(byte);
            }
            row_has_content = true;
            continue;
        }
        if (quote_closed) {
            if (byte == ',') {
                finish_field();
                row_has_content = true;
                continue;
            }
            if (byte == '\r' || byte == '\n') {
                if (!finish_row()) {
                    return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
                        std::string(label) + " contains a blank or oversized row"));
                }
                if (byte == '\r' && index + 1U < bytes.size() && bytes[index + 1U] == '\n') {
                    ++index;
                }
                continue;
            }
            return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
                std::string(label) + " contains data after a closing quote"));
        }
        if (byte == '"') {
            if (!field.empty()) {
                return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
                    std::string(label) + " quote must begin a field"));
            }
            quoted = true;
            row_has_content = true;
        } else if (byte == ',') {
            finish_field();
            row_has_content = true;
        } else if (byte == '\r' || byte == '\n') {
            if (!finish_row()) {
                return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
                    std::string(label) + " contains a blank or oversized row"));
            }
            if (byte == '\r' && index + 1U < bytes.size() && bytes[index + 1U] == '\n') {
                ++index;
            }
        } else {
            field.push_back(byte);
            row_has_content = true;
        }
    }
    if (quoted) {
        return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
            std::string(label) + " ends inside a quoted field"));
    }
    if (row_has_content && !finish_row()) {
        return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
            std::string(label) + " contains a blank or oversized row"));
    }
    if (rows.size() < 2U || rows.size() - 1U > AuthoringDocument::kMaxCollectionEntries) {
        return core::Result<std::vector<std::vector<std::string>>>::failure(parse_error(
            std::string(label) + " requires a header and bounded data rows"));
    }
    return core::Result<std::vector<std::vector<std::string>>>::success(std::move(rows));
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

template <typename Enum>
const char* named(Enum value, std::initializer_list<std::pair<Enum, const char*>> values) {
    for (const auto& [candidate, name] : values) {
        if (candidate == value) return name;
    }
    return "unknown";
}

template <typename Enum>
core::Result<Enum> parse_named(
    std::string_view value,
    std::initializer_list<std::pair<Enum, const char*>> values,
    std::string label) {
    for (const auto& [candidate, name] : values) {
        if (value == name) return core::Result<Enum>::success(candidate);
    }
    return core::Result<Enum>::failure(invalid(std::move(label)));
}

bool read_uint(std::istream& input, std::uint64_t& value) {
    input >> value;
    return static_cast<bool>(input);
}

bool read_double(std::istream& input, double& value) {
    input >> value;
    return static_cast<bool>(input) && std::isfinite(value);
}

bool read_bool(std::istream& input, bool& value) {
    std::uint64_t encoded = 0U;
    if (!read_uint(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

bool read_text(std::istream& input, std::string& value, bool allow_empty = true) {
    return static_cast<bool>(input >> std::quoted(value)) && safe_text(value, allow_empty);
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string encoded;
    if (!(input >> encoded)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(encoded);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

bool read_id(std::istream& input, Id& id) {
    std::uint64_t value = 0U;
    if (!read_uint(input, value)) return false;
    id = value;
    return true;
}

bool read_optional_id(std::istream& input, std::optional<Id>& id) {
    std::uint64_t value = 0U;
    if (!read_uint(input, value)) return false;
    if (value == 0U) id.reset();
    else id = value;
    return true;
}

bool read_count(std::istream& input, std::size_t& count, std::size_t maximum) {
    std::uint64_t encoded = 0U;
    if (!read_uint(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) return false;
    count = static_cast<std::size_t>(encoded);
    return true;
}

bool require_record(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool require_end(std::istream& input) {
    std::string token;
    std::string trailing;
    return static_cast<bool>(input >> token) && token == "END" && !(input >> trailing);
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
    return read_double(input, transform.translation.x) &&
        read_double(input, transform.translation.y) &&
        read_double(input, transform.translation.z) &&
        read_double(input, transform.rotation.x) &&
        read_double(input, transform.rotation.y) &&
        read_double(input, transform.rotation.z) &&
        read_double(input, transform.rotation.w) &&
        read_double(input, transform.scale.x) &&
        read_double(input, transform.scale.y) &&
        read_double(input, transform.scale.z);
}

void write_strings(std::ostringstream& output, const std::vector<std::string>& values) {
    output << values.size();
    for (const auto& value : values) output << ' ' << std::quoted(value);
}

bool read_strings(std::istream& input, std::vector<std::string>& values) {
    std::size_t count = 0U;
    if (!read_count(input, count, kMaxDiagnostics)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::string value;
        if (!read_text(input, value)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

void write_ids(std::ostringstream& output, const std::vector<Id>& values) {
    output << values.size();
    for (const Id value : values) output << ' ' << value;
}

bool read_ids(std::istream& input, std::vector<Id>& values) {
    std::size_t count = 0U;
    if (!read_count(input, count, AuthoringDocument::kMaxCollectionEntries)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        Id value = 0U;
        if (!read_id(input, value)) return false;
        values.push_back(value);
    }
    return true;
}

template <typename T>
bool unique_ids(const std::vector<T>& values, Id T::*member) {
    std::set<Id> ids;
    for (const auto& value : values) {
        const Id id = value.*member;
        if (id == 0U || !ids.insert(id).second) return false;
    }
    return true;
}

bool valid_ids(const std::vector<Id>& values) {
    return std::all_of(values.begin(), values.end(), [](Id value) { return value != 0U; });
}

template <typename T>
bool contains_id(const std::vector<T>& values, Id T::*member, Id id) {
    return std::any_of(values.begin(), values.end(), [id, member](const T& value) {
        return value.*member == id;
    });
}

template <typename T>
const T* find_id(const std::vector<T>& values, Id T::*member, Id id) {
    const auto found = std::find_if(values.begin(), values.end(), [id, member](const T& value) {
        return value.*member == id;
    });
    return found == values.end() ? nullptr : &*found;
}

template <typename T>
T* find_id(std::vector<T>& values, Id T::*member, Id id) {
    const auto found = std::find_if(values.begin(), values.end(), [id, member](const T& value) {
        return value.*member == id;
    });
    return found == values.end() ? nullptr : &*found;
}

template <typename E>
bool enum_known(E value, std::initializer_list<E> values) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

template <typename T>
core::Result<void> validate_collection_size(const std::vector<T>& values, std::string_view label) {
    if (values.size() > AuthoringDocument::kMaxCollectionEntries) {
        return core::Result<void>::failure(validation(std::string(label) + " exceeds its bound"));
    }
    return core::Result<void>::success();
}

} // namespace

const char* source_kind_name(SourceKind value) noexcept {
    return named(value, {{SourceKind::step_ap242, "step-ap242"}, {SourceKind::bom, "bom"},
        {SourceKind::drawing, "drawing"}, {SourceKind::specification, "specification"}});
}
const char* topology_kind_name(TopologyKind value) noexcept {
    return named(value, {{TopologyKind::assembly, "assembly"}, {TopologyKind::part, "part"},
        {TopologyKind::occurrence, "occurrence"}, {TopologyKind::body, "body"},
        {TopologyKind::face, "face"}, {TopologyKind::edge, "edge"},
        {TopologyKind::vertex, "vertex"}, {TopologyKind::feature, "feature"},
        {TopologyKind::datum, "datum"}, {TopologyKind::topology_set, "topology-set"}});
}
const char* requirement_kind_name(RequirementKind value) noexcept {
    return named(value, {{RequirementKind::dimensional_distance, "dimensional-distance"},
        {RequirementKind::diameter, "diameter"}, {RequirementKind::radius, "radius"},
        {RequirementKind::angle, "angle"}, {RequirementKind::thickness, "thickness"},
        {RequirementKind::clearance, "clearance"}, {RequirementKind::position, "position"},
        {RequirementKind::flatness, "flatness"}, {RequirementKind::parallelism, "parallelism"},
        {RequirementKind::perpendicularity, "perpendicularity"},
        {RequirementKind::concentricity, "concentricity"},
        {RequirementKind::surface_condition, "surface-condition"},
        {RequirementKind::presence_absence, "presence-absence"},
        {RequirementKind::visual_surface, "visual-surface"},
        {RequirementKind::assembly_relationship, "assembly-relationship"},
        {RequirementKind::material_verification, "material-verification"}});
}
const char* extraction_state_name(ExtractionState value) noexcept {
    return named(value, {{ExtractionState::manual, "manual"},
        {ExtractionState::imported_structured, "imported-structured"},
        {ExtractionState::machine_extracted_proposed, "machine-extracted-proposed"},
        {ExtractionState::human_verified, "human-verified"},
        {ExtractionState::human_corrected, "human-corrected"},
        {ExtractionState::rejected, "rejected"}, {ExtractionState::stale, "stale"}});
}
const char* reconciliation_status_name(ReconciliationStatus value) noexcept {
    return named(value, {{ReconciliationStatus::exact, "exact"},
        {ReconciliationStatus::matched_by_external_id, "matched-by-external-id"},
        {ReconciliationStatus::matched_by_part_number, "matched-by-part-number"},
        {ReconciliationStatus::quantity_mismatch, "quantity-mismatch"},
        {ReconciliationStatus::revision_mismatch, "revision-mismatch"},
        {ReconciliationStatus::material_mismatch, "material-mismatch"},
        {ReconciliationStatus::source_only, "source-only"},
        {ReconciliationStatus::assembly_only, "assembly-only"},
        {ReconciliationStatus::ambiguous, "ambiguous"},
        {ReconciliationStatus::unresolved, "unresolved"}});
}
const char* inspection_kind_name(InspectionKind value) noexcept {
    return named(value, {{InspectionKind::point_measurement, "point-measurement"},
        {InspectionKind::distance, "distance"}, {InspectionKind::angle, "angle"},
        {InspectionKind::diameter, "diameter"}, {InspectionKind::thickness, "thickness"},
        {InspectionKind::clearance, "clearance"}, {InspectionKind::surface_scan, "surface-scan"},
        {InspectionKind::visual_surface_check, "visual-surface-check"},
        {InspectionKind::feature_presence, "feature-presence"},
        {InspectionKind::datum_capture, "datum-capture"},
        {InspectionKind::dimensional_feature_check, "dimensional-feature-check"}});
}
bool inspection_kind_compatible(
    RequirementKind requirement, InspectionKind characteristic) noexcept {
    using R = RequirementKind;
    using I = InspectionKind;
    switch (requirement) {
    case R::dimensional_distance:
        return characteristic == I::distance || characteristic == I::point_measurement ||
            characteristic == I::dimensional_feature_check;
    case R::diameter:
        return characteristic == I::diameter || characteristic == I::dimensional_feature_check;
    case R::radius:
        return characteristic == I::dimensional_feature_check;
    case R::angle:
        return characteristic == I::angle || characteristic == I::dimensional_feature_check;
    case R::thickness:
        return characteristic == I::thickness || characteristic == I::dimensional_feature_check;
    case R::clearance:
        return characteristic == I::clearance || characteristic == I::distance;
    case R::position:
        return characteristic == I::point_measurement ||
            characteristic == I::dimensional_feature_check;
    case R::flatness:
    case R::parallelism:
    case R::perpendicularity:
    case R::concentricity:
        return characteristic == I::dimensional_feature_check;
    case R::surface_condition:
    case R::visual_surface:
        return characteristic == I::surface_scan || characteristic == I::visual_surface_check;
    case R::presence_absence:
    case R::assembly_relationship:
        return characteristic == I::feature_presence;
    case R::material_verification:
        return false;
    }
    return false;
}
const char* inspection_status_name(InspectionStatus value) noexcept {
    return named(value, {{InspectionStatus::authored, "authored"},
        {InspectionStatus::disabled, "disabled"}, {InspectionStatus::stale, "stale"}});
}
const char* target_status_name(TargetStatus value) noexcept {
    return named(value, {{TargetStatus::proposed, "proposed"}, {TargetStatus::accepted, "accepted"},
        {TargetStatus::rejected, "rejected"}, {TargetStatus::stale, "stale"},
        {TargetStatus::unreachable, "unreachable"}, {TargetStatus::collision, "collision"},
        {TargetStatus::occluded, "occluded"}, {TargetStatus::runtime_verified, "runtime-verified"},
        {TargetStatus::runtime_failed, "runtime-failed"}});
}
const char* workcell_entity_kind_name(WorkcellEntityKind value) noexcept {
    return named(value, {{WorkcellEntityKind::inspection_assembly, "inspection-assembly"},
        {WorkcellEntityKind::fixture, "fixture"}, {WorkcellEntityKind::robot_base, "robot-base"},
        {WorkcellEntityKind::sensor_tool, "sensor-tool"},
        {WorkcellEntityKind::reference_frame, "reference-frame"},
        {WorkcellEntityKind::operator_exclusion, "operator-exclusion"}});
}
const char* keep_out_kind_name(KeepOutKind value) noexcept {
    return named(value, {{KeepOutKind::box, "box"}, {KeepOutKind::sphere, "sphere"},
        {KeepOutKind::cylinder, "cylinder"}, {KeepOutKind::mesh, "mesh"}});
}
const char* derived_product_status_name(DerivedProductStatus value) noexcept {
    return named(value, {{DerivedProductStatus::current, "current"},
        {DerivedProductStatus::stale, "stale"}, {DerivedProductStatus::rejected, "rejected"},
        {DerivedProductStatus::unverified, "unverified"}});
}
const char* path_sample_status_name(PathSampleStatus value) noexcept {
    return named(value, {{PathSampleStatus::valid, "valid"}, {PathSampleStatus::rejected, "rejected"},
        {PathSampleStatus::stale, "stale"}, {PathSampleStatus::unreachable, "unreachable"},
        {PathSampleStatus::collision, "collision"},
        {PathSampleStatus::clearance_warning, "clearance-warning"},
        {PathSampleStatus::unverified, "unverified"}});
}

core::Result<void> EngineeringSource::validate() const {
    if (id == 0U || !enum_known(kind, {SourceKind::step_ap242, SourceKind::bom,
        SourceKind::drawing, SourceKind::specification}) ||
        !safe_text(locator) || !safe_text(revision) || digest.is_zero() ||
        !safe_text(provider_identity) || !safe_text(provider_version) ||
        import_settings_digest.is_zero() || !safe_text(import_run_id) || !safe_text(units) ||
        diagnostics.size() > kMaxDiagnostics) {
        return core::Result<void>::failure(validation("engineering source is malformed"));
    }
    for (const auto& diagnostic : diagnostics) {
        if (!safe_text(diagnostic, true)) {
            return core::Result<void>::failure(validation("engineering source diagnostic is invalid"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> EngineeringPart::validate() const {
    if (id == 0U || !safe_text(external_id) || !safe_text(name) || !safe_text(revision) ||
        !safe_text(material, true) || source_id == 0U ||
        (nominal_mass.has_value() && (!std::isfinite(*nominal_mass) || *nominal_mass < 0.0))) {
        return core::Result<void>::failure(validation("engineering part is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> EngineeringOccurrence::validate() const {
    if (id == 0U || !safe_text(external_id) || part_id == 0U ||
        (parent_id.has_value() && (*parent_id == 0U || *parent_id == id)) ||
        !transform.finite() || quantity == 0U || source_id == 0U) {
        return core::Result<void>::failure(validation("engineering occurrence is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> TopologyBinding::validate() const {
    if (id == 0U || source_id == 0U || part_id == 0U ||
        (occurrence_id.has_value() && *occurrence_id == 0U) ||
        !enum_known(kind, {TopologyKind::assembly, TopologyKind::part, TopologyKind::occurrence,
            TopologyKind::body, TopologyKind::face, TopologyKind::edge, TopologyKind::vertex,
            TopologyKind::feature, TopologyKind::datum, TopologyKind::topology_set}) ||
        !safe_text(external_id) || !safe_text(source_revision) || display_mesh_asset == 0U) {
        return core::Result<void>::failure(validation("topology binding is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> EngineeringBomLine::validate() const {
    if (id == 0U || !safe_text(part_number) || !safe_text(revision) ||
        !safe_text(description, true) || !safe_text(material, true) ||
        !std::isfinite(quantity) || quantity <= 0.0 || !safe_text(unit) || source_id == 0U ||
        (mapped_part_id.has_value() && *mapped_part_id == 0U) ||
        !enum_known(reconciliation, {ReconciliationStatus::exact,
            ReconciliationStatus::matched_by_external_id, ReconciliationStatus::matched_by_part_number,
            ReconciliationStatus::quantity_mismatch, ReconciliationStatus::revision_mismatch,
            ReconciliationStatus::material_mismatch, ReconciliationStatus::source_only,
            ReconciliationStatus::assembly_only, ReconciliationStatus::ambiguous,
            ReconciliationStatus::unresolved})) {
        return core::Result<void>::failure(validation("engineering BOM line is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> RequirementProvenance::validate() const {
    if (source_id == 0U || !safe_text(section, true) || !safe_text(callout, true) ||
        !safe_text(drawing_zone, true) || !safe_text(extractor_identity, true) ||
        !std::isfinite(extraction_confidence) || extraction_confidence < 0.0 ||
        extraction_confidence > 1.0 || !safe_text(original_text, true)) {
        return core::Result<void>::failure(validation("requirement provenance is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> EngineeringRequirement::validate() const {
    if (id == 0U || !enum_known(kind, {RequirementKind::dimensional_distance,
        RequirementKind::diameter, RequirementKind::radius, RequirementKind::angle,
        RequirementKind::thickness, RequirementKind::clearance, RequirementKind::position,
        RequirementKind::flatness, RequirementKind::parallelism, RequirementKind::perpendicularity,
        RequirementKind::concentricity, RequirementKind::surface_condition,
        RequirementKind::presence_absence, RequirementKind::visual_surface,
        RequirementKind::assembly_relationship, RequirementKind::material_verification}) ||
        !enum_known(extraction, {ExtractionState::manual, ExtractionState::imported_structured,
            ExtractionState::machine_extracted_proposed, ExtractionState::human_verified,
            ExtractionState::human_corrected, ExtractionState::rejected, ExtractionState::stale}) ||
        !valid_ids(topology_ids) || topology_ids.empty() || !std::isfinite(nominal) ||
        !std::isfinite(plus) || !std::isfinite(minus) || plus < 0.0 || minus < 0.0 ||
        !safe_text(unit) || !safe_text(acceptance_rule, true) ||
        !safe_text(unsupported_content, true, 4096U) ||
        !safe_text(human_review_justification, true) ||
        !valid_ids(std::vector<Id>{provenance.source_id})) {
        return core::Result<void>::failure(validation("engineering requirement is malformed"));
    }
    if (!unsupported_content.empty() && extraction != ExtractionState::machine_extracted_proposed &&
        extraction != ExtractionState::stale && extraction != ExtractionState::rejected) {
        return core::Result<void>::failure(validation(
            "engineering requirement with unsupported source content cannot be active"));
    }
    if (auto result = provenance.validate(); !result) return result;
    for (const auto& datum : datum_refs) if (!safe_text(datum)) {
        return core::Result<void>::failure(validation("requirement datum reference is invalid"));
    }
    for (const auto& modifier : modifier_refs) if (!safe_text(modifier)) {
        return core::Result<void>::failure(validation("requirement modifier reference is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SensorConstraint::validate() const {
    if (!safe_text(sensor_type) || !safe_text(tool_center_frame) ||
        !std::isfinite(min_standoff) || !std::isfinite(max_standoff) ||
        !std::isfinite(preferred_standoff) || !std::isfinite(orientation_cone_degrees) ||
        !std::isfinite(field_of_view_degrees) || !std::isfinite(min_clearance) ||
        !std::isfinite(measurement_duration_seconds) || min_standoff < 0.0 ||
        max_standoff < min_standoff || preferred_standoff < min_standoff ||
        preferred_standoff > max_standoff || orientation_cone_degrees <= 0.0 ||
        orientation_cone_degrees > 180.0 || field_of_view_degrees < 0.0 ||
        field_of_view_degrees > 360.0 || min_clearance < 0.0 ||
        measurement_duration_seconds < 0.0) {
        return core::Result<void>::failure(validation("sensor constraint is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> CoverageConstraint::validate() const {
    if (!safe_text(kind) || !std::isfinite(percentage) || percentage < 0.0 || percentage > 100.0) {
        return core::Result<void>::failure(validation("coverage constraint is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> AccessConstraint::validate() const {
    if (!std::isfinite(minimum_approach_clearance) || minimum_approach_clearance < 0.0) {
        return core::Result<void>::failure(validation("access constraint is malformed"));
    }
    for (const auto& value : allowed_approach_directions) if (!safe_text(value)) {
        return core::Result<void>::failure(validation("allowed approach direction is invalid"));
    }
    for (const auto& value : forbidden_approach_directions) if (!safe_text(value)) {
        return core::Result<void>::failure(validation("forbidden approach direction is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> InspectionCharacteristic::validate() const {
    if (id == 0U || requirement_id == 0U || !valid_ids(topology_ids) || topology_ids.empty() ||
        !enum_known(kind, {InspectionKind::point_measurement, InspectionKind::distance,
            InspectionKind::angle, InspectionKind::diameter, InspectionKind::thickness,
            InspectionKind::clearance, InspectionKind::surface_scan,
            InspectionKind::visual_surface_check, InspectionKind::feature_presence,
            InspectionKind::datum_capture, InspectionKind::dimensional_feature_check}) ||
        !enum_known(status, {InspectionStatus::authored, InspectionStatus::disabled,
            InspectionStatus::stale})) {
        return core::Result<void>::failure(validation("inspection characteristic is malformed"));
    }
    if (auto result = sensor.validate(); !result) return result;
    if (auto result = coverage.validate(); !result) return result;
    if (auto result = access.validate(); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> InspectionTarget::validate() const {
    if (id == 0U || characteristic_id == 0U || source_topology_id == 0U ||
        !sensor_pose.finite() || !std::isfinite(standoff) || standoff < 0.0 ||
        !safe_text(generation_basis) ||
        !enum_known(status, {TargetStatus::proposed, TargetStatus::accepted,
            TargetStatus::rejected, TargetStatus::stale, TargetStatus::unreachable,
            TargetStatus::collision, TargetStatus::occluded, TargetStatus::runtime_verified,
            TargetStatus::runtime_failed})) {
        return core::Result<void>::failure(validation("inspection target is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> WorkcellEntity::validate() const {
    if (id == 0U || !enum_known(kind, {WorkcellEntityKind::inspection_assembly,
        WorkcellEntityKind::fixture, WorkcellEntityKind::robot_base,
        WorkcellEntityKind::sensor_tool, WorkcellEntityKind::reference_frame,
        WorkcellEntityKind::operator_exclusion}) || !safe_text(name) ||
        !transform.finite() || !safe_text(source, true)) {
        return core::Result<void>::failure(validation("workcell entity is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> KeepOutRegion::validate() const {
    if (id == 0U || !enum_known(kind, {KeepOutKind::box, KeepOutKind::sphere,
        KeepOutKind::cylinder, KeepOutKind::mesh}) || !safe_text(purpose) ||
        !transform.finite() || !dimensions.finite() || dimensions.x <= 0.0 ||
        dimensions.y <= 0.0 || dimensions.z <= 0.0 || !safe_text(source, true)) {
        return core::Result<void>::failure(validation("keep-out region is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> EngineeringWorkcell::validate() const {
    if (id == 0U || !safe_text(revision) || !safe_text(robot_model_identity) ||
        !safe_text(robot_source_identity) || !safe_text(active_tool_frame) ||
        !safe_text(capability_profile) || !safe_text(sensor_identity)) {
        return core::Result<void>::failure(validation("engineering workcell is malformed"));
    }
    if (entities.empty()) return core::Result<void>::failure(validation("workcell has no entities"));
    std::set<Id> ids;
    for (const auto& entity : entities) {
        if (auto result = entity.validate(); !result) return result;
        if (!ids.insert(entity.id).second) return core::Result<void>::failure(validation("workcell entity id is duplicated"));
    }
    for (const auto& entity : frames) {
        if (auto result = entity.validate(); !result) return result;
        if (!ids.insert(entity.id).second) return core::Result<void>::failure(validation("workcell frame id is duplicated"));
    }
    for (const auto& keep_out : keep_outs) {
        if (auto result = keep_out.validate(); !result) return result;
        if (!ids.insert(keep_out.id).second) return core::Result<void>::failure(validation("keep-out id is duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> EngineeringWorkcell::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::ostringstream output;
    output << "WORKCELL_RECORD " << id << ' ' << std::quoted(revision) << ' '
           << std::quoted(robot_model_identity) << ' ' << std::quoted(robot_source_identity) << ' '
           << std::quoted(active_tool_frame) << ' ' << std::quoted(capability_profile) << ' '
           << std::quoted(sensor_identity) << '\n';
    output << "ENTITIES " << entities.size() << '\n';
    for (const auto& entity : entities) {
        output << "ENTITY " << entity.id << ' ' << workcell_entity_kind_name(entity.kind) << ' '
               << std::quoted(entity.name) << ' ';
        write_transform(output, entity.transform);
        output << ' ' << std::quoted(entity.source) << ' ' << (entity.enabled ? 1 : 0) << '\n';
    }
    output << "KEEP_OUTS " << keep_outs.size() << '\n';
    for (const auto& keep_out : keep_outs) {
        output << "KEEP_OUT " << keep_out.id << ' ' << keep_out_kind_name(keep_out.kind) << ' '
               << std::quoted(keep_out.purpose) << ' ';
        write_transform(output, keep_out.transform);
        output << ' ' << keep_out.dimensions.x << ' ' << keep_out.dimensions.y << ' '
               << keep_out.dimensions.z << ' ' << std::quoted(keep_out.source) << ' '
               << (keep_out.enabled ? 1 : 0) << '\n';
    }
    output << "FRAMES " << frames.size() << '\n';
    for (const auto& frame : frames) {
        output << "FRAME " << frame.id << ' ' << workcell_entity_kind_name(frame.kind) << ' '
               << std::quoted(frame.name) << ' ';
        write_transform(output, frame.transform);
        output << ' ' << std::quoted(frame.source) << ' ' << (frame.enabled ? 1 : 0) << '\n';
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(output.str()));
}

core::Result<void> DerivedProductState::validate() const {
    if (id == 0U || basis_digest.is_zero() || !enum_known(status, {
        DerivedProductStatus::current, DerivedProductStatus::stale,
        DerivedProductStatus::rejected, DerivedProductStatus::unverified}) ||
        !safe_text(stale_reason, true)) {
        return core::Result<void>::failure(validation("derived product state is malformed"));
    }
    if (status == DerivedProductStatus::stale && stale_reason.empty()) {
        return core::Result<void>::failure(validation("stale product state requires a reason"));
    }
    return core::Result<void>::success();
}

core::Result<void> PathSample::validate() const {
    if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0 ||
        !tool_pose.finite() || !enum_known(status, {PathSampleStatus::valid,
            PathSampleStatus::rejected, PathSampleStatus::stale, PathSampleStatus::unreachable,
            PathSampleStatus::collision, PathSampleStatus::clearance_warning,
            PathSampleStatus::unverified}) || !std::isfinite(clearance) || clearance < 0.0 ||
        (target_id.has_value() && *target_id == 0U)) {
        return core::Result<void>::failure(validation("path sample is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> VantaPathResultReceipt::validate() const {
    const auto safe_identity = [](std::string_view value) {
        return safe_text(value) && value.find_first_of("/\\:") == std::string_view::npos;
    };
    if (!safe_text(project_namespace) || project_revision.exhausted() ||
        authoring_revision.exhausted() || publication_digest.is_zero() ||
        authoring_digest.is_zero() || !provider_identity.starts_with("vanta.") ||
        !safe_identity(provider_identity) || !safe_identity(provider_version) ||
        !safe_identity(run_id) || result_digest.is_zero() ||
        !enum_known(reported_status, {DerivedProductStatus::current,
            DerivedProductStatus::stale, DerivedProductStatus::rejected,
            DerivedProductStatus::unverified})) {
        return core::Result<void>::failure(validation(
            "VANTA path result receipt is malformed"));
    }
    return core::Result<void>::success();
}

core::Result<void> GeneratedPath::validate() const {
    if (id == 0U || basis_digest.is_zero() || samples.empty() ||
        !enum_known(status, {DerivedProductStatus::current, DerivedProductStatus::stale,
            DerivedProductStatus::rejected, DerivedProductStatus::unverified})) {
        return core::Result<void>::failure(validation("generated path is malformed"));
    }
    std::uint32_t expected = 0U;
    for (const auto& sample : samples) {
        if (sample.sequence != expected++) return core::Result<void>::failure(validation("path sample sequence is not contiguous"));
        if (auto result = sample.validate(); !result) return result;
    }
    if (vanta_receipt.has_value()) {
        if (auto result = vanta_receipt->validate(); !result) return result;
        if ((status != DerivedProductStatus::unverified &&
             status != DerivedProductStatus::stale &&
             status != DerivedProductStatus::rejected) ||
            basis_digest != vanta_receipt->authoring_digest ||
            basis_revision != vanta_receipt->authoring_revision) {
            return core::Result<void>::failure(validation(
                "VANTA path result must remain unverified and bound to its source authoring"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> VantaPathResult::validate() const {
    const auto safe_identity = [](std::string_view value) {
        return safe_text(value) && value.find_first_of("/\\:") == std::string_view::npos;
    };
    if (!safe_text(project_namespace) || project_revision.exhausted() ||
        authoring_revision.exhausted() || publication_digest.is_zero() ||
        authoring_digest.is_zero() || !provider_identity.starts_with("vanta.") ||
        !safe_identity(provider_identity) || !safe_identity(provider_version) ||
        !safe_identity(run_id) || path.vanta_receipt.has_value() ||
        path.basis_digest != authoring_digest ||
        path.basis_revision != authoring_revision || result_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "VANTA path result header or basis is malformed"));
    }
    if (auto result = path.validate(); !result) return result;
    if (result_digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "VANTA path result digest does not bind its bytes"));
    }
    return core::Result<void>::success();
}

std::string VantaPathResult::canonical_without_digest() const {
    std::ostringstream output;
    output << "CARTO_VANTA_PATH_RESULT 1\n";
    output << "PROJECT " << std::quoted(project_namespace) << ' '
           << project_revision.value() << ' ' << authoring_revision.value() << '\n';
    output << "SOURCE " << publication_digest.hex() << ' '
           << authoring_digest.hex() << '\n';
    output << "PROVIDER " << std::quoted(provider_identity) << ' '
           << std::quoted(provider_version) << ' ' << std::quoted(run_id) << '\n';
    output << "PATH_RECORD " << path.id << ' ' << path.basis_digest.hex() << ' '
           << path.basis_revision.value() << ' '
           << derived_product_status_name(path.status) << ' '
           << path.samples.size() << '\n';
    for (const auto& sample : path.samples) {
        output << "SAMPLE " << sample.sequence << ' ' << std::setprecision(17)
               << sample.normalized << ' ';
        write_transform(output, sample.tool_pose);
        output << ' ' << sample.target_id.value_or(0U) << ' '
               << path_sample_status_name(sample.status) << ' '
               << sample.clearance << '\n';
    }
    return output.str();
}

core::Result<void> VantaPathResult::refresh_digest() {
    result_digest = digest_text(canonical_without_digest());
    return validate();
}

std::string VantaPathResult::serialize() const {
    return canonical_without_digest() + "RESULT " + result_digest.hex() + "\nEND\n";
}

core::Result<VantaPathResult> VantaPathResult::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<VantaPathResult>::failure(parse_error(
            "VANTA path result exceeds its configured size bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic >> version) || magic != "CARTO_VANTA_PATH_RESULT" ||
        version != 1U) {
        return core::Result<VantaPathResult>::failure(parse_error(
            "VANTA path result header is invalid"));
    }
    VantaPathResult result;
    std::uint64_t project_revision = 0U;
    std::uint64_t authoring_revision = 0U;
    if (!require_record(input, "PROJECT") ||
        !read_text(input, result.project_namespace, false) ||
        !read_uint(input, project_revision) || !read_uint(input, authoring_revision) ||
        !require_record(input, "SOURCE") || !read_digest(input, result.publication_digest) ||
        !read_digest(input, result.authoring_digest) || !require_record(input, "PROVIDER") ||
        !read_text(input, result.provider_identity, false) ||
        !read_text(input, result.provider_version, false) ||
        !read_text(input, result.run_id, false)) {
        return core::Result<VantaPathResult>::failure(parse_error(
            "VANTA path result source records are invalid"));
    }
    result.project_revision = core::Revision(project_revision);
    result.authoring_revision = core::Revision(authoring_revision);

    std::string status;
    std::size_t sample_count = 0U;
    std::uint64_t path_basis_revision = 0U;
    if (!require_record(input, "PATH_RECORD") || !read_id(input, result.path.id) ||
        !read_digest(input, result.path.basis_digest) ||
        !read_uint(input, path_basis_revision) || !(input >> status) ||
        !read_count(input, sample_count, AuthoringDocument::kMaxCollectionEntries)) {
        return core::Result<VantaPathResult>::failure(parse_error(
            "VANTA path result path record is invalid"));
    }
    result.path.basis_revision = core::Revision(path_basis_revision);
    const auto parsed_status = parse_named<DerivedProductStatus>(status,
        {{DerivedProductStatus::current, "current"},
         {DerivedProductStatus::stale, "stale"},
         {DerivedProductStatus::rejected, "rejected"},
         {DerivedProductStatus::unverified, "unverified"}},
        "VANTA path result status is invalid");
    if (!parsed_status) {
        return core::Result<VantaPathResult>::failure(parsed_status.error());
    }
    result.path.status = parsed_status.value();
    for (std::size_t index = 0U; index < sample_count; ++index) {
        PathSample sample;
        std::uint64_t sequence = 0U;
        std::string sample_status;
        if (!require_record(input, "SAMPLE") || !read_uint(input, sequence) ||
            sequence > std::numeric_limits<std::uint32_t>::max() ||
            !read_double(input, sample.normalized) || !read_transform(input, sample.tool_pose) ||
            !read_optional_id(input, sample.target_id) || !(input >> sample_status) ||
            !read_double(input, sample.clearance)) {
            return core::Result<VantaPathResult>::failure(parse_error(
                "VANTA path result sample record is invalid"));
        }
        const auto parsed_sample = parse_named<PathSampleStatus>(sample_status,
            {{PathSampleStatus::valid, "valid"},
             {PathSampleStatus::rejected, "rejected"},
             {PathSampleStatus::stale, "stale"},
             {PathSampleStatus::unreachable, "unreachable"},
             {PathSampleStatus::collision, "collision"},
             {PathSampleStatus::clearance_warning, "clearance-warning"},
             {PathSampleStatus::unverified, "unverified"}},
            "VANTA path result sample status is invalid");
        if (!parsed_sample) {
            return core::Result<VantaPathResult>::failure(parsed_sample.error());
        }
        sample.sequence = static_cast<std::uint32_t>(sequence);
        sample.status = parsed_sample.value();
        result.path.samples.push_back(std::move(sample));
    }
    if (!require_record(input, "RESULT") ||
        !read_digest(input, result.result_digest) || !require_end(input)) {
        return core::Result<VantaPathResult>::failure(parse_error(
            "VANTA path result trailer is invalid"));
    }
    if (auto valid = result.validate(); !valid) {
        return core::Result<VantaPathResult>::failure(valid.error().with_context(
            "VANTA path result"));
    }
    return core::Result<VantaPathResult>::success(std::move(result));
}

core::Result<void> CapabilityEvidence::validate() const {
    for (const auto* values : {&preserved, &derived, &unsupported, &runtime_validation}) {
        if (values->size() > kMaxDiagnostics) return core::Result<void>::failure(validation("capability evidence exceeds its bound"));
        for (const auto& value : *values) if (!safe_text(value)) return core::Result<void>::failure(validation("capability evidence item is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> AuthoringDocument::validate() const {
    if (!safe_text(project_namespace) || project_revision.exhausted()) {
        return core::Result<void>::failure(validation("engineering project identity is malformed"));
    }
    if (auto result = validate_collection_size(sources, "engineering sources"); !result) return result;
    if (auto result = validate_collection_size(parts, "engineering parts"); !result) return result;
    if (auto result = validate_collection_size(occurrences, "engineering occurrences"); !result) return result;
    if (auto result = validate_collection_size(topology, "engineering topology"); !result) return result;
    if (auto result = validate_collection_size(bom, "engineering BOM"); !result) return result;
    if (auto result = validate_collection_size(requirements, "engineering requirements"); !result) return result;
    if (auto result = validate_collection_size(characteristics, "engineering characteristics"); !result) return result;
    if (auto result = validate_collection_size(targets, "engineering targets"); !result) return result;
    if (auto result = validate_collection_size(derived_products, "engineering derived products"); !result) return result;
    if (!unique_ids(sources, &EngineeringSource::id) || !unique_ids(parts, &EngineeringPart::id) ||
        !unique_ids(occurrences, &EngineeringOccurrence::id) || !unique_ids(topology, &TopologyBinding::id) ||
        !unique_ids(bom, &EngineeringBomLine::id) || !unique_ids(requirements, &EngineeringRequirement::id) ||
        !unique_ids(characteristics, &InspectionCharacteristic::id) || !unique_ids(targets, &InspectionTarget::id) ||
        !unique_ids(derived_products, &DerivedProductState::id)) {
        return core::Result<void>::failure(validation("engineering identities are duplicated or zero"));
    }
    for (const auto& value : sources) if (auto result = value.validate(); !result) return result;
    for (const auto& value : parts) {
        if (auto result = value.validate(); !result) return result;
        if (!contains_id(sources, &EngineeringSource::id, value.source_id)) return core::Result<void>::failure(validation("part source is unresolved"));
    }
    for (const auto& value : occurrences) {
        if (auto result = value.validate(); !result) return result;
        if (!contains_id(parts, &EngineeringPart::id, value.part_id) || !contains_id(sources, &EngineeringSource::id, value.source_id)) return core::Result<void>::failure(validation("occurrence reference is unresolved"));
        if (value.parent_id.has_value() && !contains_id(occurrences, &EngineeringOccurrence::id, *value.parent_id)) return core::Result<void>::failure(validation("occurrence parent is unresolved"));
    }
    for (const auto& value : topology) {
        if (auto result = value.validate(); !result) return result;
        if (!contains_id(sources, &EngineeringSource::id, value.source_id) || !contains_id(parts, &EngineeringPart::id, value.part_id) || (value.occurrence_id.has_value() && !contains_id(occurrences, &EngineeringOccurrence::id, *value.occurrence_id))) return core::Result<void>::failure(validation("topology reference is unresolved"));
    }
    for (const auto& value : bom) {
        if (auto result = value.validate(); !result) return result;
        if (!contains_id(sources, &EngineeringSource::id, value.source_id) || (value.mapped_part_id.has_value() && !contains_id(parts, &EngineeringPart::id, *value.mapped_part_id))) return core::Result<void>::failure(validation("BOM reference is unresolved"));
    }
    for (const auto& value : requirements) {
        if (auto result = value.validate(); !result) return result;
        if (!contains_id(sources, &EngineeringSource::id, value.provenance.source_id)) return core::Result<void>::failure(validation("requirement source is unresolved"));
        for (const Id topology_id : value.topology_ids) if (!contains_id(topology, &TopologyBinding::id, topology_id)) return core::Result<void>::failure(validation("requirement topology binding is unresolved"));
    }
    for (const auto& value : characteristics) {
        if (auto result = value.validate(); !result) return result;
        const auto* requirement = find_id(requirements, &EngineeringRequirement::id, value.requirement_id);
        if (requirement == nullptr) return core::Result<void>::failure(validation("inspection requirement is unresolved"));
        for (const Id topology_id : value.topology_ids) {
            if (!contains_id(topology, &TopologyBinding::id, topology_id)) {
                return core::Result<void>::failure(validation("inspection topology binding is unresolved"));
            }
            if (std::find(requirement->topology_ids.begin(), requirement->topology_ids.end(),
                          topology_id) == requirement->topology_ids.end()) {
                return core::Result<void>::failure(validation(
                    "inspection characteristic topology is outside its requirement binding"));
            }
        }
    }
    for (const auto& value : targets) {
        if (auto result = value.validate(); !result) return result;
        const auto* characteristic = find_id(
            characteristics, &InspectionCharacteristic::id, value.characteristic_id);
        if (characteristic == nullptr ||
            !contains_id(topology, &TopologyBinding::id, value.source_topology_id)) {
            return core::Result<void>::failure(validation("inspection target reference is unresolved"));
        }
        if (std::find(characteristic->topology_ids.begin(), characteristic->topology_ids.end(),
                      value.source_topology_id) == characteristic->topology_ids.end()) {
            return core::Result<void>::failure(validation(
                "inspection target topology is outside its characteristic binding"));
        }
    }
    if (workcell.has_value()) if (auto result = workcell->validate(); !result) return result;
    if (workcell_revision_history.size() > kMaxCollectionEntries ||
        (!workcell.has_value() && !workcell_revision_history.empty())) {
        return core::Result<void>::failure(validation("workcell revision history is unresolved or exceeds its limit"));
    }
    std::set<std::string> workcell_revisions;
    for (std::size_t index = 0U; index < workcell_revision_history.size(); ++index) {
        const auto& record = workcell_revision_history[index];
        const bool has_justification = std::any_of(
            record.justification.begin(), record.justification.end(),
            [](unsigned char byte) { return byte != ' '; });
        const bool subject_kind_valid = record.subject_kind == "entity" ||
            record.subject_kind == "frame" || record.subject_kind == "keep-out";
        const bool subject_exists = workcell.has_value() &&
            (record.subject_kind == "entity"
                ? contains_id(workcell->entities, &WorkcellEntity::id, record.subject_id)
                : record.subject_kind == "frame"
                    ? contains_id(workcell->frames, &WorkcellEntity::id, record.subject_id)
                    : record.subject_kind == "keep-out" &&
                        contains_id(workcell->keep_outs, &KeepOutRegion::id, record.subject_id));
        if (!safe_text(record.revision, false, 128U) ||
            !safe_text(record.previous_revision, false, 128U) ||
            !subject_kind_valid || record.subject_id == 0U || !subject_exists ||
            record.state_digest.is_zero() ||
            !safe_text(record.justification, false, 1024U) || !has_justification ||
            record.revision == record.previous_revision ||
            !workcell_revisions.insert(record.revision).second ||
            (index > 0U && record.previous_revision !=
                workcell_revision_history[index - 1U].revision)) {
            return core::Result<void>::failure(validation("workcell revision history is malformed"));
        }
    }
    if (!workcell_revision_history.empty() &&
        workcell_revision_history.back().revision != workcell->revision) {
        return core::Result<void>::failure(validation("workcell revision does not match its history"));
    }
    if (!workcell_revision_history.empty()) {
        const auto workcell_digest = workcell->canonical_digest();
        if (!workcell_digest) return core::Result<void>::failure(workcell_digest.error());
        if (workcell_revision_history.back().state_digest != workcell_digest.value()) {
            return core::Result<void>::failure(validation(
                "current workcell content does not match its latest revision record"));
        }
    }
    if (path.has_value()) {
        if (auto result = path->validate(); !result) return result;
        if (path->vanta_receipt.has_value() &&
            path->vanta_receipt->project_namespace != project_namespace) {
            return core::Result<void>::failure(validation(
                "VANTA path result receipt belongs to a different project"));
        }
        for (const auto& sample : path->samples) {
            if (!sample.target_id.has_value()) continue;
            const auto target = std::find_if(targets.begin(), targets.end(),
                [&sample](const InspectionTarget& candidate) {
                    return candidate.id == *sample.target_id;
                });
            if (target == targets.end()) {
                return core::Result<void>::failure(validation(
                    "path target reference is unresolved"));
            }
            if (path->vanta_receipt.has_value() &&
                path->status == DerivedProductStatus::unverified &&
                target->status != TargetStatus::accepted) {
                return core::Result<void>::failure(validation(
                    "unverified VANTA path sample references a non-accepted target"));
            }
        }
    }
    for (const auto& value : derived_products) if (auto result = value.validate(); !result) return result;
    return evidence.validate();
}

std::string AuthoringDocument::serialize() const {
    std::ostringstream output;
    output << kAuthoringMagic << ' ' << kSchemaVersion << '\n';
    output << "PROJECT " << std::quoted(project_namespace) << ' ' << project_revision.value() << '\n';
    output << "SOURCES " << sources.size() << '\n';
    for (const auto& value : sources) {
        output << "SOURCE " << value.id << ' ' << source_kind_name(value.kind) << ' '
               << std::quoted(value.locator) << ' ' << std::quoted(value.revision) << ' '
               << value.digest.hex() << ' ' << std::quoted(value.provider_identity) << ' '
               << std::quoted(value.provider_version) << ' ' << value.import_settings_digest.hex() << ' '
               << std::quoted(value.import_run_id) << ' ' << std::quoted(value.units) << ' '
               << (value.lossy ? 1 : 0) << ' ';
        write_strings(output, value.diagnostics);
        output << '\n';
    }
    output << "PARTS " << parts.size() << '\n';
    for (const auto& value : parts) output << "PART " << value.id << ' ' << std::quoted(value.external_id) << ' ' << std::quoted(value.name) << ' ' << std::quoted(value.revision) << ' ' << std::quoted(value.material) << ' ' << (value.nominal_mass.has_value() ? 1 : 0) << ' ' << value.nominal_mass.value_or(0.0) << ' ' << value.source_id << '\n';
    output << "OCCURRENCES " << occurrences.size() << '\n';
    for (const auto& value : occurrences) { output << "OCCURRENCE " << value.id << ' ' << std::quoted(value.external_id) << ' ' << value.part_id << ' ' << (value.parent_id.value_or(0U)) << ' '; write_transform(output, value.transform); output << ' ' << value.quantity << ' ' << value.source_id << '\n'; }
    output << "TOPOLOGY " << topology.size() << '\n';
    for (const auto& value : topology) output << "TOPOLOGY_BINDING " << value.id << ' ' << value.source_id << ' ' << value.part_id << ' ' << value.occurrence_id.value_or(0U) << ' ' << topology_kind_name(value.kind) << ' ' << std::quoted(value.external_id) << ' ' << std::quoted(value.source_revision) << ' ' << value.display_mesh_asset << ' ' << value.display_triangle << '\n';
    output << "BOM " << bom.size() << '\n';
    for (const auto& value : bom) output << "BOM_LINE " << value.id << ' ' << std::quoted(value.part_number) << ' ' << std::quoted(value.revision) << ' ' << std::quoted(value.description) << ' ' << std::quoted(value.material) << ' ' << std::setprecision(17) << value.quantity << ' ' << std::quoted(value.unit) << ' ' << value.source_id << ' ' << value.mapped_part_id.value_or(0U) << ' ' << reconciliation_status_name(value.reconciliation) << '\n';
    output << "REQUIREMENTS " << requirements.size() << '\n';
    for (const auto& value : requirements) {
        output << "REQUIREMENT " << value.id << ' ' << requirement_kind_name(value.kind) << ' ' << extraction_state_name(value.extraction) << ' ' << value.provenance.source_id << ' ' << value.provenance.page << ' ' << std::quoted(value.provenance.section) << ' ' << std::quoted(value.provenance.callout) << ' ' << std::quoted(value.provenance.drawing_zone) << ' ' << std::quoted(value.provenance.extractor_identity) << ' ' << std::setprecision(17) << value.provenance.extraction_confidence << ' ' << std::quoted(value.provenance.original_text) << ' '; write_ids(output, value.topology_ids); output << ' ' << value.nominal << ' ' << value.plus << ' ' << value.minus << ' ' << std::quoted(value.unit) << ' '; write_strings(output, value.datum_refs); output << ' '; write_strings(output, value.modifier_refs); output << ' ' << std::quoted(value.acceptance_rule) << ' ' << (value.enabled ? 1 : 0) << ' ' << std::quoted(value.human_review_justification) << ' ' << std::quoted(value.unsupported_content) << '\n';
    }
    output << "CHARACTERISTICS " << characteristics.size() << '\n';
    for (const auto& value : characteristics) {
        output << "CHARACTERISTIC " << value.id << ' ' << value.requirement_id << ' ' << inspection_kind_name(value.kind) << ' '; write_ids(output, value.topology_ids); output << ' ' << std::quoted(value.sensor.sensor_type) << ' ' << std::quoted(value.sensor.tool_center_frame) << ' ' << std::setprecision(17) << value.sensor.min_standoff << ' ' << value.sensor.max_standoff << ' ' << value.sensor.preferred_standoff << ' ' << value.sensor.orientation_cone_degrees << ' ' << value.sensor.field_of_view_degrees << ' ' << value.sensor.min_clearance << ' ' << (value.sensor.line_of_sight_required ? 1 : 0) << ' ' << (value.sensor.contact_required ? 1 : 0) << ' ' << value.sensor.measurement_duration_seconds << ' ' << std::quoted(value.coverage.kind) << ' ' << value.coverage.percentage << ' '; write_strings(output, value.access.allowed_approach_directions); output << ' '; write_strings(output, value.access.forbidden_approach_directions); output << ' ' << value.access.minimum_approach_clearance << ' ' << (value.access.fixture_occlusion_allowed ? 1 : 0) << ' ' << (value.access.robot_base_reposition_permitted ? 1 : 0) << ' ' << value.priority << ' ' << inspection_status_name(value.status) << '\n';
    }
    output << "TARGETS " << targets.size() << '\n';
    for (const auto& value : targets) { output << "TARGET " << value.id << ' ' << value.characteristic_id << ' ' << value.source_topology_id << ' '; write_transform(output, value.sensor_pose); output << ' ' << std::setprecision(17) << value.standoff << ' ' << target_status_name(value.status) << ' ' << std::quoted(value.generation_basis) << ' ' << (value.human_authored ? 1 : 0) << '\n'; }
    output << "WORKCELL " << (workcell.has_value() ? 1 : 0) << '\n';
    if (workcell.has_value()) {
        const auto& value = *workcell;
        output << "WORKCELL_RECORD " << value.id << ' ' << std::quoted(value.revision) << ' ' << std::quoted(value.robot_model_identity) << ' ' << std::quoted(value.robot_source_identity) << ' ' << std::quoted(value.active_tool_frame) << ' ' << std::quoted(value.capability_profile) << ' ' << std::quoted(value.sensor_identity) << '\n';
        output << "ENTITIES " << value.entities.size() << '\n';
        for (const auto& entity : value.entities) { output << "ENTITY " << entity.id << ' ' << workcell_entity_kind_name(entity.kind) << ' ' << std::quoted(entity.name) << ' '; write_transform(output, entity.transform); output << ' ' << std::quoted(entity.source) << ' ' << (entity.enabled ? 1 : 0) << '\n'; }
        output << "KEEP_OUTS " << value.keep_outs.size() << '\n';
        for (const auto& keep_out : value.keep_outs) { output << "KEEP_OUT " << keep_out.id << ' ' << keep_out_kind_name(keep_out.kind) << ' ' << std::quoted(keep_out.purpose) << ' '; write_transform(output, keep_out.transform); output << ' ' << keep_out.dimensions.x << ' ' << keep_out.dimensions.y << ' ' << keep_out.dimensions.z << ' ' << std::quoted(keep_out.source) << ' ' << (keep_out.enabled ? 1 : 0) << '\n'; }
        output << "FRAMES " << value.frames.size() << '\n';
        for (const auto& entity : value.frames) { output << "FRAME " << entity.id << ' ' << workcell_entity_kind_name(entity.kind) << ' ' << std::quoted(entity.name) << ' '; write_transform(output, entity.transform); output << ' ' << std::quoted(entity.source) << ' ' << (entity.enabled ? 1 : 0) << '\n'; }
    }
    output << "WORKCELL_REVISION_HISTORY " << workcell_revision_history.size() << '\n';
    for (const auto& record : workcell_revision_history) {
        output << "WORKCELL_REVISION " << std::quoted(record.revision) << ' '
               << std::quoted(record.previous_revision) << ' '
               << std::quoted(record.subject_kind) << ' ' << record.subject_id << ' '
               << record.state_digest.hex() << ' '
               << std::quoted(record.justification) << '\n';
    }
    output << "PATH " << (path.has_value() ? 1 : 0) << '\n';
    if (path.has_value()) {
        output << "PATH_RECORD " << path->id << ' ' << path->basis_digest.hex() << ' ' << path->basis_revision.value() << ' ' << derived_product_status_name(path->status) << ' ' << path->samples.size() << '\n';
        output << "PATH_RESULT_RECEIPT " << (path->vanta_receipt.has_value() ? 1 : 0);
        if (path->vanta_receipt.has_value()) {
            const auto& receipt = *path->vanta_receipt;
            output << ' ' << std::quoted(receipt.project_namespace) << ' '
                   << receipt.project_revision.value() << ' '
                   << receipt.authoring_revision.value() << ' '
                   << receipt.publication_digest.hex() << ' '
                   << receipt.authoring_digest.hex() << ' '
                   << std::quoted(receipt.provider_identity) << ' '
                   << std::quoted(receipt.provider_version) << ' '
                   << std::quoted(receipt.run_id) << ' '
                   << receipt.result_digest.hex() << ' '
                   << derived_product_status_name(receipt.reported_status);
        }
        output << '\n';
        for (const auto& sample : path->samples) { output << "SAMPLE " << sample.sequence << ' ' << std::setprecision(17) << sample.normalized << ' '; write_transform(output, sample.tool_pose); output << ' ' << sample.target_id.value_or(0U) << ' ' << path_sample_status_name(sample.status) << ' ' << sample.clearance << '\n'; }
    }
    output << "DERIVED " << derived_products.size() << '\n';
    for (const auto& value : derived_products) output << "DERIVED_PRODUCT " << value.id << ' ' << value.basis_digest.hex() << ' ' << value.basis_revision.value() << ' ' << derived_product_status_name(value.status) << ' ' << std::quoted(value.stale_reason) << '\n';
    output << "EVIDENCE\n";
    output << "PRESERVED "; write_strings(output, evidence.preserved); output << '\n';
    output << "DERIVED_EVIDENCE "; write_strings(output, evidence.derived); output << '\n';
    output << "UNSUPPORTED "; write_strings(output, evidence.unsupported); output << '\n';
    output << "RUNTIME_VALIDATION "; write_strings(output, evidence.runtime_validation); output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<AuthoringDocument> AuthoringDocument::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) return core::Result<AuthoringDocument>::failure(parse_error("engineering authoring bytes exceed their bound"));
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic >> version) || magic != kAuthoringMagic ||
        (version < 1U || version > kSchemaVersion)) {
        return core::Result<AuthoringDocument>::failure(parse_error(
            "engineering authoring header is invalid"));
    }
    const std::uint64_t authoring_schema_version = version;
    AuthoringDocument document;
    if (!require_record(input, "PROJECT") || !read_text(input, document.project_namespace, false) || !read_uint(input, version)) return core::Result<AuthoringDocument>::failure(parse_error("engineering project record is invalid"));
    document.project_revision = core::Revision(version);

    auto read_section_count = [&input](std::string_view name, std::size_t& count) -> bool {
        return require_record(input, name) && read_count(input, count, AuthoringDocument::kMaxCollectionEntries);
    };
    std::size_t count = 0U;
    if (!read_section_count("SOURCES", count)) return core::Result<AuthoringDocument>::failure(parse_error("engineering sources section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) {
        EngineeringSource value; std::string kind; bool lossy = false;
        if (!require_record(input, "SOURCE") || !read_id(input, value.id) || !(input >> kind) || !read_text(input, value.locator, false) || !read_text(input, value.revision, false) || !read_digest(input, value.digest) || !read_text(input, value.provider_identity, false) || !read_text(input, value.provider_version, false) || !read_digest(input, value.import_settings_digest) || !read_text(input, value.import_run_id, false) || !read_text(input, value.units, false) || !read_bool(input, lossy) || !read_strings(input, value.diagnostics)) return core::Result<AuthoringDocument>::failure(parse_error("engineering source record is invalid"));
        const auto parsed = parse_named<SourceKind>(kind, {{SourceKind::step_ap242, "step-ap242"}, {SourceKind::bom, "bom"}, {SourceKind::drawing, "drawing"}, {SourceKind::specification, "specification"}}, "engineering source kind is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); value.kind = parsed.value(); value.lossy = lossy; document.sources.push_back(std::move(value));
    }
    if (!read_section_count("PARTS", count)) return core::Result<AuthoringDocument>::failure(parse_error("engineering parts section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { EngineeringPart value; bool has_mass = false; if (!require_record(input, "PART") || !read_id(input, value.id) || !read_text(input, value.external_id, false) || !read_text(input, value.name, false) || !read_text(input, value.revision, false) || !read_text(input, value.material) || !read_bool(input, has_mass) || !read_double(input, value.nominal_mass.emplace()) || !read_id(input, value.source_id)) return core::Result<AuthoringDocument>::failure(parse_error("engineering part record is invalid")); if (!has_mass) value.nominal_mass.reset(); document.parts.push_back(std::move(value)); }
    if (!read_section_count("OCCURRENCES", count)) return core::Result<AuthoringDocument>::failure(parse_error("engineering occurrences section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { EngineeringOccurrence value; std::uint64_t quantity = 0U; if (!require_record(input, "OCCURRENCE") || !read_id(input, value.id) || !read_text(input, value.external_id, false) || !read_id(input, value.part_id) || !read_optional_id(input, value.parent_id) || !read_transform(input, value.transform) || !read_uint(input, quantity) || !read_id(input, value.source_id) || quantity > std::numeric_limits<std::uint32_t>::max()) return core::Result<AuthoringDocument>::failure(parse_error("engineering occurrence record is invalid")); value.quantity = static_cast<std::uint32_t>(quantity); document.occurrences.push_back(std::move(value)); }
    if (!read_section_count("TOPOLOGY", count)) return core::Result<AuthoringDocument>::failure(parse_error("engineering topology section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { TopologyBinding value; std::optional<Id> occurrence; std::string kind; std::uint64_t triangle = 0U; if (!require_record(input, "TOPOLOGY_BINDING") || !read_id(input, value.id) || !read_id(input, value.source_id) || !read_id(input, value.part_id) || !read_optional_id(input, occurrence) || !(input >> kind) || !read_text(input, value.external_id, false) || !read_text(input, value.source_revision, false) || !read_uint(input, value.display_mesh_asset) || !read_uint(input, triangle)) return core::Result<AuthoringDocument>::failure(parse_error("topology binding record is invalid")); const auto parsed = parse_named<TopologyKind>(kind, {{TopologyKind::assembly, "assembly"}, {TopologyKind::part, "part"}, {TopologyKind::occurrence, "occurrence"}, {TopologyKind::body, "body"}, {TopologyKind::face, "face"}, {TopologyKind::edge, "edge"}, {TopologyKind::vertex, "vertex"}, {TopologyKind::feature, "feature"}, {TopologyKind::datum, "datum"}, {TopologyKind::topology_set, "topology-set"}}, "topology kind is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); value.occurrence_id = occurrence; value.kind = parsed.value(); value.display_triangle = static_cast<std::uint32_t>(triangle); document.topology.push_back(std::move(value)); }
    if (!read_section_count("BOM", count)) return core::Result<AuthoringDocument>::failure(parse_error("engineering BOM section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { EngineeringBomLine value; std::string status; std::optional<Id> mapped; if (!require_record(input, "BOM_LINE") || !read_id(input, value.id) || !read_text(input, value.part_number, false) || !read_text(input, value.revision, false) || !read_text(input, value.description) || !read_text(input, value.material) || !read_double(input, value.quantity) || !read_text(input, value.unit, false) || !read_id(input, value.source_id) || !read_optional_id(input, mapped) || !(input >> status)) return core::Result<AuthoringDocument>::failure(parse_error("BOM line record is invalid")); const auto parsed = parse_named<ReconciliationStatus>(status, {{ReconciliationStatus::exact, "exact"}, {ReconciliationStatus::matched_by_external_id, "matched-by-external-id"}, {ReconciliationStatus::matched_by_part_number, "matched-by-part-number"}, {ReconciliationStatus::quantity_mismatch, "quantity-mismatch"}, {ReconciliationStatus::revision_mismatch, "revision-mismatch"}, {ReconciliationStatus::material_mismatch, "material-mismatch"}, {ReconciliationStatus::source_only, "source-only"}, {ReconciliationStatus::assembly_only, "assembly-only"}, {ReconciliationStatus::ambiguous, "ambiguous"}, {ReconciliationStatus::unresolved, "unresolved"}}, "BOM reconciliation status is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); value.mapped_part_id = mapped; value.reconciliation = parsed.value(); document.bom.push_back(std::move(value)); }
    if (!read_section_count("REQUIREMENTS", count)) {
        return core::Result<AuthoringDocument>::failure(parse_error(
            "requirements section is invalid"));
    }
    for (std::size_t index = 0U; index < count; ++index) {
        EngineeringRequirement value;
        std::string kind;
        std::string extraction;
        std::uint64_t page = 0U;
        bool enabled = false;
        if (!require_record(input, "REQUIREMENT") || !read_id(input, value.id) ||
            !(input >> kind) || !(input >> extraction) ||
            !read_id(input, value.provenance.source_id) || !read_uint(input, page) ||
            page > std::numeric_limits<std::uint32_t>::max() ||
            !read_text(input, value.provenance.section) ||
            !read_text(input, value.provenance.callout) ||
            !read_text(input, value.provenance.drawing_zone) ||
            !read_text(input, value.provenance.extractor_identity) ||
            !read_double(input, value.provenance.extraction_confidence) ||
            !read_text(input, value.provenance.original_text) ||
            !read_ids(input, value.topology_ids) || !read_double(input, value.nominal) ||
            !read_double(input, value.plus) || !read_double(input, value.minus) ||
            !read_text(input, value.unit, false) || !read_strings(input, value.datum_refs) ||
            !read_strings(input, value.modifier_refs) ||
            !read_text(input, value.acceptance_rule) || !read_bool(input, enabled) ||
            (authoring_schema_version >= 3U &&
                !read_text(input, value.human_review_justification)) ||
            (authoring_schema_version >= 5U &&
                !read_text(input, value.unsupported_content))) {
            return core::Result<AuthoringDocument>::failure(parse_error(
                "requirement record is invalid"));
        }
        value.provenance.page = static_cast<std::uint32_t>(page);
        const auto parsed_kind = parse_named<RequirementKind>(kind, {
            {RequirementKind::dimensional_distance, "dimensional-distance"},
            {RequirementKind::diameter, "diameter"}, {RequirementKind::radius, "radius"},
            {RequirementKind::angle, "angle"}, {RequirementKind::thickness, "thickness"},
            {RequirementKind::clearance, "clearance"}, {RequirementKind::position, "position"},
            {RequirementKind::flatness, "flatness"},
            {RequirementKind::parallelism, "parallelism"},
            {RequirementKind::perpendicularity, "perpendicularity"},
            {RequirementKind::concentricity, "concentricity"},
            {RequirementKind::surface_condition, "surface-condition"},
            {RequirementKind::presence_absence, "presence-absence"},
            {RequirementKind::visual_surface, "visual-surface"},
            {RequirementKind::assembly_relationship, "assembly-relationship"},
            {RequirementKind::material_verification, "material-verification"}},
            "requirement kind is invalid");
        const auto parsed_extraction = parse_named<ExtractionState>(extraction, {
            {ExtractionState::manual, "manual"},
            {ExtractionState::imported_structured, "imported-structured"},
            {ExtractionState::machine_extracted_proposed, "machine-extracted-proposed"},
            {ExtractionState::human_verified, "human-verified"},
            {ExtractionState::human_corrected, "human-corrected"},
            {ExtractionState::rejected, "rejected"}, {ExtractionState::stale, "stale"}},
            "requirement extraction state is invalid");
        if (!parsed_kind || !parsed_extraction) {
            return core::Result<AuthoringDocument>::failure(
                !parsed_kind ? parsed_kind.error() : parsed_extraction.error());
        }
        value.kind = parsed_kind.value();
        value.extraction = parsed_extraction.value();
        value.enabled = enabled;
        document.requirements.push_back(std::move(value));
    }
    if (!read_section_count("CHARACTERISTICS", count)) return core::Result<AuthoringDocument>::failure(parse_error("characteristics section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { InspectionCharacteristic value; std::string kind; std::string status; std::uint64_t priority = 0U; bool line_of_sight = false, contact = false, fixture = false, reposition = false; if (!require_record(input, "CHARACTERISTIC") || !read_id(input, value.id) || !read_id(input, value.requirement_id) || !(input >> kind) || !read_ids(input, value.topology_ids) || !read_text(input, value.sensor.sensor_type, false) || !read_text(input, value.sensor.tool_center_frame, false) || !read_double(input, value.sensor.min_standoff) || !read_double(input, value.sensor.max_standoff) || !read_double(input, value.sensor.preferred_standoff) || !read_double(input, value.sensor.orientation_cone_degrees) || !read_double(input, value.sensor.field_of_view_degrees) || !read_double(input, value.sensor.min_clearance) || !read_bool(input, line_of_sight) || !read_bool(input, contact) || !read_double(input, value.sensor.measurement_duration_seconds) || !read_text(input, value.coverage.kind, false) || !read_double(input, value.coverage.percentage) || !read_strings(input, value.access.allowed_approach_directions) || !read_strings(input, value.access.forbidden_approach_directions) || !read_double(input, value.access.minimum_approach_clearance) || !read_bool(input, fixture) || !read_bool(input, reposition) || !read_uint(input, priority) || priority > std::numeric_limits<std::uint32_t>::max() || !(input >> status)) return core::Result<AuthoringDocument>::failure(parse_error("characteristic record is invalid")); const auto parsed_kind = parse_named<InspectionKind>(kind, {{InspectionKind::point_measurement, "point-measurement"}, {InspectionKind::distance, "distance"}, {InspectionKind::angle, "angle"}, {InspectionKind::diameter, "diameter"}, {InspectionKind::thickness, "thickness"}, {InspectionKind::clearance, "clearance"}, {InspectionKind::surface_scan, "surface-scan"}, {InspectionKind::visual_surface_check, "visual-surface-check"}, {InspectionKind::feature_presence, "feature-presence"}, {InspectionKind::datum_capture, "datum-capture"}, {InspectionKind::dimensional_feature_check, "dimensional-feature-check"}}, "inspection kind is invalid"); const auto parsed_status = parse_named<InspectionStatus>(status, {{InspectionStatus::authored, "authored"}, {InspectionStatus::disabled, "disabled"}, {InspectionStatus::stale, "stale"}}, "inspection status is invalid"); if (!parsed_kind || !parsed_status) return core::Result<AuthoringDocument>::failure(!parsed_kind ? parsed_kind.error() : parsed_status.error()); value.priority = static_cast<std::uint32_t>(priority); value.kind = parsed_kind.value(); value.status = parsed_status.value(); value.sensor.line_of_sight_required = line_of_sight; value.sensor.contact_required = contact; value.access.fixture_occlusion_allowed = fixture; value.access.robot_base_reposition_permitted = reposition; document.characteristics.push_back(std::move(value)); }
    if (!read_section_count("TARGETS", count)) return core::Result<AuthoringDocument>::failure(parse_error("targets section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { InspectionTarget value; std::string status; bool human = false; if (!require_record(input, "TARGET") || !read_id(input, value.id) || !read_id(input, value.characteristic_id) || !read_id(input, value.source_topology_id) || !read_transform(input, value.sensor_pose) || !read_double(input, value.standoff) || !(input >> status) || !read_text(input, value.generation_basis, false) || !read_bool(input, human)) return core::Result<AuthoringDocument>::failure(parse_error("inspection target record is invalid")); const auto parsed = parse_named<TargetStatus>(status, {{TargetStatus::proposed, "proposed"}, {TargetStatus::accepted, "accepted"}, {TargetStatus::rejected, "rejected"}, {TargetStatus::stale, "stale"}, {TargetStatus::unreachable, "unreachable"}, {TargetStatus::collision, "collision"}, {TargetStatus::occluded, "occluded"}, {TargetStatus::runtime_verified, "runtime-verified"}, {TargetStatus::runtime_failed, "runtime-failed"}}, "target status is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); value.status = parsed.value(); value.human_authored = human; document.targets.push_back(std::move(value)); }
    std::uint64_t present = 0U; if (!require_record(input, "WORKCELL") || !read_uint(input, present) || present > 1U) return core::Result<AuthoringDocument>::failure(parse_error("workcell section is invalid"));
    if (present != 0U) { EngineeringWorkcell value; if (!require_record(input, "WORKCELL_RECORD") || !read_id(input, value.id) || !read_text(input, value.revision, false) || !read_text(input, value.robot_model_identity, false) || !read_text(input, value.robot_source_identity, false) || !read_text(input, value.active_tool_frame, false) || !read_text(input, value.capability_profile, false) || !read_text(input, value.sensor_identity, false)) return core::Result<AuthoringDocument>::failure(parse_error("workcell record is invalid")); if (!read_section_count("ENTITIES", count)) return core::Result<AuthoringDocument>::failure(parse_error("workcell entities section is invalid")); for (std::size_t index = 0U; index < count; ++index) { WorkcellEntity entity; std::string kind; bool enabled = false; if (!require_record(input, "ENTITY") || !read_id(input, entity.id) || !(input >> kind) || !read_text(input, entity.name, false) || !read_transform(input, entity.transform) || !read_text(input, entity.source) || !read_bool(input, enabled)) return core::Result<AuthoringDocument>::failure(parse_error("workcell entity is invalid")); const auto parsed = parse_named<WorkcellEntityKind>(kind, {{WorkcellEntityKind::inspection_assembly, "inspection-assembly"}, {WorkcellEntityKind::fixture, "fixture"}, {WorkcellEntityKind::robot_base, "robot-base"}, {WorkcellEntityKind::sensor_tool, "sensor-tool"}, {WorkcellEntityKind::reference_frame, "reference-frame"}, {WorkcellEntityKind::operator_exclusion, "operator-exclusion"}}, "workcell entity kind is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); entity.kind = parsed.value(); entity.enabled = enabled; value.entities.push_back(std::move(entity)); } if (!read_section_count("KEEP_OUTS", count)) return core::Result<AuthoringDocument>::failure(parse_error("keep-outs section is invalid")); for (std::size_t index = 0U; index < count; ++index) { KeepOutRegion keep_out; std::string kind; bool enabled = false; if (!require_record(input, "KEEP_OUT") || !read_id(input, keep_out.id) || !(input >> kind) || !read_text(input, keep_out.purpose, false) || !read_transform(input, keep_out.transform) || !read_double(input, keep_out.dimensions.x) || !read_double(input, keep_out.dimensions.y) || !read_double(input, keep_out.dimensions.z) || !read_text(input, keep_out.source) || !read_bool(input, enabled)) return core::Result<AuthoringDocument>::failure(parse_error("keep-out record is invalid")); const auto parsed = parse_named<KeepOutKind>(kind, {{KeepOutKind::box, "box"}, {KeepOutKind::sphere, "sphere"}, {KeepOutKind::cylinder, "cylinder"}, {KeepOutKind::mesh, "mesh"}}, "keep-out kind is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); keep_out.kind = parsed.value(); keep_out.enabled = enabled; value.keep_outs.push_back(std::move(keep_out)); } if (!read_section_count("FRAMES", count)) return core::Result<AuthoringDocument>::failure(parse_error("workcell frames section is invalid")); for (std::size_t index = 0U; index < count; ++index) { WorkcellEntity entity; std::string kind; bool enabled = false; if (!require_record(input, "FRAME") || !read_id(input, entity.id) || !(input >> kind) || !read_text(input, entity.name, false) || !read_transform(input, entity.transform) || !read_text(input, entity.source) || !read_bool(input, enabled)) return core::Result<AuthoringDocument>::failure(parse_error("workcell frame is invalid")); const auto parsed = parse_named<WorkcellEntityKind>(kind, {{WorkcellEntityKind::reference_frame, "reference-frame"}}, "workcell frame kind is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); entity.kind = parsed.value(); entity.enabled = enabled; value.frames.push_back(std::move(entity)); } document.workcell = std::move(value); }
    if (authoring_schema_version >= 2U) {
        if (!read_section_count("WORKCELL_REVISION_HISTORY", count)) {
            return core::Result<AuthoringDocument>::failure(parse_error(
                "workcell revision history section is invalid"));
        }
        for (std::size_t index = 0U; index < count; ++index) {
            WorkcellRevisionRecord record;
            if (!require_record(input, "WORKCELL_REVISION") ||
                !read_text(input, record.revision, false) ||
                !read_text(input, record.previous_revision, false) ||
                !read_text(input, record.subject_kind, false) ||
                !read_id(input, record.subject_id) ||
                !read_digest(input, record.state_digest) ||
                !read_text(input, record.justification, false)) {
                return core::Result<AuthoringDocument>::failure(parse_error(
                    "workcell revision history entry is invalid"));
            }
            document.workcell_revision_history.push_back(std::move(record));
        }
    }
    if (!require_record(input, "PATH") || !read_uint(input, present) || present > 1U) return core::Result<AuthoringDocument>::failure(parse_error("path section is invalid"));
    if (present != 0U) {
        GeneratedPath value;
        std::string status;
        if (!require_record(input, "PATH_RECORD") || !read_id(input, value.id) ||
            !read_digest(input, value.basis_digest) || !read_uint(input, version) ||
            !(input >> status) ||
            !read_count(input, count, AuthoringDocument::kMaxCollectionEntries)) {
            return core::Result<AuthoringDocument>::failure(parse_error(
                "path record is invalid"));
        }
        value.basis_revision = core::Revision(version);
        const auto parsed = parse_named<DerivedProductStatus>(status,
            {{DerivedProductStatus::current, "current"},
             {DerivedProductStatus::stale, "stale"},
             {DerivedProductStatus::rejected, "rejected"},
             {DerivedProductStatus::unverified, "unverified"}},
            "path status is invalid");
        if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error());
        value.status = parsed.value();
        if (authoring_schema_version >= 4U) {
            std::uint64_t has_receipt = 0U;
            if (!require_record(input, "PATH_RESULT_RECEIPT") ||
                !read_uint(input, has_receipt) || has_receipt > 1U) {
                return core::Result<AuthoringDocument>::failure(parse_error(
                    "VANTA path result receipt section is invalid"));
            }
            if (has_receipt != 0U) {
                VantaPathResultReceipt receipt;
                std::uint64_t receipt_project_revision = 0U;
                std::uint64_t receipt_authoring_revision = 0U;
                std::string reported_status;
                if (!read_text(input, receipt.project_namespace, false) ||
                    !read_uint(input, receipt_project_revision) ||
                    !read_uint(input, receipt_authoring_revision) ||
                    !read_digest(input, receipt.publication_digest) ||
                    !read_digest(input, receipt.authoring_digest) ||
                    !read_text(input, receipt.provider_identity, false) ||
                    !read_text(input, receipt.provider_version, false) ||
                    !read_text(input, receipt.run_id, false) ||
                    !read_digest(input, receipt.result_digest) ||
                    !(input >> reported_status)) {
                    return core::Result<AuthoringDocument>::failure(parse_error(
                        "VANTA path result receipt record is invalid"));
                }
                receipt.project_revision = core::Revision(receipt_project_revision);
                receipt.authoring_revision = core::Revision(receipt_authoring_revision);
                const auto parsed_reported_status = parse_named<DerivedProductStatus>(
                    reported_status,
                    {{DerivedProductStatus::current, "current"},
                     {DerivedProductStatus::stale, "stale"},
                     {DerivedProductStatus::rejected, "rejected"},
                     {DerivedProductStatus::unverified, "unverified"}},
                    "VANTA reported path status is invalid");
                if (!parsed_reported_status) {
                    return core::Result<AuthoringDocument>::failure(
                        parsed_reported_status.error());
                }
                receipt.reported_status = parsed_reported_status.value();
                value.vanta_receipt = std::move(receipt);
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            PathSample sample;
            std::uint64_t sequence = 0U;
            std::string sample_status;
            if (!require_record(input, "SAMPLE") || !read_uint(input, sequence) ||
                sequence > std::numeric_limits<std::uint32_t>::max() ||
                !read_double(input, sample.normalized) ||
                !read_transform(input, sample.tool_pose) ||
                !read_optional_id(input, sample.target_id) || !(input >> sample_status) ||
                !read_double(input, sample.clearance)) {
                return core::Result<AuthoringDocument>::failure(parse_error(
                    "path sample is invalid"));
            }
            sample.sequence = static_cast<std::uint32_t>(sequence);
            const auto parsed_sample = parse_named<PathSampleStatus>(sample_status,
                {{PathSampleStatus::valid, "valid"},
                 {PathSampleStatus::rejected, "rejected"},
                 {PathSampleStatus::stale, "stale"},
                 {PathSampleStatus::unreachable, "unreachable"},
                 {PathSampleStatus::collision, "collision"},
                 {PathSampleStatus::clearance_warning, "clearance-warning"},
                 {PathSampleStatus::unverified, "unverified"}},
                "path sample status is invalid");
            if (!parsed_sample) {
                return core::Result<AuthoringDocument>::failure(parsed_sample.error());
            }
            sample.status = parsed_sample.value();
            value.samples.push_back(std::move(sample));
        }
        document.path = std::move(value);
    }
    if (!read_section_count("DERIVED", count)) return core::Result<AuthoringDocument>::failure(parse_error("derived section is invalid"));
    for (std::size_t index = 0U; index < count; ++index) { DerivedProductState value; std::string status; if (!require_record(input, "DERIVED_PRODUCT") || !read_id(input, value.id) || !read_digest(input, value.basis_digest) || !read_uint(input, version) || !(input >> status) || !read_text(input, value.stale_reason)) return core::Result<AuthoringDocument>::failure(parse_error("derived product record is invalid")); value.basis_revision = core::Revision(version); const auto parsed = parse_named<DerivedProductStatus>(status, {{DerivedProductStatus::current, "current"}, {DerivedProductStatus::stale, "stale"}, {DerivedProductStatus::rejected, "rejected"}, {DerivedProductStatus::unverified, "unverified"}}, "derived product status is invalid"); if (!parsed) return core::Result<AuthoringDocument>::failure(parsed.error()); value.status = parsed.value(); document.derived_products.push_back(std::move(value)); }
    if (!require_record(input, "EVIDENCE") || !require_record(input, "PRESERVED") || !read_strings(input, document.evidence.preserved) || !require_record(input, "DERIVED_EVIDENCE") || !read_strings(input, document.evidence.derived) || !require_record(input, "UNSUPPORTED") || !read_strings(input, document.evidence.unsupported) || !require_record(input, "RUNTIME_VALIDATION") || !read_strings(input, document.evidence.runtime_validation) || !require_end(input)) return core::Result<AuthoringDocument>::failure(parse_error("engineering evidence or terminal record is invalid"));
    if (auto result = document.validate(); !result) return core::Result<AuthoringDocument>::failure(result.error().with_context("engineering authoring deserialize"));
    return core::Result<AuthoringDocument>::success(std::move(document));
}

core::Result<assets::Sha256Digest> AuthoringDocument::canonical_digest() const {
    if (auto result = validate(); !result) return core::Result<assets::Sha256Digest>::failure(result.error());
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<void> AuthoringDocument::invalidate_source(
    Id source_id, std::string new_revision, assets::Sha256Digest new_digest) {
    if (source_id == 0U || !safe_text(new_revision) || new_digest.is_zero()) return core::Result<void>::failure(invalid("source invalidation requires a valid revision and digest"));
    auto* source = find_id(sources, &EngineeringSource::id, source_id);
    if (source == nullptr) return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::not_found, "engineering source is not present"));
    source->revision = std::move(new_revision);
    source->digest = new_digest;
    source->diagnostics.push_back("source revision changed; dependent authored bindings require reconciliation");
    if (source->kind == SourceKind::bom) {
        for (auto& line : bom) {
            if (line.source_id == source_id) line.reconciliation = ReconciliationStatus::unresolved;
        }
    }
    for (auto& requirement : requirements) {
        if (requirement.provenance.source_id == source_id) requirement.extraction = ExtractionState::stale;
    }
    for (auto& characteristic : characteristics) characteristic.status = InspectionStatus::stale;
    for (auto& target : targets) target.status = TargetStatus::stale;
    if (path.has_value()) { path->status = DerivedProductStatus::stale; for (auto& sample : path->samples) sample.status = PathSampleStatus::stale; }
    for (auto& product : derived_products) { product.status = DerivedProductStatus::stale; product.stale_reason = "engineering source revision changed"; }
    project_revision = project_revision.next();
    return validate();
}

core::Result<std::vector<EngineeringBomLine>> parse_bom_csv(
    std::string_view bytes, Id source_id, Id first_line_id) {
    if (bytes.empty() || bytes.size() > kMaxBomCsvBytes || source_id == 0U ||
        first_line_id == 0U) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(invalid(
            "BOM CSV must be non-empty, bounded, and bound to non-zero identities"));
    }
    if (!valid_utf8(bytes)) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
            "BOM CSV must contain valid UTF-8 text"));
    }
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.remove_prefix(3U);

    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    bool quote_closed = false;
    bool row_has_content = false;
    const auto finish_field = [&]() {
        fields.push_back(std::move(field));
        field.clear();
        quote_closed = false;
    };
    const auto finish_row = [&]() -> bool {
        finish_field();
        if (fields.empty() || (fields.size() == 1U && fields.front().empty())) return false;
        rows.push_back(std::move(fields));
        fields.clear();
        row_has_content = false;
        return rows.size() <= AuthoringDocument::kMaxCollectionEntries + 1U;
    };

    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const char byte = bytes[index];
        if (quoted) {
            if (byte == '"') {
                if (index + 1U < bytes.size() && bytes[index + 1U] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    quoted = false;
                    quote_closed = true;
                }
            } else if (byte == '\r' || byte == '\n') {
                return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                    "BOM CSV fields cannot contain line breaks"));
            } else {
                field.push_back(byte);
            }
            row_has_content = true;
            continue;
        }

        if (quote_closed) {
            if (byte == ',') {
                finish_field();
                row_has_content = true;
                continue;
            }
            if (byte == '\r' || byte == '\n') {
                if (!finish_row()) {
                    return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                        "BOM CSV contains a blank or oversized row"));
                }
                if (byte == '\r' && index + 1U < bytes.size() && bytes[index + 1U] == '\n') {
                    ++index;
                }
                continue;
            }
            return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                "BOM CSV contains data after a closing quote"));
        }

        if (byte == '"') {
            if (!field.empty()) {
                return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                    "BOM CSV quote must begin a field"));
            }
            quoted = true;
            row_has_content = true;
        } else if (byte == ',') {
            finish_field();
            row_has_content = true;
        } else if (byte == '\r' || byte == '\n') {
            if (!finish_row()) {
                return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                    "BOM CSV contains a blank or oversized row"));
            }
            if (byte == '\r' && index + 1U < bytes.size() && bytes[index + 1U] == '\n') {
                ++index;
            }
        } else {
            field.push_back(byte);
            row_has_content = true;
        }
    }
    if (quoted) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
            "BOM CSV ends inside a quoted field"));
    }
    if (row_has_content) {
        if (!finish_row()) {
            return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                "BOM CSV contains a blank or oversized row"));
        }
    }
    static const std::vector<std::string> expected_header{
        "part_number", "revision", "description", "material", "quantity", "unit"};
    if (rows.size() < 2U || rows.front() != expected_header) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
            "BOM CSV header must be part_number,revision,description,material,quantity,unit"));
    }
    if (rows.size() - 1U > AuthoringDocument::kMaxCollectionEntries) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
            "BOM CSV exceeds the engineering row limit"));
    }

    std::vector<EngineeringBomLine> result;
    result.reserve(rows.size() - 1U);
    for (std::size_t index = 1U; index < rows.size(); ++index) {
        const auto& row = rows[index];
        if (row.size() != expected_header.size() ||
            index - 1U > std::numeric_limits<Id>::max() - first_line_id) {
            return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                "BOM CSV row has the wrong number of columns or exhausts line identities"));
        }
        EngineeringBomLine line;
        line.id = first_line_id + static_cast<Id>(index - 1U);
        line.part_number = row[0];
        line.revision = row[1];
        line.description = row[2];
        line.material = row[3];
        line.unit = row[5];
        line.source_id = source_id;
        line.reconciliation = ReconciliationStatus::unresolved;
        const auto [end, error] = std::from_chars(
            row[4].data(), row[4].data() + row[4].size(), line.quantity,
            std::chars_format::general);
        if (error != std::errc{} || end != row[4].data() + row[4].size() ||
            !std::isfinite(line.quantity) || line.quantity <= 0.0) {
            return core::Result<std::vector<EngineeringBomLine>>::failure(parse_error(
                "BOM CSV quantity must be a finite positive decimal"));
        }
        if (auto valid = line.validate(); !valid) {
            return core::Result<std::vector<EngineeringBomLine>>::failure(
                valid.error().with_context("BOM CSV row " + std::to_string(index + 1U)));
        }
        result.push_back(std::move(line));
    }
    return core::Result<std::vector<EngineeringBomLine>>::success(std::move(result));
}

core::Result<std::vector<RequirementCsvRecord>> parse_requirement_csv(
    std::string_view bytes) {
    auto parsed_rows = parse_csv_rows(bytes, kMaxRequirementCsvBytes,
        "engineering requirement CSV");
    if (!parsed_rows) {
        return core::Result<std::vector<RequirementCsvRecord>>::failure(
            parsed_rows.error());
    }
    static const std::vector<std::string> expected_header{
        "page", "section", "callout", "kind", "topology_external_id", "nominal",
        "plus", "minus", "unit", "acceptance_rule", "original_text"};
    static const std::vector<std::string> annotated_header{
        "page", "section", "callout", "kind", "topology_external_id", "nominal",
        "plus", "minus", "unit", "acceptance_rule", "original_text",
        "unsupported_content"};
    const auto& rows = parsed_rows.value();
    const bool has_unsupported_content = rows.front() == annotated_header;
    if (!has_unsupported_content && rows.front() != expected_header) {
        return core::Result<std::vector<RequirementCsvRecord>>::failure(parse_error(
            "requirement CSV header must use the bounded profile, optionally followed by unsupported_content"));
    }

    std::vector<RequirementCsvRecord> result;
    result.reserve(rows.size() - 1U);
    for (std::size_t index = 1U; index < rows.size(); ++index) {
        const auto& row = rows[index];
        const std::string row_context = "requirement CSV row " + std::to_string(index + 1U);
        if (row.size() != (has_unsupported_content ? annotated_header.size() : expected_header.size())) {
            return core::Result<std::vector<RequirementCsvRecord>>::failure(parse_error(
                row_context + " has the wrong number of columns"));
        }
        RequirementCsvRecord record;
        const auto [page_end, page_error] = std::from_chars(
            row[0].data(), row[0].data() + row[0].size(), record.page);
        const auto kind = parse_named<RequirementKind>(row[3], {
            {RequirementKind::dimensional_distance, "dimensional-distance"},
            {RequirementKind::diameter, "diameter"},
            {RequirementKind::radius, "radius"},
            {RequirementKind::angle, "angle"},
            {RequirementKind::thickness, "thickness"},
            {RequirementKind::clearance, "clearance"}}, "unsupported requirement kind");
        const auto parse_number = [](const std::string& value, double& destination) {
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), destination,
                std::chars_format::general);
            return error == std::errc{} && end == value.data() + value.size() &&
                std::isfinite(destination);
        };
        if (page_error != std::errc{} || page_end != row[0].data() + row[0].size() ||
            !kind || !parse_number(row[5], record.nominal) ||
            !parse_number(row[6], record.plus) || !parse_number(row[7], record.minus)) {
            return core::Result<std::vector<RequirementCsvRecord>>::failure(parse_error(
                row_context + " has an invalid page, kind, or numeric value"));
        }
        record.section = row[1];
        record.callout = row[2];
        record.kind = kind.value();
        record.topology_external_id = row[4];
        record.unit = row[8];
        record.acceptance_rule = row[9];
        record.original_text = row[10];
        if (has_unsupported_content) record.unsupported_content = row[11];

        const bool angular = record.kind == RequirementKind::angle;
        const bool requires_positive_nominal = record.kind == RequirementKind::diameter ||
            record.kind == RequirementKind::radius || record.kind == RequirementKind::thickness;
        const bool supported_unit = angular
            ? record.unit == "degrees"
            : record.unit == "millimeters" || record.unit == "meters" ||
                record.unit == "inches";
        if (!safe_text(record.section) || !safe_text(record.callout) ||
            !safe_text(record.topology_external_id) || !safe_text(record.unit) ||
            !safe_text(record.acceptance_rule) ||
            !safe_text(record.original_text) || !supported_unit ||
            !safe_text(record.unsupported_content, true, 4096U) ||
            record.plus < 0.0 || record.minus < 0.0 ||
            (requires_positive_nominal ? record.nominal <= 0.0 : record.nominal < 0.0) ||
            (angular && (record.nominal > 360.0 ||
                record.nominal - record.minus < 0.0 ||
                record.nominal + record.plus > 360.0))) {
            return core::Result<std::vector<RequirementCsvRecord>>::failure(parse_error(
                row_context + " is outside the supported requirement profile"));
        }
        result.push_back(std::move(record));
    }
    return core::Result<std::vector<RequirementCsvRecord>>::success(std::move(result));
}

std::optional<double> assembly_quantity_for_part(
    const AuthoringDocument& authoring, Id part_id) {
    if (part_id == 0U || authoring.occurrences.empty()) return std::nullopt;
    std::map<Id, const EngineeringOccurrence*> occurrences;
    for (const auto& occurrence : authoring.occurrences) occurrences.emplace(occurrence.id, &occurrence);
    std::map<Id, std::optional<long double>> memo;
    std::set<Id> visiting;
    std::function<std::optional<long double>(Id, std::size_t)> multiplier =
        [&](Id id, std::size_t depth) -> std::optional<long double> {
            if (const auto found = memo.find(id); found != memo.end()) return found->second;
            const auto found = occurrences.find(id);
            if (found == occurrences.end() || depth >= 256U || !visiting.insert(id).second) {
                return std::nullopt;
            }
            const auto& occurrence = *found->second;
            long double parent_multiplier = 1.0L;
            if (occurrence.parent_id.has_value()) {
                const auto parent = multiplier(*occurrence.parent_id, depth + 1U);
                if (!parent.has_value()) {
                    visiting.erase(id);
                    memo[id] = std::nullopt;
                    return std::nullopt;
                }
                parent_multiplier = *parent;
            }
            const long double value = parent_multiplier * occurrence.quantity;
            visiting.erase(id);
            if (!std::isfinite(value) || value > std::numeric_limits<double>::max()) {
                memo[id] = std::nullopt;
                return std::nullopt;
            }
            memo[id] = value;
            return value;
        };

    long double total = 0.0L;
    bool found_part = false;
    for (const auto& occurrence : authoring.occurrences) {
        if (occurrence.part_id != part_id) continue;
        found_part = true;
        const auto count = multiplier(occurrence.id, 0U);
        if (!count.has_value()) return std::nullopt;
        total += *count;
        if (!std::isfinite(total) || total > std::numeric_limits<double>::max()) {
            return std::nullopt;
        }
    }
    if (!found_part || total <= 0.0L) return std::nullopt;
    return static_cast<double>(total);
}

core::Result<std::vector<EngineeringBomLine>> reconcile_bom_lines(
    const AuthoringDocument& authoring,
    std::vector<EngineeringBomLine> imported_lines,
    Id source_id,
    Id first_line_id) {
    if (auto valid = authoring.validate(); !valid) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(valid.error());
    }
    if (source_id == 0U || first_line_id == 0U || imported_lines.empty()) {
        return core::Result<std::vector<EngineeringBomLine>>::failure(invalid(
            "BOM reconciliation requires lines and non-zero source identities"));
    }

    std::set<std::string> imported_part_numbers;
    for (auto& line : imported_lines) {
        if (line.source_id != source_id) {
            return core::Result<std::vector<EngineeringBomLine>>::failure(invalid(
                "BOM reconciliation source does not match imported line provenance"));
        }
        imported_part_numbers.insert(line.part_number);
        std::vector<const EngineeringPart*> matches;
        for (const auto& part : authoring.parts) {
            if (part.external_id == line.part_number) matches.push_back(&part);
        }
        line.mapped_part_id.reset();
        if (matches.empty()) {
            line.reconciliation = ReconciliationStatus::source_only;
            continue;
        }
        if (matches.size() != 1U) {
            line.reconciliation = ReconciliationStatus::ambiguous;
            continue;
        }
        const auto& part = *matches.front();
        line.mapped_part_id = part.id;
        if (line.revision != part.revision) {
            line.reconciliation = ReconciliationStatus::revision_mismatch;
            continue;
        }
        if (!line.material.empty() && line.material != part.material) {
            line.reconciliation = ReconciliationStatus::material_mismatch;
            continue;
        }
        if (line.material.empty()) {
            line.reconciliation = ReconciliationStatus::unresolved;
            continue;
        }
        if (line.unit != "each") {
            line.reconciliation = ReconciliationStatus::unresolved;
            continue;
        }
        const auto assembly_quantity = assembly_quantity_for_part(authoring, part.id);
        if (!assembly_quantity.has_value()) {
            line.reconciliation = ReconciliationStatus::unresolved;
            continue;
        }
        const double tolerance = std::max({1.0, std::abs(line.quantity),
            std::abs(*assembly_quantity)}) * 1e-9;
        line.reconciliation = std::abs(line.quantity - *assembly_quantity) <= tolerance
            ? ReconciliationStatus::exact : ReconciliationStatus::quantity_mismatch;
    }

    std::vector<EngineeringBomLine> result = std::move(imported_lines);
    for (const auto& part : authoring.parts) {
        if (imported_part_numbers.contains(part.external_id)) continue;
        const auto quantity = assembly_quantity_for_part(authoring, part.id);
        if (!quantity.has_value()) continue;
        if (result.size() >= AuthoringDocument::kMaxCollectionEntries ||
            first_line_id > std::numeric_limits<Id>::max() - result.size()) {
            return core::Result<std::vector<EngineeringBomLine>>::failure(invalid(
                "BOM reconciliation exceeds the line identity or collection limit"));
        }
        EngineeringBomLine line;
        line.id = first_line_id + static_cast<Id>(result.size());
        line.part_number = part.external_id;
        line.revision = part.revision;
        line.description = part.name;
        line.material = part.material;
        line.quantity = *quantity;
        line.unit = "each";
        line.source_id = source_id;
        line.mapped_part_id = part.id;
        line.reconciliation = ReconciliationStatus::assembly_only;
        result.push_back(std::move(line));
    }
    return core::Result<std::vector<EngineeringBomLine>>::success(std::move(result));
}

core::Result<void> InspectionPackage::validate() const {
    if (!safe_text(project_namespace) || authoring_digest.is_zero() || authoring_bytes.empty() || authoring_bytes.size() > AuthoringDocument::kMaxSerializedBytes || !safe_text(publication_profile) || publication_digest.is_zero()) return core::Result<void>::failure(validation("inspection package header is malformed"));
    const auto authoring = AuthoringDocument::deserialize(authoring_bytes);
    if (!authoring) return core::Result<void>::failure(authoring.error().with_context("inspection package authoring"));
    if (contains_local_path_reference(authoring.value())) {
        return core::Result<void>::failure(validation(
            "inspection package contains local filesystem path references"));
    }
    const auto digest = authoring.value().canonical_digest();
    if (!digest || digest.value() != authoring_digest || authoring.value().project_namespace != project_namespace) return core::Result<void>::failure(validation("inspection package authoring digest or namespace does not match"));
    if (auto result = evidence.validate(); !result) return result;
    if (publication_digest != digest_text(canonical_without_digest())) return core::Result<void>::failure(validation("inspection package publication digest does not bind its bytes"));
    return core::Result<void>::success();
}

std::string InspectionPackage::canonical_without_digest() const {
    std::ostringstream output;
    output << kPackageMagic << ' ' << kSchemaVersion << '\n' << "PROJECT " << std::quoted(project_namespace) << ' ' << project_revision.value() << '\n' << "AUTHORING " << authoring_digest.hex() << ' ' << std::quoted(authoring_bytes) << '\n' << "PROFILE " << std::quoted(publication_profile) << '\n';
    output << "PRESERVED "; write_strings(output, evidence.preserved); output << '\n' << "DERIVED "; write_strings(output, evidence.derived); output << '\n' << "UNSUPPORTED "; write_strings(output, evidence.unsupported); output << '\n' << "RUNTIME_VALIDATION "; write_strings(output, evidence.runtime_validation); output << '\n' << "END\n";
    return output.str();
}

core::Result<void> InspectionPackage::refresh_digest() {
    if (auto result = evidence.validate(); !result) return result;
    const auto authoring = AuthoringDocument::deserialize(authoring_bytes);
    if (!authoring) return core::Result<void>::failure(authoring.error());
    const auto digest = authoring.value().canonical_digest();
    if (!digest) return core::Result<void>::failure(digest.error());
    authoring_digest = digest.value();
    project_namespace = authoring.value().project_namespace;
    publication_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string InspectionPackage::serialize() const { return canonical_without_digest() + "PUBLICATION " + publication_digest.hex() + "\n"; }

core::Result<InspectionPackage> InspectionPackage::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) return core::Result<InspectionPackage>::failure(parse_error("inspection package bytes exceed their bound"));
    std::istringstream input{std::string(text)}; std::string magic; std::uint64_t version = 0U;
    if (!(input >> magic >> version) || magic != kPackageMagic || version != kSchemaVersion) return core::Result<InspectionPackage>::failure(parse_error("inspection package header is invalid"));
    InspectionPackage package; std::uint64_t revision = 0U;
    if (!require_record(input, "PROJECT") || !read_text(input, package.project_namespace, false) || !read_uint(input, revision) || !require_record(input, "AUTHORING") || !read_digest(input, package.authoring_digest) || !(input >> std::quoted(package.authoring_bytes)) || package.authoring_bytes.empty() || package.authoring_bytes.size() > AuthoringDocument::kMaxSerializedBytes || !require_record(input, "PROFILE") || !read_text(input, package.publication_profile, false) || !require_record(input, "PRESERVED") || !read_strings(input, package.evidence.preserved) || !require_record(input, "DERIVED") || !read_strings(input, package.evidence.derived) || !require_record(input, "UNSUPPORTED") || !read_strings(input, package.evidence.unsupported) || !require_record(input, "RUNTIME_VALIDATION") || !read_strings(input, package.evidence.runtime_validation) || !require_record(input, "END") || !require_record(input, "PUBLICATION") || !read_digest(input, package.publication_digest)) return core::Result<InspectionPackage>::failure(parse_error("inspection package records are invalid"));
    package.project_revision = core::Revision(revision);
    if (auto result = package.validate(); !result) return core::Result<InspectionPackage>::failure(result.error().with_context("inspection package deserialize"));
    return core::Result<InspectionPackage>::success(std::move(package));
}

core::Result<InspectionPackage> InspectionPackage::publish(
    const AuthoringDocument& authoring, core::Revision project_revision,
    std::string project_namespace) {
    if (auto result = authoring.validate(); !result) return core::Result<InspectionPackage>::failure(result.error());
    if (project_revision.exhausted() || project_namespace != authoring.project_namespace) return core::Result<InspectionPackage>::failure(invalid("inspection publication project identity is invalid"));
    for (const auto& requirement : authoring.requirements) {
        if (!requirement.enabled) continue;
        if (requirement.extraction == ExtractionState::stale) {
            return core::Result<InspectionPackage>::failure(stale(
                "publication is blocked by a stale engineering requirement"));
        }
        if (requirement.extraction == ExtractionState::rejected ||
            requirement.extraction == ExtractionState::machine_extracted_proposed) {
            return core::Result<InspectionPackage>::failure(validation(
                "publication is blocked until every enabled requirement is human accepted or corrected"));
        }
        const bool has_characteristic = std::any_of(
            authoring.characteristics.begin(), authoring.characteristics.end(),
            [&requirement](const InspectionCharacteristic& characteristic) {
                return characteristic.requirement_id == requirement.id &&
                    characteristic.status != InspectionStatus::disabled &&
                    characteristic.status != InspectionStatus::stale;
            });
        if (!has_characteristic) {
            return core::Result<InspectionPackage>::failure(validation(
                "publication requires an active inspection characteristic for every enabled requirement"));
        }
    }
    for (const auto& characteristic : authoring.characteristics) {
        if (characteristic.status == InspectionStatus::stale) return core::Result<InspectionPackage>::failure(stale("publication is blocked by a stale inspection characteristic"));
        if (characteristic.status == InspectionStatus::disabled) continue;
        const bool accepted = std::any_of(authoring.targets.begin(), authoring.targets.end(), [&characteristic](const InspectionTarget& target) { return target.characteristic_id == characteristic.id && (target.status == TargetStatus::accepted || target.status == TargetStatus::runtime_verified); });
        if (!accepted) return core::Result<InspectionPackage>::failure(validation("publication requires one accepted target for every enabled characteristic"));
    }
    if (!authoring.workcell.has_value()) return core::Result<InspectionPackage>::failure(validation("publication requires an authored workcell"));
    AuthoringDocument publication_authoring = authoring;
    publication_authoring.path.reset();
    std::erase_if(publication_authoring.derived_products,
        [](const DerivedProductState& product) {
            return product.status == DerivedProductStatus::stale;
        });
    if (std::find(publication_authoring.evidence.preserved.begin(),
            publication_authoring.evidence.preserved.end(),
            "Cartographer retains prior path results locally; VANTA request packages omit them") ==
        publication_authoring.evidence.preserved.end()) {
        publication_authoring.evidence.preserved.push_back(
            "Cartographer retains prior path results locally; VANTA request packages omit them");
    }
    if (auto result = publication_authoring.validate(); !result) {
        return core::Result<InspectionPackage>::failure(result.error().with_context(
            "inspection publication projection"));
    }
    InspectionPackage package;
    package.project_namespace = std::move(project_namespace);
    package.project_revision = project_revision;
    package.authoring_bytes = publication_authoring.serialize();
    package.evidence = publication_authoring.evidence;
    if (auto result = package.refresh_digest(); !result) {
        return core::Result<InspectionPackage>::failure(result.error());
    }
    if (auto result = package.validate(); !result) {
        return core::Result<InspectionPackage>::failure(result.error());
    }
    return core::Result<InspectionPackage>::success(std::move(package));
}

core::Result<void> PackageAdmissionResult::validate() const {
    if (!enum_known(status, {PackageAdmissionStatus::admit, PackageAdmissionStatus::refuse})) return core::Result<void>::failure(validation("package admission status is invalid"));
    if (status == PackageAdmissionStatus::admit && (admitted_publication_digest.is_zero() || admitted_authoring_digest.is_zero() || !reasons.empty())) return core::Result<void>::failure(validation("admitted package result is incomplete"));
    if (status == PackageAdmissionStatus::refuse && reasons.empty()) return core::Result<void>::failure(validation("refused package result requires reasons"));
    return core::Result<void>::success();
}

core::Result<PackageAdmissionResult> admit_for_consumer(const InspectionPackage& package) {
    if (auto result = package.validate(); !result) return core::Result<PackageAdmissionResult>::failure(result.error());
    const auto authoring = AuthoringDocument::deserialize(package.authoring_bytes);
    if (!authoring) return core::Result<PackageAdmissionResult>::failure(authoring.error());
    PackageAdmissionResult result; result.admitted_publication_digest = package.publication_digest; result.admitted_authoring_digest = package.authoring_digest;
    for (const auto& source : authoring.value().sources) if (source.lossy) result.reasons.push_back("source " + std::to_string(source.id) + " declares lossy import");
    for (const auto& line : authoring.value().bom) if (line.reconciliation == ReconciliationStatus::ambiguous || line.reconciliation == ReconciliationStatus::unresolved) result.reasons.push_back("BOM line " + std::to_string(line.id) + " is unresolved");
    for (const auto& target : authoring.value().targets) if (target.status == TargetStatus::stale) result.reasons.push_back("target " + std::to_string(target.id) + " is stale");
    if (authoring.value().path.has_value() &&
        (authoring.value().path->status == DerivedProductStatus::stale ||
         std::any_of(authoring.value().path->samples.begin(),
             authoring.value().path->samples.end(), [](const PathSample& sample) {
                 return sample.status == PathSampleStatus::stale;
             }))) {
        result.reasons.push_back("inspection path is stale");
    }
    for (const auto& product : authoring.value().derived_products) {
        if (product.status == DerivedProductStatus::stale) {
            result.reasons.push_back(
                "derived engineering product " + std::to_string(product.id) + " is stale");
        }
    }
    if (authoring.value().workcell->active_tool_frame.empty()) result.reasons.push_back("active tool frame is missing");
    if (!result.reasons.empty()) { result.status = PackageAdmissionStatus::refuse; } else { result.status = PackageAdmissionStatus::admit; }
    if (auto valid_result = result.validate(); !valid_result) return core::Result<PackageAdmissionResult>::failure(valid_result.error());
    return core::Result<PackageAdmissionResult>::success(std::move(result));
}

core::Result<AuthoringDocument> make_dv012_fixture() {
    AuthoringDocument document; document.project_namespace = "cartographer.dv012.fixture"; document.project_revision = core::Revision(1U);
    const auto source_digest = digest_text("dv012-step-ap242-fixture-revision-c"); const auto bom_digest = digest_text("dv012-bom-revision-c"); const auto settings_digest = digest_text("provider=fixture;units=millimeters;tessellation=bounded");
    document.sources = {{1U, SourceKind::step_ap242, "fixture://dv012/assembly.step", "C", source_digest, "cartographer.fixture-cad", "1.0.0", settings_digest, "fixture-import-1", "millimeters", false, {}}, {2U, SourceKind::bom, "fixture://dv012/assembly.csv", "C", bom_digest, "cartographer.fixture-bom", "1.0.0", settings_digest, "fixture-import-1", "each", false, {}}};
    document.parts = {{10U, "BRACKET-A17", "Bracket A17", "C", "7075-T6", 0.84, 1U}, {11U, "PIN-P04", "Pin P04", "B", "17-4PH", 0.12, 1U}};
    document.occurrences = {{20U, "BRACKET-A17:1", 10U, std::nullopt, core::Transform::identity(), 1U, 1U}, {21U, "BRACKET-A17:2", 10U, std::nullopt, core::Transform{{0.12, 0.0, 0.0}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 1U, 1U}, {22U, "PIN-P04:1", 11U, std::nullopt, core::Transform{{0.06, 0.04, 0.0}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 1U, 1U}};
    document.topology = {{100U, 1U, 10U, 20U, TopologyKind::face, "FACE-A17-OUTER", "C", 1000U, 12U}, {101U, 1U, 10U, 20U, TopologyKind::edge, "EDGE-A17-DATUM", "C", 1000U, 13U}, {102U, 1U, 11U, 22U, TopologyKind::face, "FACE-P04-END", "C", 1001U, 4U}};
    document.bom = {{200U, "BRACKET-A17", "C", "Machined bracket", "7075-T6", 2.0, "each", 2U, 10U, ReconciliationStatus::exact}, {201U, "PIN-P04", "B", "Inspection pin", "17-4PH", 1.0, "each", 2U, 11U, ReconciliationStatus::exact}};
    document.requirements = {{300U, RequirementKind::dimensional_distance, ExtractionState::human_verified, {1U, 34U, "Section 4", "DIM-17", "B4", "fixture-pmi", 0.98, "12.50 +/- 0.05 mm"}, {100U, 101U}, 12.5, 0.05, 0.05, "millimeters", {"A", "B"}, {}, "within bilateral tolerance", true, {}, {}}, {301U, RequirementKind::visual_surface, ExtractionState::human_corrected, {1U, 34U, "Section 4", "SURF-02", "C2", "fixture-pmi", 0.91, "No visible tool marks"}, {100U}, 0.0, 0.0, 0.0, "visual", {}, {}, "uniform surface", true, {}, {}}, {302U, RequirementKind::diameter, ExtractionState::imported_structured, {1U, 34U, "Section 5", "DIA-03", "D1", "fixture-pmi", 0.88, "4.00 +/- 0.02 mm"}, {102U}, 4.0, 0.02, 0.02, "millimeters", {"A"}, {}, "within bilateral tolerance", true, {}, {}}};
    SensorConstraint sensor{"structured-light", "tool0", 0.10, 0.35, 0.20, 25.0, 60.0, 0.05, true, false, 0.5};
    AccessConstraint access{{"+z"}, {"-x"}, 0.05, false, false};
    document.characteristics = {{400U, 300U, InspectionKind::distance, {100U, 101U}, sensor, {"single_point", 0.0}, access, 10U, InspectionStatus::authored}, {401U, 301U, InspectionKind::visual_surface_check, {100U}, sensor, {"full_visible_surface", 100.0}, access, 20U, InspectionStatus::authored}, {402U, 302U, InspectionKind::diameter, {102U}, sensor, {"feature_loop", 100.0}, access, 30U, InspectionStatus::authored}};
    document.targets = {{500U, 400U, 100U, core::Transform{{0.0, 0.0, 0.20}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 0.20, TargetStatus::accepted, "fixture-normal-offset-v1", false}, {501U, 401U, 100U, core::Transform{{0.0, 0.0, 0.22}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 0.22, TargetStatus::rejected, "fixture-normal-offset-v1", false}, {502U, 401U, 100U, core::Transform{{0.01, 0.0, 0.20}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 0.20, TargetStatus::accepted, "human-corrected-offset-v1", true}, {503U, 402U, 102U, core::Transform{{0.06, 0.04, 0.20}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 0.20, TargetStatus::accepted, "fixture-normal-offset-v1", false}};
    document.workcell = EngineeringWorkcell{600U, "C", {{601U, WorkcellEntityKind::inspection_assembly, "Assembly", core::Transform::identity(), "fixture://dv012/assembly.step", true}, {602U, WorkcellEntityKind::fixture, "Fixture", core::Transform::identity(), "fixture://dv012/fixture", true}, {603U, WorkcellEntityKind::robot_base, "Robot Base", core::Transform::identity(), "fixture://dv012/robot", true}, {604U, WorkcellEntityKind::sensor_tool, "Structured Light", core::Transform::identity(), "fixture://dv012/sensor", true}}, {{610U, KeepOutKind::box, "fixture body", core::Transform{{0.0, 0.0, 0.1}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, {0.5, 0.5, 0.25}, "fixture://dv012/keepout", true}}, {{620U, WorkcellEntityKind::reference_frame, "Inspection Datum A", core::Transform::identity(), "fixture://dv012/datum-a", true}}, "robot.family.example", "fixture://dv012/robot.urdf", "tool0", "dv012-robot-inspection-v1", "structured-light-v1"};
    const auto authoring_digest = digest_text("fixture-target-basis");
    document.path = GeneratedPath{700U, authoring_digest, core::Revision(1U), DerivedProductStatus::unverified, {{0U, 0.0, core::Transform::identity(), 500U, PathSampleStatus::unverified, 0.25}, {1U, 1.0, core::Transform{{0.06, 0.04, 0.20}, core::Quaternion::identity(), {1.0, 1.0, 1.0}}, 503U, PathSampleStatus::unverified, 0.25}}, std::nullopt};
    document.derived_products = {{701U, authoring_digest, core::Revision(1U), DerivedProductStatus::unverified, "downstream runtime path feasibility is unverified"}};
    document.evidence.preserved = {"source identity", "assembly part and occurrence identity", "topology/display correspondence", "requirement source evidence", "human corrections"};
    document.evidence.derived = {"candidate inspection targets", "path preview"};
    document.evidence.unsupported = {"full Y14.5 semantic coverage", "CAD B-Rep authoring", "continuous robot planning"};
    document.evidence.runtime_validation = {"the consuming application must validate reachability, collision, and execution feasibility"};
    if (auto result = document.validate(); !result) return core::Result<AuthoringDocument>::failure(result.error());
    return core::Result<AuthoringDocument>::success(std::move(document));
}

} // namespace carto::engineering
