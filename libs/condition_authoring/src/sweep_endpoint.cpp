#include <carto/condition/sweep_endpoint.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <span>
#include <string_view>
#include <utility>

namespace carto::condition {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_identity(std::string_view value) noexcept {
    if (value.empty() || value.size() > SweepEndpointPortBinding::kMaxIdentityBytes) {
        return false;
    }
    for (const char character : value) {
        const bool letter = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    return value.front() != '.' && value.front() != '_' && value.front() != '-';
}

bool valid_side(SweepEndpointSide side) noexcept {
    return side == SweepEndpointSide::start || side == SweepEndpointSide::end;
}

bool same_endpoint(
    const SweepEndpointPortBinding& first,
    const SweepEndpointPortBinding& second) noexcept {
    return first.sweep_source_identity == second.sweep_source_identity &&
        first.sweep_source_revision == second.sweep_source_revision &&
        first.side == second.side;
}

assets::Sha256Digest source_digest(const geometry::SweepSource& source) {
    const std::string encoded = source.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()});
}

assets::Sha256Digest centerline_digest(const geometry::CurvePath& centerline) {
    const std::string encoded = centerline.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()});
}

bool near(double left, double right) noexcept {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= 1.0e-10 * scale;
}

core::Vec3d transform_point(const core::Transform& transform, core::Vec3d point) noexcept {
    return transform.translation + transform.rotation.rotate(
        core::componentwise_multiply(transform.scale, point));
}

core::Vec3d transform_direction(
    const core::Transform& transform, core::Vec3d direction) noexcept {
    return transform.rotation.rotate(
        core::componentwise_multiply(transform.scale, direction)).normalized();
}

struct ResolvedPortFrame final {
    core::Transform local_to_owner = core::Transform::identity();
    std::vector<std::uint64_t> port_ids;
    std::vector<assets::Sha256Digest> port_digests;
};

core::Result<ResolvedPortFrame> resolve_port_frame(
    const connection::ConnectionPortGraph& graph,
    const connection::ConnectionPort& port,
    std::vector<connection::PortId>& stack) {
    if (stack.size() >= SweepEndpointSpatialReview::kMaxFrameDepth) {
        return core::Result<ResolvedPortFrame>::failure(validation(
            "sweep endpoint port-frame chain exceeds its depth bound"));
    }
    if (std::find(stack.begin(), stack.end(), port.id) != stack.end()) {
        return core::Result<ResolvedPortFrame>::failure(validation(
            "sweep endpoint port-frame chain contains a cycle"));
    }
    if (auto valid = port.validate(); !valid) {
        return core::Result<ResolvedPortFrame>::failure(
            valid.error().with_context("sweep endpoint port frame"));
    }
    if (port.authoring.kind != world::AuthoringBindingKind::scene_object ||
        port.authoring.identity != port.owner.value) {
        return core::Result<ResolvedPortFrame>::failure(validation(
            "sweep endpoint port frame does not bind its exact owner object"));
    }

    stack.push_back(port.id);
    ResolvedPortFrame result;
    result.local_to_owner = port.local_frame.transform_to_parent;
    result.port_ids.push_back(port.id.value);
    const auto digest = port.canonical_digest();
    if (!digest) {
        stack.pop_back();
        return core::Result<ResolvedPortFrame>::failure(
            digest.error().with_context("sweep endpoint port digest"));
    }
    result.port_digests.push_back(digest.value());

    if (port.local_frame.parent_frame.has_value()) {
        const connection::ConnectionPort* parent = nullptr;
        for (const auto& [candidate_id, candidate] : graph.ports()) {
            static_cast<void>(candidate_id);
            if (candidate.owner != port.owner ||
                candidate.local_frame.frame_identity != *port.local_frame.parent_frame) {
                continue;
            }
            if (parent != nullptr) {
                stack.pop_back();
                return core::Result<ResolvedPortFrame>::failure(validation(
                    "sweep endpoint parent frame identity is ambiguous for this owner"));
            }
            parent = &candidate;
        }
        if (parent == nullptr) {
            stack.pop_back();
            return core::Result<ResolvedPortFrame>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "sweep endpoint parent frame is absent from the current port graph"));
        }
        if (parent->local_frame.coordinate_convention !=
            port.local_frame.coordinate_convention) {
            stack.pop_back();
            return core::Result<ResolvedPortFrame>::failure(validation(
                "sweep endpoint parent chain mixes coordinate conventions"));
        }
        auto parent_frame = resolve_port_frame(graph, *parent, stack);
        if (!parent_frame) {
            stack.pop_back();
            return parent_frame;
        }
        result.local_to_owner = parent_frame.value().local_to_owner.combine(
            result.local_to_owner);
        result.port_ids.insert(result.port_ids.end(),
            parent_frame.value().port_ids.begin(), parent_frame.value().port_ids.end());
        result.port_digests.insert(result.port_digests.end(),
            parent_frame.value().port_digests.begin(), parent_frame.value().port_digests.end());
    }
    stack.pop_back();
    if (!result.local_to_owner.finite() ||
        result.port_ids.size() > SweepEndpointSpatialReview::kMaxFrameDepth) {
        return core::Result<ResolvedPortFrame>::failure(validation(
            "sweep endpoint resolved frame is non-finite or exceeds its chain bound"));
    }
    return core::Result<ResolvedPortFrame>::success(std::move(result));
}

