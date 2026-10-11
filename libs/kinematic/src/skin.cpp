#include <carto/kinematic/skin.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace carto::kinematic {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_SKIN_BINDING";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(
        input >> transform.translation.x >> transform.translation.y >>
        transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
        transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
        transform.scale.y >> transform.scale.z);
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

} // namespace

core::Result<void> SkinBinding::validate() const {
    if (mesh_asset == 0U) {
        return core::Result<void>::failure(invalid(
            "skin binding mesh identity is invalid"));
    }
    if (mesh_digest.is_zero() || graph_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "skin binding must be content-addressed against mesh and graph"));
    }
    if (!mesh_bind_transform.finite()) {
        return core::Result<void>::failure(invalid(
            "skin binding bind transform is not finite"));
    }
    if (influences.size() > kMaxInfluencedVertices) {
        return core::Result<void>::failure(validation(
            "skin binding exceeds its bounded vertex capacity"));
    }
    for (const auto& [vertex, weights] : influences) {
        if (weights.empty() || weights.size() > kMaxInfluencesPerVertex) {
            return core::Result<void>::failure(validation(
                "skin binding influence count per vertex is out of bounds"));
        }
        double sum = 0.0;
        for (std::size_t index = 0U; index < weights.size(); ++index) {
            const JointInfluence& influence = weights[index];
            if (!influence.joint || !std::isfinite(influence.weight) ||
                influence.weight <= 0.0) {
                return core::Result<void>::failure(invalid(
                    "skin binding influence joint or weight is invalid"));
            }
            if (index > 0U && !(weights[index - 1U].joint < influence.joint)) {
                return core::Result<void>::failure(validation(
                    "skin binding influences must be sorted by unique joint id"));
            }
            sum += influence.weight;
        }
        if (!std::isfinite(sum) ||
            std::abs(sum - 1.0) > kWeightSumTolerance) {
            return core::Result<void>::failure(validation(
                "skin binding influence weights must be normalized per vertex"));
        }
    }
    return core::Result<void>::success();
}

std::string SkinBinding::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "MESH " << mesh_asset << ' ' << mesh_digest.hex() << '\n';
    output << "GRAPH " << graph_digest.hex() << '\n';
    output << "BIND_TRANSFORM ";
    write_transform(output, mesh_bind_transform);
    output << '\n';
    output << "INFLUENCES " << influences.size() << '\n';
    for (const auto& [vertex, weights] : influences) {
        output << "INFLUENCE " << vertex << ' ' << weights.size();
        for (const JointInfluence& influence : weights) {
            output << ' ' << influence.joint.value << ' ' << influence.weight;
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SkinBinding::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<SkinBinding> SkinBinding::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<SkinBinding>::failure(version_error(
            "skin binding schema version is not supported"));
    }

    SkinBinding binding;
    if (!require_token(input, "MESH") ||
        !read_integer(input, binding.mesh_asset) ||
        !read_digest(input, binding.mesh_digest) ||
        !require_token(input, "GRAPH") ||
        !read_digest(input, binding.graph_digest) ||
        !require_token(input, "BIND_TRANSFORM") ||
        !read_transform(input, binding.mesh_bind_transform)) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding header is invalid"));
    }

    std::size_t influence_count = 0U;
    if (!require_token(input, "INFLUENCES") ||
        !read_count(input, kMaxInfluencedVertices, influence_count)) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding influence header is invalid"));
    }
    for (std::size_t index = 0U; index < influence_count; ++index) {
        std::uint32_t vertex = 0U;
        std::size_t weight_count = 0U;
        if (!require_token(input, "INFLUENCE") ||
            !read_integer(input, vertex) ||
            vertex > kMaxVertexIndex ||
            !read_count(input, kMaxInfluencesPerVertex, weight_count)) {
            return core::Result<SkinBinding>::failure(parse_error(
                "skin binding influence record is malformed"));
        }
        std::vector<JointInfluence> weights;
        weights.reserve(weight_count);
        for (std::size_t weight_index = 0U; weight_index < weight_count;
             ++weight_index) {
            JointInfluence influence;
            if (!read_integer(input, influence.joint.value) ||
                !(input >> influence.weight)) {
                return core::Result<SkinBinding>::failure(parse_error(
                    "skin binding influence weight is malformed"));
            }
            weights.push_back(influence);
        }
        if (!binding.influences.emplace(vertex, std::move(weights)).second) {
            return core::Result<SkinBinding>::failure(parse_error(
                "skin binding influence vertex is duplicated"));
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<SkinBinding>::failure(parse_error(
            "skin binding contains trailing unbound data"));
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<SkinBinding>::failure(result.error());
    }
    return core::Result<SkinBinding>::success(std::move(binding));
}

} // namespace carto::kinematic
