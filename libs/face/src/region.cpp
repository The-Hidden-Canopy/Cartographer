#include <carto/face/region.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::face {
namespace {

constexpr std::string_view kRegionMagic = "CARTO_FACE_REGION_SCHEMA";
constexpr std::string_view kGraphDigestMagic = "CARTO_FACE_CONTROL_GRAPH";

using detail::invalid;
using detail::parse_error;
using detail::safe_token;
using detail::validation;
using detail::version_error;

void write_digest(std::ostringstream& output, const assets::Sha256Digest& digest) {
    detail::write_json_string(output, digest.hex());
}

core::Result<assets::Sha256Digest> read_digest(
    std::istringstream& input,
    const char* label) {
    const auto hex = detail::read_quoted(input, label, 64U);
    if (!hex) return core::Result<assets::Sha256Digest>::failure(hex.error());
    const auto parsed = assets::Sha256Digest::from_hex(hex.value());
    if (!parsed) {
        return core::Result<assets::Sha256Digest>::failure(parse_error(
            std::string(label) + " digest is malformed"));
    }
    return parsed;
}

core::Result<void> require_sorted_unique_bindings(
    const std::vector<RegionBinding>& bindings) {
    for (std::size_t index = 1U; index < bindings.size(); ++index) {
        if (!(bindings[index - 1U] < bindings[index])) {
            return core::Result<void>::failure(validation(
                "region bindings must be sorted and unique by (region, side)"));
        }
    }
    return core::Result<void>::success();
}

// Canonical form of the control graph for digest binding. Covers every durable
// field of every node (the map is already sorted by id) and every edge sorted
// by (driver, follower); equal graphs digest equally.
std::string control_graph_canonical(const character::ControlGraph& controls) {
    std::ostringstream output;
    output << kGraphDigestMagic << " 1\n";
    for (const auto& [id, definition] : controls.nodes()) {
        output << "NODE " << id.value << ' '
               << character::control_shape_name(definition.shape) << ' ';
        detail::write_json_string(output, definition.control_set);
        output << ' ' << static_cast<unsigned>(definition.allowed_axes);
        const core::Transform& transform = definition.transform;
        output << std::setprecision(17);
        output << ' ' << transform.translation.x << ' ' << transform.translation.y
               << ' ' << transform.translation.z;
        output << ' ' << transform.rotation.x << ' ' << transform.rotation.y
               << ' ' << transform.rotation.z << ' ' << transform.rotation.w;
        output << ' ' << transform.scale.x << ' ' << transform.scale.y
               << ' ' << transform.scale.z;
        output << " JOINTS " << definition.affected_joints.size();
        for (const kinematic::JointId joint : definition.affected_joints) {
            output << ' ' << joint.value;
        }
        output << " LIMITS";
        for (std::size_t axis = 0U; axis < character::kAxisCount; ++axis) {
            output << ' ' << definition.axis_minimum[axis]
                   << ' ' << definition.axis_maximum[axis];
        }
        output << " FOLLOW ";
        if (definition.follow_space.has_value()) {
            output << definition.follow_space->value;
        } else {
            output << 0U;
        }
        output << '\n';
    }
    std::vector<character::ControlEdge> edges = controls.edges();
    std::sort(edges.begin(), edges.end());
    output << "EDGES " << edges.size() << '\n';
    for (const character::ControlEdge& edge : edges) {
        output << "EDGE " << edge.driver.value << ' ' << edge.follower.value << '\n';
    }
    output << "END\n";
    return output.str();
}

} // namespace

std::string_view facial_region_name(FacialRegion region) noexcept {
    switch (region) {
    case FacialRegion::brow: return "brow";
    case FacialRegion::lid: return "lid";
    case FacialRegion::cheek: return "cheek";
    case FacialRegion::jaw: return "jaw";
    case FacialRegion::lip: return "lip";
    case FacialRegion::tongue: return "tongue";
    }
    return "brow";
}

std::string_view facial_side_name(FacialSide side) noexcept {
    switch (side) {
    case FacialSide::left: return "left";
    case FacialSide::right: return "right";
    case FacialSide::bilateral: return "bilateral";
    }
    return "bilateral";
}

core::Result<FacialRegion> facial_region_from_name(std::string_view text) {
    for (const FacialRegion region : {
             FacialRegion::brow, FacialRegion::lid, FacialRegion::cheek,
             FacialRegion::jaw, FacialRegion::lip, FacialRegion::tongue}) {
        if (facial_region_name(region) == text) {
            return core::Result<FacialRegion>::success(region);
        }
    }
    return core::Result<FacialRegion>::failure(
        invalid("unknown facial region name"));
}

core::Result<FacialSide> facial_side_from_name(std::string_view text) {
    for (const FacialSide side :
         {FacialSide::left, FacialSide::right, FacialSide::bilateral}) {
        if (facial_side_name(side) == text) {
            return core::Result<FacialSide>::success(side);
        }
    }
    return core::Result<FacialSide>::failure(
        invalid("unknown facial side name"));
}