core::Result<void> validate_binding_record(const SweepEndpointPortBinding& binding) {
    if (!valid_identity(binding.sweep_source_identity) ||
        binding.sweep_source_revision.value() == 0U || !valid_side(binding.side) ||
        binding.port_id.value == 0U) {
        return core::Result<void>::failure(invalid(
            "sweep endpoint binding identity, revision, side, or port is invalid"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> SweepEndpointPortBinding::validate(
    const geometry::SweepSource& source,
    const connection::ConnectionPort& port) const {
    if (auto result = source.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("sweep endpoint source"));
    }
    if (auto result = validate_binding_record(*this); !result) return result;
    if (sweep_source_identity != source.source_identity ||
        sweep_source_revision != source.source_revision) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "sweep endpoint binding does not target the supplied source revision"));
    }
    if (port.id != port_id) {
        return core::Result<void>::failure(core::Diagnostic(core::ErrorCode::stale_data,
            "sweep endpoint binding does not target the supplied connection port"));
    }
    if (auto result = port.validate(); !result) return result;
    if (source.owner_object_id != 0U &&
        port.owner.value != source.owner_object_id) {
        return core::Result<void>::failure(validation(
            "owner-bound sweep endpoint port must belong to the sweep source owner"));
    }
    return core::Result<void>::success();
}

core::Result<void> SweepEndpointCompatibilityEvidence::validate() const {
    if (auto result = validate_binding_record(first); !result) return result;
    if (auto result = validate_binding_record(second); !result) return result;
    if (same_endpoint(first, second) || first.port_id == second.port_id) {
        return core::Result<void>::failure(validation(
            "sweep endpoint compatibility evidence repeats one endpoint or port"));
    }
    if (first.sweep_source_identity == second.sweep_source_identity &&
        (first.sweep_source_revision != second.sweep_source_revision ||
         first_sweep_digest != second_sweep_digest)) {
        return core::Result<void>::failure(validation(
            "one sweep identity cannot bind conflicting revisions or source digests"));
    }
    if (first_sweep_digest.is_zero() || second_sweep_digest.is_zero() ||
        first_port_digest.is_zero() || second_port_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "sweep endpoint evidence requires both source and port digests"));
    }
    return port_compatibility.validate();
}

core::Result<void> SweepEndpointSpatialPolicy::validate() const {
    const double axis_length = port_outward_axis_local.length();
    if (!port_outward_axis_local.finite() || !std::isfinite(axis_length) ||
        std::abs(axis_length - 1.0) > 1.0e-9 ||
        !std::isfinite(maximum_position_error) || maximum_position_error < 0.0 ||
        maximum_position_error > 1.0e6 ||
        !std::isfinite(minimum_axis_alignment) || minimum_axis_alignment < 0.0 ||
        minimum_axis_alignment > 1.0) {
        return core::Result<void>::failure(validation(
            "sweep endpoint spatial policy requires a unit port axis and bounded tolerances"));
    }
    return core::Result<void>::success();
}

