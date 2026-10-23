#include <carto/project/authoring_standards.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::project {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PROJECT_AUTHORING_STANDARDS";
constexpr std::size_t kMaxIdBytes = 64U;
constexpr std::size_t kMaxLabelBytes = 128U;
constexpr std::size_t kMaxValueBytes = 512U;
constexpr std::size_t kMaxRationaleBytes = 512U;

core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::invalid_argument, std::move(message)};
}

core::Diagnostic malformed(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}

bool safe_identifier(std::string_view value) {
    if (value.empty() || value.size() > kMaxIdBytes) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
    });
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool numeric_category(ProjectStandardCategory category) {
    switch (category) {
    case ProjectStandardCategory::interior_door_width:
    case ProjectStandardCategory::guest_room_wall_height:
    case ProjectStandardCategory::stair_rise:
    case ProjectStandardCategory::stair_run:
    case ProjectStandardCategory::bevel_width:
    case ProjectStandardCategory::texel_density:
    case ProjectStandardCategory::lightmap_density:
    case ProjectStandardCategory::grid_increment:
        return true;
    case ProjectStandardCategory::material_family:
    case ProjectStandardCategory::naming_policy:
    case ProjectStandardCategory::collision_profile:
    case ProjectStandardCategory::common_socket:
    case ProjectStandardCategory::unit_convention:
        return false;
    }
    return false;
}

std::string_view required_unit(ProjectStandardCategory category) {
    switch (category) {
    case ProjectStandardCategory::interior_door_width:
    case ProjectStandardCategory::guest_room_wall_height:
    case ProjectStandardCategory::stair_rise:
    case ProjectStandardCategory::stair_run:
    case ProjectStandardCategory::bevel_width:
    case ProjectStandardCategory::grid_increment:
        return "m";
    case ProjectStandardCategory::texel_density:
    case ProjectStandardCategory::lightmap_density:
        return "px/m";
    case ProjectStandardCategory::material_family:
    case ProjectStandardCategory::naming_policy:
    case ProjectStandardCategory::collision_profile:
    case ProjectStandardCategory::common_socket:
    case ProjectStandardCategory::unit_convention:
        return "";
    }
    return "";
}

std::optional<double> parse_positive_scalar(std::string_view value) {
    if (value.empty()) return std::nullopt;
    double parsed = 0.0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed, std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
        std::isfinite(parsed) && parsed > 0.0
        ? std::optional<double>{parsed} : std::nullopt;
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

bool read_finite_real(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token)) return false;
    double parsed = 0.0;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::string canonical_real(double value) {
    std::array<char, 64U> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{}) return {};
    return std::string(buffer.data(), converted.ptr);
}

std::string standard_evidence_text(
    ProjectStandardCategory category,
    const ProjectStandardEvidence& evidence) {
    std::string text = "CARTOGRAPHER_PROJECT_STANDARD_EVIDENCE 1\n";
    text += "category " + std::to_string(static_cast<unsigned>(category)) + "\n";
    text += "source " + std::to_string(evidence.source_revision.value()) + "\n";
    text += "tolerance " + canonical_real(evidence.tolerance_meters) + "\n";
    text += "measurements " + std::to_string(evidence.measurements.size()) + "\n";
    for (const auto& measurement : evidence.measurements) {
        text += "measurement " + std::to_string(measurement.object_id) + " " +
            std::to_string(measurement.mesh_asset) + " " +
            std::to_string(measurement.mesh_revision.value()) + " " +
            canonical_real(measurement.measured_value_meters) + "\n";
    }
    text += "END\n";
    return text;
}

bool inferable_standard_category(ProjectStandardCategory category) {
    return category == ProjectStandardCategory::interior_door_width ||
        category == ProjectStandardCategory::guest_room_wall_height;
}

double measurement_median(
    const std::vector<ProjectStandardMeasurementEvidence>& measurements) {
    std::vector<double> values;
    values.reserve(measurements.size());
    for (const auto& measurement : measurements) {
        values.push_back(measurement.measured_value_meters);
    }
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2U;
    if (values.size() % 2U != 0U) return values[middle];
    return values[middle - 1U] / 2.0 + values[middle] / 2.0;
}

} // namespace