core::Result<void> RegionBinding::validate() const {
    if (!safe_token(control_set, kMaxTokenBytes)) {
        return core::Result<void>::failure(invalid(
            "region binding control set is not a bounded safe token"));
    }
    if (!safe_token(morph_group, kMaxTokenBytes)) {
        return core::Result<void>::failure(invalid(
            "region binding morph group is not a bounded safe token"));
    }
    return core::Result<void>::success();
}

core::Result<void> FacialRegionSchema::insert(RegionBinding binding) {
    if (auto result = binding.validate(); !result) return result;
    if (bindings.size() >= kMaxBindings) {
        return core::Result<void>::failure(invalid(
            "facial region schema exceeds its binding bound"));
    }
    const auto position = std::lower_bound(bindings.begin(), bindings.end(), binding);
    if (position != bindings.end() && *position == binding) {
        return core::Result<void>::failure(invalid(
            "facial region binding already exists for this region and side"));
    }
    bindings.insert(position, std::move(binding));
    return core::Result<void>::success();
}

core::Result<void> FacialRegionSchema::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(version_error(
            "facial region schema version is not supported"));
    }
    if (!character_id) {
        return core::Result<void>::failure(invalid(
            "facial region schema must name a character identity"));
    }
    if (control_graph_digest.is_zero() || morph_library_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "facial region schema digests must be nonzero"));
    }
    if (bindings.size() > kMaxBindings) {
        return core::Result<void>::failure(validation(
            "facial region schema exceeds its binding bound"));
    }
    for (const RegionBinding& binding : bindings) {
        if (auto result = binding.validate(); !result) return result;
    }
    return require_sorted_unique_bindings(bindings);
}

std::string FacialRegionSchema::serialize() const {
    std::ostringstream output;
    output << kRegionMagic << ' ' << kSchemaVersion << '\n';
    output << "CHARACTER " << character_id.value << '\n';
    output << "CONTROL_GRAPH_SHA256 ";
    write_digest(output, control_graph_digest);
    output << '\n';
    output << "MORPH_LIBRARY_SHA256 ";
    write_digest(output, morph_library_digest);
    output << '\n';
    output << "BINDINGS " << bindings.size() << '\n';
    for (const RegionBinding& binding : bindings) {
        output << "BINDING " << facial_region_name(binding.region) << ' '
               << facial_side_name(binding.side) << ' ';
        detail::write_json_string(output, binding.control_set);
        output << ' ';
        detail::write_json_string(output, binding.morph_group);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> FacialRegionSchema::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<FacialRegionSchema> FacialRegionSchema::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<FacialRegionSchema>::failure(parse_error(
            "facial region schema text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kRegionMagic, kSchemaVersion); !result) {
        return core::Result<FacialRegionSchema>::failure(result.error());
    }
    FacialRegionSchema schema;
    const auto character_tag = detail::read_token(input, "CHARACTER");
    if (!character_tag) {
        return core::Result<FacialRegionSchema>::failure(character_tag.error());
    }
    if (character_tag.value() != "CHARACTER") {
        return core::Result<FacialRegionSchema>::failure(parse_error(
            "facial region schema framing is invalid: CHARACTER"));
    }
    const auto character = detail::read_token(input, "CHARACTER identity");
    if (!character) return core::Result<FacialRegionSchema>::failure(character.error());
    std::uint64_t character_value = 0U;
    {
        const auto parsed = std::from_chars(
            character.value().data(),
            character.value().data() + character.value().size(),
            character_value);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != character.value().data() + character.value().size()) {
            return core::Result<FacialRegionSchema>::failure(parse_error(
                "facial region schema character identity is invalid"));
        }
    }
    schema.character_id = character::CharacterId{character_value};
    const auto header = detail::read_token(input, "CONTROL_GRAPH_SHA256");
    if (!header) return core::Result<FacialRegionSchema>::failure(header.error());
    if (header.value() != "CONTROL_GRAPH_SHA256") {
        return core::Result<FacialRegionSchema>::failure(parse_error(
            "facial region schema framing is invalid: CONTROL_GRAPH_SHA256"));
    }
    const auto control_digest = read_digest(input, "CONTROL_GRAPH_SHA256");
    if (!control_digest) {
        return core::Result<FacialRegionSchema>::failure(control_digest.error());
    }
    schema.control_graph_digest = control_digest.value();
    const auto morph_header = detail::read_token(input, "MORPH_LIBRARY_SHA256");
    if (!morph_header) {
        return core::Result<FacialRegionSchema>::failure(morph_header.error());
    }
    if (morph_header.value() != "MORPH_LIBRARY_SHA256") {
        return core::Result<FacialRegionSchema>::failure(parse_error(
            "facial region schema framing is invalid: MORPH_LIBRARY_SHA256"));
    }
    const auto morph_digest = read_digest(input, "MORPH_LIBRARY_SHA256");
    if (!morph_digest) {
        return core::Result<FacialRegionSchema>::failure(morph_digest.error());
    }
    schema.morph_library_digest = morph_digest.value();
    const auto count_tag = detail::read_token(input, "BINDINGS");
    if (!count_tag) {
        return core::Result<FacialRegionSchema>::failure(count_tag.error());
    }
    if (count_tag.value() != "BINDINGS") {
        return core::Result<FacialRegionSchema>::failure(parse_error(
            "facial region schema framing is invalid: BINDINGS"));
    }
    const auto count_token = detail::read_token(input, "BINDINGS count");
    if (!count_token) {
        return core::Result<FacialRegionSchema>::failure(count_token.error());
    }
    std::uint64_t binding_count = 0U;
    {
        const auto parsed = std::from_chars(
            count_token.value().data(),
            count_token.value().data() + count_token.value().size(),
            binding_count);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != count_token.value().data() + count_token.value().size() ||
            binding_count > kMaxBindings) {
            return core::Result<FacialRegionSchema>::failure(parse_error(
                "facial region schema binding count is invalid"));
        }
    }
    for (std::uint64_t index = 0U; index < binding_count; ++index) {
        const auto tag = detail::read_token(input, "BINDING");
        if (!tag) return core::Result<FacialRegionSchema>::failure(tag.error());
        if (tag.value() != "BINDING") {
            return core::Result<FacialRegionSchema>::failure(parse_error(
                "facial region schema binding framing is invalid"));
        }
        const auto region = detail::read_token(input, "region");
        if (!region) return core::Result<FacialRegionSchema>::failure(region.error());
        const auto side = detail::read_token(input, "side");
        if (!side) return core::Result<FacialRegionSchema>::failure(side.error());
        const auto region_value = facial_region_from_name(region.value());
        if (!region_value) {
            return core::Result<FacialRegionSchema>::failure(parse_error(
                "facial region schema binding region is unknown"));
        }
        const auto side_value = facial_side_from_name(side.value());
        if (!side_value) {
            return core::Result<FacialRegionSchema>::failure(parse_error(
                "facial region schema binding side is unknown"));
        }
        const auto control_set = detail::read_quoted(
            input, "control_set", RegionBinding::kMaxTokenBytes);
        if (!control_set) {
            return core::Result<FacialRegionSchema>::failure(control_set.error());
        }
        const auto morph_group = detail::read_quoted(
            input, "morph_group", RegionBinding::kMaxTokenBytes);
        if (!morph_group) {
            return core::Result<FacialRegionSchema>::failure(morph_group.error());
        }
        schema.bindings.push_back(RegionBinding{
            region_value.value(), side_value.value(),
            control_set.value(), morph_group.value()});
    }
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<FacialRegionSchema>::failure(result.error());
    }
    if (auto result = schema.validate(); !result) {
        return core::Result<FacialRegionSchema>::failure(result.error());
    }
    return core::Result<FacialRegionSchema>::success(std::move(schema));
}