core::Result<void> SweepEndpointSpatialReview::validate() const {
    if (auto valid = validate_binding_record(binding); !valid) return valid;
    if (sweep_source_digest.is_zero() || centerline_digest.is_zero() ||
        port_digest.is_zero() || frame_chain_port_ids.empty() ||
        frame_chain_port_ids.size() > kMaxFrameDepth ||
        frame_chain_port_ids.size() != frame_chain_port_digests.size() ||
        frame_chain_port_ids.front() != binding.port_id.value ||
        frame_chain_port_digests.front() != port_digest ||
        std::any_of(frame_chain_port_ids.begin(), frame_chain_port_ids.end(),
            [](std::uint64_t value) { return value == 0U; }) ||
        std::any_of(frame_chain_port_digests.begin(), frame_chain_port_digests.end(),
            [](const assets::Sha256Digest& value) { return value.is_zero(); })) {
        return core::Result<void>::failure(validation(
            "sweep endpoint spatial review has incomplete source or frame-chain evidence"));
    }
    for (std::size_t index = 0U; index < frame_chain_port_ids.size(); ++index) {
        if (std::find(frame_chain_port_ids.begin(),
                frame_chain_port_ids.begin() + static_cast<std::ptrdiff_t>(index),
                frame_chain_port_ids[index]) !=
            frame_chain_port_ids.begin() + static_cast<std::ptrdiff_t>(index)) {
            return core::Result<void>::failure(validation(
                "sweep endpoint spatial frame chain repeats a port"));
        }
    }
    if (auto valid = policy.validate(); !valid) return valid;

    const double sweep_axis_length = sweep_outward_axis_world.length();
    const double port_axis_length = port_outward_axis_world.length();
    const double measured_position_error =
        (sweep_endpoint_world - port_origin_world).length();
    const double measured_axis_alignment =
        core::dot(sweep_outward_axis_world, port_outward_axis_world);
    if (!sweep_endpoint_world.finite() || !port_origin_world.finite() ||
        !sweep_outward_axis_world.finite() || !port_outward_axis_world.finite() ||
        !std::isfinite(sweep_axis_length) || !near(sweep_axis_length, 1.0) ||
        !std::isfinite(port_axis_length) || !near(port_axis_length, 1.0) ||
        !std::isfinite(position_error) || position_error < 0.0 ||
        !std::isfinite(axis_alignment) || axis_alignment < -1.0 || axis_alignment > 1.0 ||
        !near(position_error, measured_position_error) ||
        !near(axis_alignment, measured_axis_alignment)) {
        return core::Result<void>::failure(validation(
            "sweep endpoint spatial measurements are non-finite or internally inconsistent"));
    }
    const bool expected_position = position_error <= policy.maximum_position_error;
    const bool expected_axis = axis_alignment >= policy.minimum_axis_alignment;
    if (position_within_tolerance != expected_position ||
        axis_within_tolerance != expected_axis || aligned != (expected_position && expected_axis)) {
        return core::Result<void>::failure(validation(
            "sweep endpoint spatial outcome does not match its measured tolerances"));
    }
    return core::Result<void>::success();
}

std::string SweepEndpointSpatialReview::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "CARTOGRAPHER_SWEEP_ENDPOINT_SPATIAL_REVIEW " << kSchemaVersion << '\n';
    output << "BINDING " << std::quoted(binding.sweep_source_identity) << ' '
           << binding.sweep_source_revision.value() << ' '
           << static_cast<unsigned>(binding.side) << ' ' << binding.port_id.value << '\n';
    output << "PROJECT " << project_generation << ' ' << project_revision.value() << ' '
           << scene_revision.value() << '\n';
    output << "SOURCE_DIGESTS " << sweep_source_digest.hex() << ' '
           << centerline_digest.hex() << ' ' << port_digest.hex() << '\n';
    output << "FRAME_CHAIN " << frame_chain_port_ids.size() << '\n';
    for (std::size_t index = 0U; index < frame_chain_port_ids.size(); ++index) {
        output << "FRAME " << frame_chain_port_ids[index] << ' '
               << frame_chain_port_digests[index].hex() << '\n';
    }
    output << "POLICY " << policy.port_outward_axis_local.x << ' '
           << policy.port_outward_axis_local.y << ' '
           << policy.port_outward_axis_local.z << ' '
           << policy.maximum_position_error << ' '
           << policy.minimum_axis_alignment << '\n';
    output << "MEASUREMENTS "
           << sweep_endpoint_world.x << ' ' << sweep_endpoint_world.y << ' '
           << sweep_endpoint_world.z << ' '
           << sweep_outward_axis_world.x << ' ' << sweep_outward_axis_world.y << ' '
           << sweep_outward_axis_world.z << ' '
           << port_origin_world.x << ' ' << port_origin_world.y << ' '
           << port_origin_world.z << ' '
           << port_outward_axis_world.x << ' ' << port_outward_axis_world.y << ' '
           << port_outward_axis_world.z << ' ' << position_error << ' '
           << axis_alignment << ' ' << position_within_tolerance << ' '
           << axis_within_tolerance << ' ' << aligned << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> SweepEndpointSpatialReview::canonical_digest() const {
    if (auto valid = validate(); !valid) {
        return core::Result<assets::Sha256Digest>::failure(valid.error());
    }
    const std::string encoded = serialize();
    if (encoded.size() > kMaxSerializedBytes) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "sweep endpoint spatial review exceeds its serialized byte bound"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()}));
}