core::Result<assets::Sha256Digest> ProjectStandardEvidence::canonical_digest(
    ProjectStandardCategory category) const {
    if (!inferable_standard_category(category) || source_revision.value() == 0U ||
        !std::isfinite(tolerance_meters) || tolerance_meters <= 0.0 ||
        tolerance_meters > ProjectStandardEvidence::kMaximumToleranceMeters ||
        measurements.size() < 3U || measurements.size() > 16U) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "project standard evidence is outside the supported source, category, or sample bounds"));
    }
    std::uint64_t previous_object = 0U;
    std::set<std::uint64_t> mesh_assets;
    for (const auto& measurement : measurements) {
        if (measurement.object_id == 0U || measurement.object_id <= previous_object ||
            measurement.mesh_asset == 0U || !mesh_assets.insert(measurement.mesh_asset).second ||
            measurement.mesh_revision.value() == 0U ||
            !std::isfinite(measurement.measured_value_meters) ||
            measurement.measured_value_meters <= 0.0) {
            return core::Result<assets::Sha256Digest>::failure(invalid(
                "project standard measurement identities or values are invalid or unordered"));
        }
        previous_object = measurement.object_id;
    }
    return core::Result<assets::Sha256Digest>::success(
        digest_text(standard_evidence_text(category, *this)));
}