core::Result<assets::Sha256Digest> morph_library_digest(
    std::span<const character::MorphTargetId> targets) {
    std::vector<character::MorphTargetId> sorted(targets.begin(), targets.end());
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "morph target population must not contain duplicate identities"));
    }
    std::ostringstream output;
    output << "CARTO_FACE_MORPH_LIBRARY 1\n";
    output << "TARGETS " << sorted.size() << '\n';
    for (const character::MorphTargetId target : sorted) {
        if (!target) {
            return core::Result<assets::Sha256Digest>::failure(invalid(
                "morph target population must not contain the invalid identity"));
        }
        output << "TARGET " << target.value << '\n';
    }
    output << "END\n";
    const std::string canonical = output.str();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<assets::Sha256Digest> control_graph_digest(
    const character::ControlGraph& controls) {
    if (auto result = controls.validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = control_graph_canonical(controls);
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<void> validate_region_bindings(
    const FacialRegionSchema& schema,
    const character::ControlGraph& controls,
    std::span<const character::MorphTargetId> morph_targets) {
    if (auto result = schema.validate(); !result) return result;
    std::set<std::string_view> control_sets;
    for (const auto& [id, definition] : controls.nodes()) {
        (void)id;
        if (definition.control_set.empty()) continue;
        control_sets.insert(definition.control_set);
    }
    for (const RegionBinding& binding : schema.bindings) {
        if (control_sets.find(binding.control_set) == control_sets.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "facial region binding names a control set the character does not have: " +
                    binding.control_set));
        }
    }
    const auto graph_digest = control_graph_digest(controls);
    if (!graph_digest) return core::Result<void>::failure(graph_digest.error());
    if (graph_digest.value() != schema.control_graph_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "facial region schema control graph digest is stale"));
    }
    const auto library_digest = morph_library_digest(morph_targets);
    if (!library_digest) return core::Result<void>::failure(library_digest.error());
    if (library_digest.value() != schema.morph_library_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "facial region schema morph library digest is stale"));
    }
    return core::Result<void>::success();
}

} // namespace carto::face