std::string SweepEndpointCompatibilityEvidence::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTOGRAPHER_SWEEP_ENDPOINT_COMPATIBILITY " << kSchemaVersion << '\n';
    output << "FIRST " << std::quoted(first.sweep_source_identity) << ' '
           << first.sweep_source_revision.value() << ' '
           << static_cast<unsigned>(first.side) << ' ' << first.port_id.value << ' '
           << first_sweep_digest.hex() << ' ' << first_port_digest.hex() << '\n';
    output << "SECOND " << std::quoted(second.sweep_source_identity) << ' '
           << second.sweep_source_revision.value() << ' '
           << static_cast<unsigned>(second.side) << ' ' << second.port_id.value << ' '
           << second_sweep_digest.hex() << ' ' << second_port_digest.hex() << '\n';
    output << "PORT_COMPATIBILITY "
           << std::quoted(port_compatibility.serialize()) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest>
SweepEndpointCompatibilityEvidence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string encoded = serialize();
    if (encoded.size() > kMaxSerializedBytes) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "sweep endpoint evidence exceeds its serialized byte bound"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return core::Result<assets::Sha256Digest>::success(
        assets::sha256(std::span<const std::uint8_t>{bytes, encoded.size()}));
}

core::Result<SweepEndpointCompatibilityEvidence> check_sweep_endpoint_compatibility(
    const geometry::SweepSource& first_source,
    const SweepEndpointPortBinding& first_binding,
    const connection::ConnectionPort& first_port,
    const geometry::SweepSource& second_source,
    const SweepEndpointPortBinding& second_binding,
    const connection::ConnectionPort& second_port) {
    if (auto result = first_binding.validate(first_source, first_port); !result) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            result.error().with_context("first sweep endpoint"));
    }
    if (auto result = second_binding.validate(second_source, second_port); !result) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            result.error().with_context("second sweep endpoint"));
    }
    if (same_endpoint(first_binding, second_binding) ||
        first_binding.port_id == second_binding.port_id) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(validation(
            "sweep endpoint join requires two distinct endpoint and port identities"));
    }
    const auto first_source_digest = source_digest(first_source);
    const auto second_source_digest = source_digest(second_source);
    if (first_source.source_identity == second_source.source_identity &&
        (first_source.source_revision != second_source.source_revision ||
         first_source_digest != second_source_digest)) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "one sweep identity was supplied with conflicting revision-bound recipes"));
    }
    const auto first_port_digest = first_port.canonical_digest();
    if (!first_port_digest) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            first_port_digest.error().with_context("first sweep endpoint port digest"));
    }
    const auto second_port_digest = second_port.canonical_digest();
    if (!second_port_digest) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            second_port_digest.error().with_context("second sweep endpoint port digest"));
    }
    const auto compatible = connection::check_port_compatibility(first_port, second_port);
    if (!compatible) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            compatible.error().with_context("sweep endpoint compatibility"));
    }

    SweepEndpointCompatibilityEvidence evidence{
        first_binding,
        second_binding,
        first_source_digest,
        second_source_digest,
        first_port_digest.value(),
        second_port_digest.value(),
        compatible.value()};
    if (auto result = evidence.validate(); !result) {
        return core::Result<SweepEndpointCompatibilityEvidence>::failure(
            result.error().with_context("sweep endpoint compatibility evidence"));
    }
    return core::Result<SweepEndpointCompatibilityEvidence>::success(std::move(evidence));
}