core::Result<void> ProjectStandardEvidence::validate(ProjectStandardCategory category) const {
    const auto expected_digest = canonical_digest(category);
    if (!expected_digest) return core::Result<void>::failure(expected_digest.error());
    const double median = measurement_median(measurements);
    if (!std::isfinite(median) || median <= 0.0 ||
        std::any_of(measurements.begin(), measurements.end(), [this, median](const auto& item) {
            return std::abs(item.measured_value_meters - median) > tolerance_meters;
        })) {
        return core::Result<void>::failure(invalid(
            "project standard evidence contains measurements outside its declared tolerance"));
    }
    if (digest.is_zero() || digest != expected_digest.value()) {
        return core::Result<void>::failure(invalid(
            "project standard evidence digest does not match its measurements"));
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectAuthoringStandard::validate() const {
    if (!safe_identifier(id) || !safe_text(label, kMaxLabelBytes) ||
        !safe_text(value, kMaxValueBytes) ||
        !safe_text(rationale, kMaxRationaleBytes, true) ||
        static_cast<unsigned>(category) >
            static_cast<unsigned>(ProjectStandardCategory::unit_convention)) {
        return core::Result<void>::failure(invalid(
            "project standard identity, text, or category is outside its bound"));
    }
    const auto expected_unit = required_unit(category);
    std::optional<double> numeric_value;
    if (numeric_category(category)) {
        numeric_value = parse_positive_scalar(value);
        if (unit != expected_unit || !numeric_value.has_value()) {
            return core::Result<void>::failure(invalid(
                "numeric project standard requires a finite positive value in its fixed unit"));
        }
    } else if (!unit.empty()) {
        return core::Result<void>::failure(invalid(
            "text project standards cannot declare a numeric unit"));
    }
    if (evidence.has_value()) {
        if (auto result = evidence->validate(category); !result) return result;
        if (!numeric_value.has_value() ||
            *numeric_value != measurement_median(evidence->measurements)) {
            return core::Result<void>::failure(invalid(
                "evidence-backed project standard value must equal the measured median"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectAuthoringStandards::set(ProjectAuthoringStandard standard) {
    if (auto result = standard.validate(); !result) return result;
    if (!records_.contains(standard.id) && records_.size() >= kMaxRecords) {
        return core::Result<void>::failure(invalid(
            "project authoring standards exceed their record limit"));
    }
    const std::string id = standard.id;
    const auto previous = records_.find(id);
    const std::optional<ProjectAuthoringStandard> previous_value = previous == records_.end()
        ? std::nullopt : std::optional<ProjectAuthoringStandard>{previous->second};
    records_.insert_or_assign(id, std::move(standard));
    if (serialize().size() > kMaxSerializedBytes) {
        if (previous_value.has_value()) records_.insert_or_assign(id, *previous_value);
        else records_.erase(id);
        return core::Result<void>::failure(invalid(
            "project authoring standards exceed their serialized byte limit"));
    }
    return core::Result<void>::success();
}

core::Result<void> ProjectAuthoringStandards::erase(std::string_view id) {
    const auto found = records_.find(std::string(id));
    if (found == records_.end()) {
        return core::Result<void>::failure({
            core::ErrorCode::not_found, "project authoring standard does not exist"});
    }
    records_.erase(found);
    return core::Result<void>::success();
}

const ProjectAuthoringStandard* ProjectAuthoringStandards::find(std::string_view id) const {
    const auto found = records_.find(std::string(id));
    return found == records_.end() ? nullptr : &found->second;
}

core::Result<void> ProjectAuthoringStandards::validate() const {
    if (records_.size() > kMaxRecords) {
        return core::Result<void>::failure(malformed(
            "project authoring standards exceed their record limit"));
    }
    for (const auto& [id, standard] : records_) {
        if (id != standard.id) {
            return core::Result<void>::failure(malformed(
                "project authoring standard map key does not match its identity"));
        }
        if (auto result = standard.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project authoring standard " + id));
        }
    }
    if (serialize().size() > kMaxSerializedBytes) {
        return core::Result<void>::failure(malformed(
            "project authoring standards exceed their serialized byte limit"));
    }
    return core::Result<void>::success();
}

std::string ProjectAuthoringStandards::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "RECORDS " << records_.size() << '\n';
    for (const auto& [id, standard] : records_) {
        output << "STANDARD " << std::quoted(id) << ' '
               << static_cast<unsigned>(standard.category) << ' '
               << std::quoted(standard.label) << ' '
               << std::quoted(standard.value) << ' '
               << std::quoted(standard.unit) << ' '
               << std::quoted(standard.rationale) << '\n';
        if (!standard.evidence.has_value()) {
            output << "EVIDENCE 0\n";
        } else {
            output << "EVIDENCE 1 " << standard.evidence->source_revision.value() << ' '
                   << canonical_real(standard.evidence->tolerance_meters) << ' '
                   << standard.evidence->digest.hex() << ' '
                   << standard.evidence->measurements.size() << '\n';
            for (const auto& measurement : standard.evidence->measurements) {
                output << "MEASUREMENT " << measurement.object_id << ' '
                       << measurement.mesh_asset << ' '
                       << measurement.mesh_revision.value() << ' '
                       << canonical_real(measurement.measured_value_meters) << '\n';
            }
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ProjectAuthoringStandards::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    if (std::none_of(records_.begin(), records_.end(), [](const auto& item) {
            return item.second.evidence.has_value();
        })) {
        // Preserve objective/checkpoint bindings created before evidence fields
        // existed. The project serializer can advance while the semantic digest
        // of unchanged, manually approved standards remains stable.
        std::ostringstream legacy;
        legacy << kMagic << " 1\n";
        legacy << "RECORDS " << records_.size() << '\n';
        for (const auto& [id, standard] : records_) {
            legacy << "STANDARD " << std::quoted(id) << ' '
                   << static_cast<unsigned>(standard.category) << ' '
                   << std::quoted(standard.label) << ' '
                   << std::quoted(standard.value) << ' '
                   << std::quoted(standard.unit) << ' '
                   << std::quoted(standard.rationale) << '\n';
        }
        legacy << "END\n";
        return core::Result<assets::Sha256Digest>::success(digest_text(legacy.str()));
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<ProjectAuthoringStandards> ProjectAuthoringStandards::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<ProjectAuthoringStandards>::failure(malformed(
            "project authoring standards are empty or exceed their byte limit"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    std::string record;
    std::size_t count = 0U;
    if (!(input >> magic >> version) || magic != kMagic || version == 0U ||
        version > kSchemaVersion ||
        !(input >> record >> count) || record != "RECORDS" || count > kMaxRecords) {
        return core::Result<ProjectAuthoringStandards>::failure(malformed(
            "project authoring standards header or record count is invalid"));
    }
    ProjectAuthoringStandards standards;
    for (std::size_t index = 0U; index < count; ++index) {
        ProjectAuthoringStandard standard;
        unsigned category = 0U;
        if (!(input >> record) || record != "STANDARD" ||
            !(input >> std::quoted(standard.id) >> category >>
                std::quoted(standard.label) >> std::quoted(standard.value) >>
                std::quoted(standard.unit) >> std::quoted(standard.rationale)) ||
            category > static_cast<unsigned>(ProjectStandardCategory::unit_convention)) {
            return core::Result<ProjectAuthoringStandards>::failure(malformed(
                "project authoring standard record is malformed"));
        }
        standard.category = static_cast<ProjectStandardCategory>(category);
        if (version >= 2U) {
            unsigned evidence_present = 0U;
            if (!(input >> record >> evidence_present) || record != "EVIDENCE" ||
                evidence_present > 1U) {
                return core::Result<ProjectAuthoringStandards>::failure(malformed(
                    "project standard evidence marker is malformed"));
            }
            if (evidence_present == 1U) {
                ProjectStandardEvidence evidence;
                std::size_t measurement_count = 0U;
                std::uint64_t source_revision = 0U;
                std::string digest_hex;
                if (!read_unsigned(input, source_revision) ||
                    !read_finite_real(input, evidence.tolerance_meters) ||
                    !(input >> digest_hex) ||
                    !read_unsigned(input, measurement_count) || measurement_count > 16U) {
                    return core::Result<ProjectAuthoringStandards>::failure(malformed(
                        "project standard evidence header is malformed"));
                }
                evidence.source_revision = core::Revision{source_revision};
                const auto digest = assets::Sha256Digest::from_hex(digest_hex);
                if (!digest) {
                    return core::Result<ProjectAuthoringStandards>::failure(malformed(
                        "project standard evidence digest is malformed"));
                }
                evidence.digest = digest.value();
                evidence.measurements.reserve(measurement_count);
                for (std::size_t sample_index = 0U;
                     sample_index < measurement_count; ++sample_index) {
                    std::uint64_t object_id = 0U;
                    std::uint64_t mesh_asset = 0U;
                    std::uint64_t mesh_revision = 0U;
                    double measured_value = 0.0;
                    if (!(input >> record) || record != "MEASUREMENT" ||
                        !read_unsigned(input, object_id) ||
                        !read_unsigned(input, mesh_asset) ||
                        !read_unsigned(input, mesh_revision) ||
                        !read_finite_real(input, measured_value)) {
                        return core::Result<ProjectAuthoringStandards>::failure(malformed(
                            "project standard measurement is malformed"));
                    }
                    evidence.measurements.push_back(ProjectStandardMeasurementEvidence{
                        object_id, mesh_asset, core::Revision{mesh_revision}, measured_value});
                }
                standard.evidence = std::move(evidence);
            }
        }
        if (standards.find(standard.id) != nullptr) {
            return core::Result<ProjectAuthoringStandards>::failure(malformed(
                "project authoring standard identity is duplicated"));
        }
        if (auto result = standards.set(std::move(standard)); !result) {
            return core::Result<ProjectAuthoringStandards>::failure(
                result.error().with_context("serialized project authoring standard"));
        }
    }
    if (!(input >> record) || record != "END") {
        return core::Result<ProjectAuthoringStandards>::failure(malformed(
            "project authoring standards are missing END"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ProjectAuthoringStandards>::failure(malformed(
            "project authoring standards contain trailing data"));
    }
    if (auto result = standards.validate(); !result) {
        return core::Result<ProjectAuthoringStandards>::failure(result.error());
    }
    return core::Result<ProjectAuthoringStandards>::success(std::move(standards));
}

} // namespace carto::project