core::Result<SweepEndpointSpatialReview> review_sweep_endpoint_spatial_fit(
    const geometry::SweepSource& source,
    const geometry::CurvePath& centerline,
    const SweepEndpointPortBinding& binding,
    const connection::ConnectionPortGraph& ports,
    const scene::Scene& scene,
    const SweepEndpointSpatialPolicy& policy,
    std::uint64_t project_generation,
    core::Revision project_revision) {
    if (auto valid = source.validate(); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            valid.error().with_context("sweep spatial review source"));
    }
    if (auto valid = centerline.validate(); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            valid.error().with_context("sweep spatial review centerline"));
    }
    if (source.centerline_identity != centerline.source_identity ||
        source.centerline_revision != centerline.source_revision) {
        return core::Result<SweepEndpointSpatialReview>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep spatial review requires the exact source-bound centerline revision"));
    }
    if (auto valid = policy.validate(); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(valid.error());
    }
    if (auto valid = ports.validate(); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            valid.error().with_context("sweep spatial review connection graph"));
    }
    if (auto valid = scene.validate(); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            valid.error().with_context("sweep spatial review scene"));
    }
    const auto* port = ports.find_port(binding.port_id);
    if (port == nullptr) {
        return core::Result<SweepEndpointSpatialReview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "sweep spatial review port is absent from the current project graph"));
    }
    if (auto valid = binding.validate(source, *port); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            valid.error().with_context("sweep spatial review binding"));
    }
    if (scene.find(port->owner) == nullptr) {
        return core::Result<SweepEndpointSpatialReview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "sweep spatial review port owner is absent from the current scene"));
    }

    const core::Revision captured_scene_revision = scene.revision();
    auto samples = centerline.sample_uniform(source.samples_per_segment);
    if (!samples) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            samples.error().with_context("sweep spatial review endpoint sampling"));
    }
    if (samples.value().size() < 2U) {
        return core::Result<SweepEndpointSpatialReview>::failure(validation(
            "sweep spatial review requires at least two endpoint samples"));
    }
    const auto& endpoint = binding.side == SweepEndpointSide::start
        ? samples.value().front() : samples.value().back();
    const double outward_sign = binding.side == SweepEndpointSide::start ? -1.0 : 1.0;

    core::Transform sweep_to_world = core::Transform::identity();
    if (source.owner_object_id != 0U) {
        auto owner_transform = scene.world_transform(scene::ObjectId{source.owner_object_id});
        if (!owner_transform) {
            return core::Result<SweepEndpointSpatialReview>::failure(
                owner_transform.error().with_context("sweep source owner transform"));
        }
        sweep_to_world = owner_transform.value();
    }
    const auto sweep_axis = transform_direction(
        sweep_to_world, endpoint.tangent.normalized() * outward_sign);
    if (!sweep_axis.finite()) {
        return core::Result<SweepEndpointSpatialReview>::failure(validation(
            "sweep spatial review could not transform its endpoint tangent"));
    }

    std::vector<connection::PortId> frame_stack;
    auto frame = resolve_port_frame(ports, *port, frame_stack);
    if (!frame) return core::Result<SweepEndpointSpatialReview>::failure(frame.error());
    auto port_owner_world = scene.world_transform(port->owner);
    if (!port_owner_world) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            port_owner_world.error().with_context("sweep endpoint port owner transform"));
    }
    const auto port_to_world = port_owner_world.value().combine(frame.value().local_to_owner);
    const auto port_axis = transform_direction(port_to_world, policy.port_outward_axis_local);
    if (!port_to_world.finite() || !port_axis.finite()) {
        return core::Result<SweepEndpointSpatialReview>::failure(validation(
            "sweep endpoint port frame resolves to a non-finite world transform"));
    }

    const auto port_digest = port->canonical_digest();
    if (!port_digest) {
        return core::Result<SweepEndpointSpatialReview>::failure(port_digest.error());
    }
    SweepEndpointSpatialReview review;
    review.binding = binding;
    review.project_generation = project_generation;
    review.project_revision = project_revision;
    review.scene_revision = captured_scene_revision;
    review.sweep_source_digest = source_digest(source);
    review.centerline_digest = centerline_digest(centerline);
    review.port_digest = port_digest.value();
    review.frame_chain_port_ids = std::move(frame.value().port_ids);
    review.frame_chain_port_digests = std::move(frame.value().port_digests);
    review.policy = policy;
    review.sweep_endpoint_world = transform_point(sweep_to_world, endpoint.position);
    review.sweep_outward_axis_world = sweep_axis;
    review.port_origin_world = port_to_world.translation;
    review.port_outward_axis_world = port_axis;
    review.position_error = (review.sweep_endpoint_world - review.port_origin_world).length();
    review.axis_alignment = std::clamp(core::dot(review.sweep_outward_axis_world,
        review.port_outward_axis_world), -1.0, 1.0);
    review.position_within_tolerance =
        review.position_error <= policy.maximum_position_error;
    review.axis_within_tolerance =
        review.axis_alignment >= policy.minimum_axis_alignment;
    review.aligned = review.position_within_tolerance && review.axis_within_tolerance;
    if (auto valid = review.validate(); !valid) {
        return core::Result<SweepEndpointSpatialReview>::failure(
            valid.error().with_context("sweep spatial review result"));
    }
    if (scene.revision() != captured_scene_revision) {
        return core::Result<SweepEndpointSpatialReview>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "scene changed while resolving sweep endpoint spatial fit"));
    }
    if (review.serialize().size() > SweepEndpointSpatialReview::kMaxSerializedBytes) {
        return core::Result<SweepEndpointSpatialReview>::failure(validation(
            "sweep endpoint spatial review exceeds its serialized byte bound"));
    }
    return core::Result<SweepEndpointSpatialReview>::success(std::move(review));
}

} // namespace carto::condition
