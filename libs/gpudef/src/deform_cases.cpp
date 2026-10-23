#include <carto/gpudef/deform_cases.hpp>

#include <carto/deform/dual_quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace carto::gpudef {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic refused(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

std::set<std::uint32_t> referenced_joints(const SkinInputView& skin) {
    std::set<std::uint32_t> joints;
    for (const kinematic::JointInfluence& influence : skin.influences) {
        joints.insert(influence.joint.value);
    }
    return joints;
}

// Canonical, deterministic text form of the driver channels; the corrective
// case's layout lane evaluates the driver over the channels decoded from
// this text, so the channel encoding itself is what the parity run binds.
std::string channels_canonical_text(const deform::DriverChannelValues& channels) {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTO_GPUDEF_DRIVER_CHANNELS 1\n";
    for (const auto& [control, value] : channels.controls) {
        output << "CONTROL " << control.value << ' ' << value << '\n';
    }
    for (const auto& [name, value] : channels.pose_parameters) {
        output << "POSE " << std::quoted(name) << ' ' << value << '\n';
    }
    for (const auto& [driver, value] : channels.drivers) {
        output << "DRIVER " << driver.value << ' ' << value << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<deform::DriverChannelValues> channels_from_canonical_text(
    std::string_view text) {
    if (text.empty() || text.size() > 1024U * 1024U) {
        return core::Result<deform::DriverChannelValues>::failure(invalid(
            "driver channel text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_DRIVER_CHANNELS" || version != 1U) {
        return core::Result<deform::DriverChannelValues>::failure(invalid(
            "driver channel framing or schema version is invalid"));
    }
    deform::DriverChannelValues channels;
    std::string token;
    while ((input >> token) && token != "END") {
        if (token == "CONTROL") {
            std::uint64_t id = 0U;
            double value = 0.0;
            if (!(input >> id >> value) || id == 0U) {
                return core::Result<deform::DriverChannelValues>::failure(invalid(
                    "driver channel control record is malformed"));
            }
            channels.controls.emplace(character::ControlId{id}, value);
        } else if (token == "POSE") {
            std::string name;
            double value = 0.0;
            if (!(input >> std::quoted(name) >> value)) {
                return core::Result<deform::DriverChannelValues>::failure(invalid(
                    "driver channel pose record is malformed"));
            }
            channels.pose_parameters.emplace(std::move(name), value);
        } else if (token == "DRIVER") {
            std::uint64_t id = 0U;
            double value = 0.0;
            if (!(input >> id >> value) || id == 0U) {
                return core::Result<deform::DriverChannelValues>::failure(invalid(
                    "driver channel driver record is malformed"));
            }
            channels.drivers.emplace(character::CorrectiveDriverId{id}, value);
        } else {
            return core::Result<deform::DriverChannelValues>::failure(invalid(
                "driver channel text contains an unknown record"));
        }
    }
    if (token != "END") {
        return core::Result<deform::DriverChannelValues>::failure(invalid(
            "driver channel text is unterminated"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<deform::DriverChannelValues>::failure(invalid(
            "driver channel text contains trailing unbound data"));
    }
    return core::Result<deform::DriverChannelValues>::success(std::move(channels));
}

// Decomposes a skin matrix into a rigid transform (unit scale, proper
// rotation). Anything else — shear, non-unit scale, reflection — is refused
// rather than approximated, matching the DQS contract in libs/deform.
core::Result<core::Transform> rigid_transform_from_mat34(const Mat34& matrix) {
    constexpr double kRigidityTolerance = 1e-9;
    const auto column = [&matrix](std::size_t index) {
        return core::Vec3d{
            matrix.rows[index], matrix.rows[4U + index], matrix.rows[8U + index]};
    };
    const core::Vec3d c0 = column(0U);
    const core::Vec3d c1 = column(1U);
    const core::Vec3d c2 = column(2U);
    const double n0 = c0.length();
    const double n1 = c1.length();
    const double n2 = c2.length();
    if (std::abs(n0 - 1.0) > kRigidityTolerance ||
        std::abs(n1 - 1.0) > kRigidityTolerance ||
        std::abs(n2 - 1.0) > kRigidityTolerance ||
        std::abs(core::dot(c0, c1)) > kRigidityTolerance ||
        std::abs(core::dot(c0, c2)) > kRigidityTolerance ||
        std::abs(core::dot(c1, c2)) > kRigidityTolerance) {
        return core::Result<core::Transform>::failure(refused(
            "dual-quaternion skinning requires rigid joint transforms"));
    }
    const double determinant =
        core::dot(c0, core::cross(c1, c2));
    if (determinant < 1.0 - kRigidityTolerance) {
        return core::Result<core::Transform>::failure(refused(
            "dual-quaternion skinning rejects reflected joint transforms"));
    }
    // Shepperd's method: quaternion from the rotation basis.
    const double trace = matrix.rows[0] + matrix.rows[5] + matrix.rows[10];
    core::Quaternion rotation{};
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        rotation = {
            (matrix.rows[9] - matrix.rows[6]) / s,
            (matrix.rows[2] - matrix.rows[8]) / s,
            (matrix.rows[4] - matrix.rows[1]) / s,
            0.25 * s,
        };
    } else if (matrix.rows[0] > matrix.rows[5] && matrix.rows[0] > matrix.rows[10]) {
        const double s = std::sqrt(1.0 + matrix.rows[0] - matrix.rows[5] -
            matrix.rows[10]) * 2.0;
        rotation = {
            0.25 * s,
            (matrix.rows[1] + matrix.rows[4]) / s,
            (matrix.rows[2] + matrix.rows[8]) / s,
            (matrix.rows[9] - matrix.rows[6]) / s,
        };
    } else if (matrix.rows[5] > matrix.rows[10]) {
        const double s = std::sqrt(1.0 + matrix.rows[5] - matrix.rows[0] -
            matrix.rows[10]) * 2.0;
        rotation = {
            (matrix.rows[1] + matrix.rows[4]) / s,
            0.25 * s,
            (matrix.rows[6] + matrix.rows[9]) / s,
            (matrix.rows[2] - matrix.rows[8]) / s,
        };
    } else {
        const double s = std::sqrt(1.0 + matrix.rows[10] - matrix.rows[0] -
            matrix.rows[5]) * 2.0;
        rotation = {
            (matrix.rows[2] + matrix.rows[8]) / s,
            (matrix.rows[6] + matrix.rows[9]) / s,
            0.25 * s,
            (matrix.rows[4] - matrix.rows[1]) / s,
        };
    }
    core::Transform transform;
    transform.rotation = rotation.normalized();
    transform.translation = {matrix.rows[3], matrix.rows[7], matrix.rows[11]};
    transform.scale = {1.0, 1.0, 1.0};
    if (!transform.finite()) {
        return core::Result<core::Transform>::failure(refused(
            "dual-quaternion skinning requires finite joint transforms"));
    }
    return core::Result<core::Transform>::success(transform);
}

// One evaluated vertex through the dual-quaternion case kernel, using the
// same hemisphere and reference conventions as libs/deform evaluate_dqs:
// the reference is the highest-weight influence (ties resolve to the
// lowest joint id because influence lists are joint-sorted), terms are
// sign-corrected against it before accumulation, and the blend is
// renormalized onto the canonical hemisphere.
core::Vec3d dqs_deform_position(
    core::Vec3d bind_position,
    std::span<const kinematic::JointInfluence> influences,
    std::span<const deform::DualQuaternion> joint_dq) {
    if (influences.empty()) {
        // Matches the packed LBS kernel: an uninfluenced vertex carries no
        // transform and lands on the origin in both lanes.
        return {0.0, 0.0, 0.0};
    }
    const deform::DualQuaternion* reference = nullptr;
    double reference_weight = -1.0;
    for (const kinematic::JointInfluence& influence : influences) {
        if (influence.weight > reference_weight) {
            reference_weight = influence.weight;
            reference = &joint_dq[influence.joint.value];
        }
    }
    deform::DualQuaternion blended{{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}};
    for (const kinematic::JointInfluence& influence : influences) {
        deform::DualQuaternion term = joint_dq[influence.joint.value] * influence.weight;
        if (deform::real_dot(*reference, term) < 0.0) {
            term.real = {-term.real.x, -term.real.y, -term.real.z, -term.real.w};
            term.dual = {-term.dual.x, -term.dual.y, -term.dual.z, -term.dual.w};
        }
        blended = blended + term;
    }
    const deform::DualQuaternion unit = deform::normalized(blended);
    const core::Transform rigid = deform::rigid_from_dual_quaternion(unit);
    return rigid.rotation.rotate(bind_position) + rigid.translation;
}

// Per-case workload validation and preparation. Returns the per-joint dual
// quaternions for the skinning cases (empty otherwise).
core::Result<std::vector<deform::DualQuaternion>> prepare_case(
    const CaseParityInput& input,
    std::set<std::uint32_t>& joints_out) {
    switch (input.deformation_case) {
    case DeformationCase::linear_blend_skinning:
    case DeformationCase::dual_quaternion_skinning: {
        if (input.skin_matrices.empty()) {
            return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                "skinning cases require at least one skin matrix"));
        }
        joints_out = referenced_joints(input.skin);
        if (joints_out.empty()) {
            // Rigid-only skins are valid; no matrices are referenced.
            return core::Result<std::vector<deform::DualQuaternion>>::success({});
        }
        if (*joints_out.rbegin() >= input.skin_matrices.size()) {
            return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                "skin influence references a joint without a skin matrix"));
        }
        for (const std::uint32_t joint : joints_out) {
            if (!input.skin_matrices[joint].finite()) {
                return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                    "skin matrices must be finite"));
            }
        }
        if (input.deformation_case == DeformationCase::linear_blend_skinning) {
            return core::Result<std::vector<deform::DualQuaternion>>::success({});
        }
        std::vector<deform::DualQuaternion> joint_dq(input.skin_matrices.size());
        for (const std::uint32_t joint : joints_out) {
            auto rigid = rigid_transform_from_mat34(input.skin_matrices[joint]);
            if (!rigid) {
                return core::Result<std::vector<deform::DualQuaternion>>::failure(
                    rigid.error());
            }
            auto dual = deform::dual_quaternion_from_rigid(rigid.value());
            if (!dual) {
                return core::Result<std::vector<deform::DualQuaternion>>::failure(
                    dual.error());
            }
            joint_dq[joint] = dual.value();
        }
        return core::Result<std::vector<deform::DualQuaternion>>::success(
            std::move(joint_dq));
    }
    case DeformationCase::morph_stage: {
        if (input.morph_deltas.empty()) {
            return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                "morph stage case requires at least one delta binding"));
        }
        if (!std::isfinite(input.morph_weight) ||
            input.morph_weight < 0.0 || input.morph_weight > 1.0) {
            return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                "morph weight must be finite and within [0, 1]"));
        }
        std::set<std::uint32_t> seen;
        for (const MorphDeltaBinding& delta : input.morph_deltas) {
            if (!delta.delta.finite() ||
                delta.vertex_index >= input.skin.positions.size() ||
                !seen.insert(delta.vertex_index).second) {
                return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                    "morph deltas must be finite, in range, and unique"));
            }
        }
        return core::Result<std::vector<deform::DualQuaternion>>::success({});
    }
    case DeformationCase::corrective_driver: {
        if (input.corrective_driver == nullptr || input.corrective_channels == nullptr) {
            return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
                "corrective driver case requires a driver policy and channels"));
        }
        return core::Result<std::vector<deform::DualQuaternion>>::success({});
    }
    }
    return core::Result<std::vector<deform::DualQuaternion>>::failure(invalid(
        "unknown deformation case"));
}

// Evaluates the case kernel for one vertex over an arbitrary position +
// influence list. corrective_weight carries the evaluated driver activation
// for the corrective case.
core::Vec3d case_deform_position(
    const CaseParityInput& input,
    core::Vec3d position,
    std::span<const kinematic::JointInfluence> influences,
    std::span<const Mat34> skin_matrices,
    std::span<const deform::DualQuaternion> joint_dq,
    std::span<const MorphDeltaBinding> morph_deltas,
    double corrective_weight,
    std::uint32_t vertex_index) {
    switch (input.deformation_case) {
    case DeformationCase::linear_blend_skinning:
        return reference_deform_position(position, influences, skin_matrices);
    case DeformationCase::dual_quaternion_skinning:
        return dqs_deform_position(position, influences, joint_dq);
    case DeformationCase::morph_stage: {
        core::Vec3d displaced = position;
        for (const MorphDeltaBinding& delta : morph_deltas) {
            if (delta.vertex_index == vertex_index) {
                displaced = displaced + delta.delta * input.morph_weight;
            }
        }
        return displaced;
    }
    case DeformationCase::corrective_driver:
        return position * corrective_weight;
    }
    return position;
}

core::Result<double> evaluate_corrective_weight(
    const CaseParityInput& input,
    const deform::DriverChannelValues& channels) {
    auto evaluated = deform::evaluate_driver(*input.corrective_driver, channels);
    if (!evaluated) {
        return core::Result<double>::failure(evaluated.error());
    }
    return core::Result<double>::success(evaluated.value().weight);
}

} // namespace

std::string_view deformation_case_name(DeformationCase deformation_case) noexcept {
    switch (deformation_case) {
    case DeformationCase::linear_blend_skinning: return "linear_blend_skinning";
    case DeformationCase::dual_quaternion_skinning: return "dual_quaternion_skinning";
    case DeformationCase::morph_stage: return "morph_stage";
    case DeformationCase::corrective_driver: return "corrective_driver";
    }
    return "unknown";
}

core::Result<void> CaseEnvelope::validate() const {
    if (!std::isfinite(declared_position_error_bound) ||
        !std::isfinite(declared_weight_error_bound) ||
        declared_position_error_bound < 0.0 ||
        declared_weight_error_bound < 0.0) {
        return core::Result<void>::failure(invalid(
            "case envelope declared bounds must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

std::string CaseEnvelope::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTO_GPUDEF_CASE_ENVELOPE 1\n";
    output << "CASE " << deformation_case_name(deformation_case) << '\n';
    output << "STREAM " << quantization_mode_name(mode) << ' '
           << influence_layout_name(layout) << '\n';
    output << "CONTRACT " << (exact_match ? "exact" : "bounded") << ' '
           << declared_position_error_bound << ' '
           << declared_weight_error_bound << '\n';
    output << "END\n";
    return output.str();
}

core::Result<CaseEnvelope> CaseEnvelope::deserialize(std::string_view text) {
    if (text.empty() || text.size() > 8U * 1024U) {
        return core::Result<CaseEnvelope>::failure(invalid(
            "case envelope text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_CASE_ENVELOPE" || version != 1U) {
        return core::Result<CaseEnvelope>::failure(invalid(
            "case envelope framing or schema version is invalid"));
    }
    CaseEnvelope envelope;
    std::string token;
    auto read_case = [&input](DeformationCase& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const DeformationCase candidate : {
                DeformationCase::linear_blend_skinning,
                DeformationCase::dual_quaternion_skinning,
                DeformationCase::morph_stage,
                DeformationCase::corrective_driver}) {
            if (name == deformation_case_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    auto read_mode = [&input](QuantizationMode& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const QuantizationMode candidate : {
                QuantizationMode::fp32, QuantizationMode::fp16,
                QuantizationMode::unorm16}) {
            if (name == quantization_mode_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    auto read_layout = [&input](InfluenceLayout& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const InfluenceLayout candidate : {
                InfluenceLayout::influences_8, InfluenceLayout::influences_4}) {
            if (name == influence_layout_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    std::string contract;
    if (!((input >> token) && token == "CASE" && read_case(envelope.deformation_case)) ||
        !((input >> token) && token == "STREAM" &&
            read_mode(envelope.mode) && read_layout(envelope.layout)) ||
        !((input >> token) && token == "CONTRACT" && (input >> contract) &&
            (contract == "exact" || contract == "bounded")) ||
        !(input >> envelope.declared_position_error_bound >>
            envelope.declared_weight_error_bound) ||
        !((input >> token) && token == "END")) {
        return core::Result<CaseEnvelope>::failure(invalid(
            "case envelope payload is malformed"));
    }
    envelope.exact_match = contract == "exact";
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CaseEnvelope>::failure(invalid(
            "case envelope text contains trailing unbound data"));
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<CaseEnvelope>::failure(result.error());
    }
    return core::Result<CaseEnvelope>::success(std::move(envelope));
}

core::Result<CaseEnvelope> declare_case_envelope(
    DeformationCase deformation_case,
    InfluenceLayout layout,
    QuantizationMode mode,
    const QuantizationErrorReceipt& pack_error,
    const EnvelopeContext& context) {
    if (auto result = pack_error.validate(); !result) {
        return core::Result<CaseEnvelope>::failure(result.error());
    }
    if (!std::isfinite(context.max_skin_scale) || context.max_skin_scale < 0.0 ||
        !std::isfinite(context.max_source_magnitude) ||
        context.max_source_magnitude < 0.0 ||
        !std::isfinite(context.driver_weight)) {
        return core::Result<CaseEnvelope>::failure(invalid(
            "envelope context must be finite and non-negative"));
    }
    const double position_vector_bound =
        std::sqrt(3.0) * pack_error.declared_position_error_bound;
    const double influence_count =
        static_cast<double>(influences_per_vertex(layout));
    const double weight_vector_bound =
        influence_count * pack_error.declared_weight_error_bound *
        context.max_skin_scale *
        (context.max_source_magnitude + position_vector_bound);

    CaseEnvelope envelope;
    envelope.deformation_case = deformation_case;
    envelope.layout = layout;
    envelope.mode = mode;
    envelope.exact_match = pack_error.max_position_component_error == 0.0 &&
        pack_error.max_weight_error == 0.0;
    envelope.declared_weight_error_bound = pack_error.declared_weight_error_bound;
    switch (deformation_case) {
    case DeformationCase::linear_blend_skinning:
    case DeformationCase::dual_quaternion_skinning:
        // |sum_i w_i M_i dp| <= scale * |dp| and
        // |sum_i dw_i M_i p|  <= n * dw * scale * |p|; both terms contribute.
        envelope.declared_position_error_bound =
            context.max_skin_scale * position_vector_bound + weight_vector_bound;
        break;
    case DeformationCase::morph_stage:
        // The delta path is exact in fp64 on both lanes; only the position
        // quantization differs.
        envelope.declared_position_error_bound = position_vector_bound;
        break;
    case DeformationCase::corrective_driver:
        // The driver activation is evaluated in fp64 on both lanes and must
        // agree exactly; only the scaled position path can differ.
        envelope.declared_position_error_bound =
            std::abs(context.driver_weight) * position_vector_bound;
        break;
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<CaseEnvelope>::failure(result.error());
    }
    return core::Result<CaseEnvelope>::success(std::move(envelope));
}

namespace {

// Shared driver of both parity lanes. lane_selector false = packed lane
// (decoded positions/influences), true = source lane.
core::Result<std::vector<core::Vec3d>> evaluate_case_lane(
    const CaseParityInput& input,
    bool source_lane) {
    std::set<std::uint32_t> referenced_joints;
    auto prepared = prepare_case(input, referenced_joints);
    if (!prepared) {
        return core::Result<std::vector<core::Vec3d>>::failure(prepared.error());
    }
    auto packed = pack(input.skin, input.layout, input.mode);
    if (!packed) {
        return core::Result<std::vector<core::Vec3d>>::failure(packed.error());
    }

    std::vector<core::Vec3d> lane_positions;
    std::vector<std::uint32_t> lane_counts;
    std::vector<kinematic::JointInfluence> lane_influences;
    if (source_lane) {
        lane_positions.assign(input.skin.positions.begin(), input.skin.positions.end());
        lane_counts.assign(input.skin.influence_counts.begin(),
            input.skin.influence_counts.end());
        lane_influences.assign(input.skin.influences.begin(),
            input.skin.influences.end());
    } else {
        auto decoded = unpack(packed.value().buffer);
        if (!decoded) {
            return core::Result<std::vector<core::Vec3d>>::failure(decoded.error());
        }
        lane_positions = std::move(decoded.value().positions);
        lane_counts = std::move(decoded.value().influence_counts);
        lane_influences = std::move(decoded.value().influences);
    }

    double corrective_weight = 1.0;
    if (input.deformation_case == DeformationCase::corrective_driver) {
        if (source_lane) {
            auto weight = evaluate_corrective_weight(input, *input.corrective_channels);
            if (!weight) {
                return core::Result<std::vector<core::Vec3d>>::failure(weight.error());
            }
            corrective_weight = weight.value();
        } else {
            // The layout lane re-derives the channels from their canonical
            // encoding, so a mismatch between the encoding and the direct
            // evaluation surfaces as envelope error.
            const std::string canonical =
                channels_canonical_text(*input.corrective_channels);
            auto channels = channels_from_canonical_text(canonical);
            if (!channels) {
                return core::Result<std::vector<core::Vec3d>>::failure(
                    channels.error());
            }
            auto weight = evaluate_corrective_weight(input, channels.value());
            if (!weight) {
                return core::Result<std::vector<core::Vec3d>>::failure(weight.error());
            }
            corrective_weight = weight.value();
        }
    }

    std::vector<core::Vec3d> output;
    output.reserve(lane_positions.size());
    std::size_t cursor = 0U;
    for (std::size_t vertex = 0U; vertex < lane_positions.size(); ++vertex) {
        if (cursor + lane_counts[vertex] > lane_influences.size()) {
            return core::Result<std::vector<core::Vec3d>>::failure(invalid(
                "lane influence stream is truncated"));
        }
        const core::Vec3d deformed = case_deform_position(
            input,
            lane_positions[vertex],
            std::span<const kinematic::JointInfluence>{
                lane_influences.data() + cursor, lane_counts[vertex]},
            input.skin_matrices,
            prepared.value(),
            input.morph_deltas,
            corrective_weight,
            static_cast<std::uint32_t>(vertex));
        if (!deformed.finite()) {
            return core::Result<std::vector<core::Vec3d>>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "deformation case produced a non-finite position"));
        }
        output.push_back(deformed);
        cursor += lane_counts[vertex];
    }
    return core::Result<std::vector<core::Vec3d>>::success(std::move(output));
}

} // namespace

core::Result<std::vector<core::Vec3d>> run_case_emulation(
    const CaseParityInput& input) {
    return evaluate_case_lane(input, false);
}

core::Result<std::vector<core::Vec3d>> run_case_reference(
    const CaseParityInput& input) {
    return evaluate_case_lane(input, true);
}

bool admitted_by_envelope(const CaseParityReceipt& receipt) noexcept {
    if (receipt.exact_match_declared) {
        return receipt.max_position_error == 0.0;
    }
    return receipt.max_position_error <= receipt.declared_position_error_bound;
}

bool admitted_by_envelope(
    const CaseEnvelope& envelope,
    const CaseParityReceipt& receipt) noexcept {
    if (envelope.deformation_case != receipt.deformation_case ||
        envelope.layout != receipt.layout || envelope.mode != receipt.mode) {
        return false;
    }
    if (envelope.exact_match) {
        return receipt.max_position_error == 0.0;
    }
    return receipt.max_position_error <= envelope.declared_position_error_bound;
}

core::Result<void> CaseParityReceipt::validate() const {
    if (vertex_count == 0U || joint_count == 0U ||
        joint_count > PackedVertexBuffer::kMaxJoints ||
        max_position_error < 0.0 || bounding_diagonal <= 0.0 ||
        declared_position_error_bound < 0.0 ||
        !std::isfinite(max_position_error) || !std::isfinite(bounding_diagonal) ||
        !std::isfinite(declared_position_error_bound)) {
        return core::Result<void>::failure(invalid(
            "case parity receipt counts or error statistics are invalid"));
    }
    if (!admitted_by_envelope(*this)) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "case parity receipt violates its declared admission envelope"));
    }
    if (packed_buffer_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "case parity receipt must be bound to a packed stream digest"));
    }
    if (device_profile) {
        if (auto result = device_profile->validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
        if (tier != EvidenceTier::native_device) {
            return core::Result<void>::failure(invalid(
                "case parity receipt with a named device profile must declare "
                "native-device tier"));
        }
    } else if (tier != EvidenceTier::cpu_reference) {
        return core::Result<void>::failure(invalid(
            "case parity receipt without a named device profile must stay "
            "cpu-reference"));
    }
    return core::Result<void>::success();
}

std::string CaseParityReceipt::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTO_GPUDEF_CASE_PARITY_RECEIPT 1\n";
    output << "CASE " << deformation_case_name(deformation_case) << '\n';
    output << "STREAM " << quantization_mode_name(mode) << ' '
           << influence_layout_name(layout) << ' ' << vertex_count << ' '
           << joint_count << '\n';
    output << "ERROR " << max_position_error << ' ' << bounding_diagonal << '\n';
    output << "ENVELOPE " << (exact_match_declared ? "exact" : "bounded") << ' '
           << declared_position_error_bound << '\n';
    output << "VERDICT " << (admitted_by_envelope(*this) ? "admitted" : "rejected")
           << '\n';
    output << "PACKED " << packed_buffer_digest.hex() << '\n';
    output << "TIER " << evidence_tier_name(tier);
    if (device_profile) {
        // The full named-hardware identity rides along: a native-tier
        // receipt must round-trip its profile or it cannot revalidate.
        output << ' ' << std::quoted(device_profile->adapter_id) << ' '
               << std::quoted(device_profile->gpu_name) << ' '
               << std::quoted(device_profile->driver_version) << ' '
               << std::quoted(device_profile->os_name) << ' '
               << std::quoted(device_profile->precision_identity);
    }
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CaseParityReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<CaseParityReceipt> CaseParityReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > 64U * 1024U) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_CASE_PARITY_RECEIPT" || version != 1U) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt framing or schema version is invalid"));
    }
    CaseParityReceipt receipt;
    std::string token;
    auto read_case = [&input](DeformationCase& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const DeformationCase candidate : {
                DeformationCase::linear_blend_skinning,
                DeformationCase::dual_quaternion_skinning,
                DeformationCase::morph_stage,
                DeformationCase::corrective_driver}) {
            if (name == deformation_case_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    auto read_mode = [&input](QuantizationMode& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const QuantizationMode candidate : {
                QuantizationMode::fp32, QuantizationMode::fp16,
                QuantizationMode::unorm16}) {
            if (name == quantization_mode_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    auto read_layout = [&input](InfluenceLayout& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const InfluenceLayout candidate : {
                InfluenceLayout::influences_8, InfluenceLayout::influences_4}) {
            if (name == influence_layout_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    std::string contract;
    std::string verdict;
    std::string tier_name;
    std::string digest_hex;
    if (!((input >> token) && token == "CASE" && read_case(receipt.deformation_case)) ||
        !((input >> token) && token == "STREAM" &&
            read_mode(receipt.mode) && read_layout(receipt.layout) &&
            (input >> receipt.vertex_count >> receipt.joint_count)) ||
        !((input >> token) && token == "ERROR" &&
            (input >> receipt.max_position_error >> receipt.bounding_diagonal)) ||
        !((input >> token) && token == "ENVELOPE" && (input >> contract) &&
            (contract == "exact" || contract == "bounded") &&
            (input >> receipt.declared_position_error_bound)) ||
        !((input >> token) && token == "VERDICT" && (input >> verdict) &&
            (verdict == "admitted" || verdict == "rejected")) ||
        !((input >> token) && token == "PACKED" && (input >> digest_hex)) ||
        !((input >> token) && token == "TIER" && (input >> tier_name))) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt payload is malformed"));
    }
    receipt.exact_match_declared = contract == "exact";
    if (verdict != (admitted_by_envelope(receipt) ? "admitted" : "rejected")) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt verdict does not match its measured error"));
    }
    auto digest = assets::Sha256Digest::from_hex(digest_hex);
    if (!digest) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt packed digest is malformed"));
    }
    receipt.packed_buffer_digest = digest.value();
    if (tier_name == "cpu-reference") {
        receipt.tier = EvidenceTier::cpu_reference;
    } else if (tier_name == "native-device") {
        receipt.tier = EvidenceTier::native_device;
        DeviceProfile profile;
        if (!(input >> std::quoted(profile.adapter_id) >>
                std::quoted(profile.gpu_name) >>
                std::quoted(profile.driver_version) >>
                std::quoted(profile.os_name) >>
                std::quoted(profile.precision_identity))) {
            return core::Result<CaseParityReceipt>::failure(invalid(
                "native-device case parity receipt requires a full device profile"));
        }
        receipt.device_profile = std::move(profile);
    } else {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt tier is unknown"));
    }
    if (!((input >> token) && token == "END")) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt is unterminated"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case parity receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<CaseParityReceipt>::failure(result.error());
    }
    return core::Result<CaseParityReceipt>::success(std::move(receipt));
}

core::Result<CaseParityReceipt> run_case_parity(const CaseParityInput& input) {
    if (input.reference_is_device_measured && !input.device_profile) {
        return core::Result<CaseParityReceipt>::failure(refused(
            "device-measured case evidence requires an attached named device "
            "profile"));
    }
    if (input.device_profile) {
        if (auto result = input.device_profile->validate(); !result) {
            return core::Result<CaseParityReceipt>::failure(result.error());
        }
        if (!input.reference_is_device_measured) {
            return core::Result<CaseParityReceipt>::failure(invalid(
                "a named device profile without device-measured reference "
                "positions cannot upgrade the tier"));
        }
    }

    auto packed = pack(input.skin, input.layout, input.mode);
    if (!packed) {
        return core::Result<CaseParityReceipt>::failure(packed.error());
    }
    auto emulated = evaluate_case_lane(input, false);
    if (!emulated) {
        return core::Result<CaseParityReceipt>::failure(emulated.error());
    }
    auto reference = evaluate_case_lane(input, true);
    if (!reference) {
        return core::Result<CaseParityReceipt>::failure(reference.error());
    }

    CaseParityReceipt receipt;
    receipt.deformation_case = input.deformation_case;
    receipt.mode = input.mode;
    receipt.layout = input.layout;
    receipt.vertex_count = static_cast<std::uint32_t>(input.skin.positions.size());
    receipt.joint_count = input.skin.joint_count;

    core::Bounds3d bounds;
    for (const core::Vec3d& position : reference.value()) {
        bounds.include(position);
    }
    if (!bounds.valid()) {
        return core::Result<CaseParityReceipt>::failure(invalid(
            "case reference positions must be finite and non-empty"));
    }
    const double dx = bounds.maximum.x - bounds.minimum.x;
    const double dy = bounds.maximum.y - bounds.minimum.y;
    const double dz = bounds.maximum.z - bounds.minimum.z;
    receipt.bounding_diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);

    for (std::size_t vertex = 0U; vertex < reference.value().size(); ++vertex) {
        receipt.max_position_error = std::max(receipt.max_position_error,
            (emulated.value()[vertex] - reference.value()[vertex]).length());
    }

    EnvelopeContext context;
    context.max_source_magnitude = 0.0;
    for (const core::Vec3d& position : input.skin.positions) {
        context.max_source_magnitude = std::max({
            context.max_source_magnitude,
            std::abs(position.x), std::abs(position.y), std::abs(position.z)});
    }
    context.max_skin_scale = 0.0;
    if (input.deformation_case == DeformationCase::linear_blend_skinning ||
        input.deformation_case == DeformationCase::dual_quaternion_skinning) {
        for (const std::uint32_t joint : referenced_joints(input.skin)) {
            const Mat34& matrix = input.skin_matrices[joint];
            const double scale = std::max({
                std::abs(matrix.rows[0]) + std::abs(matrix.rows[1]) +
                    std::abs(matrix.rows[2]),
                std::abs(matrix.rows[4]) + std::abs(matrix.rows[5]) +
                    std::abs(matrix.rows[6]),
                std::abs(matrix.rows[8]) + std::abs(matrix.rows[9]) +
                    std::abs(matrix.rows[10])});
            context.max_skin_scale = std::max(context.max_skin_scale, scale);
        }
    }
    if (input.deformation_case == DeformationCase::corrective_driver) {
        auto weight = evaluate_corrective_weight(input, *input.corrective_channels);
        if (!weight) {
            return core::Result<CaseParityReceipt>::failure(weight.error());
        }
        context.driver_weight = weight.value();
    }

    auto envelope = declare_case_envelope(
        input.deformation_case, input.layout, input.mode,
        packed.value().error, context);
    if (!envelope) {
        return core::Result<CaseParityReceipt>::failure(envelope.error());
    }
    receipt.declared_position_error_bound =
        envelope.value().declared_position_error_bound;
    receipt.exact_match_declared = envelope.value().exact_match;
    receipt.envelope_admitted = admitted_by_envelope(envelope.value(), receipt);
    if (!receipt.envelope_admitted) {
        return core::Result<CaseParityReceipt>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "measured case parity error violates the declared envelope"));
    }

    auto buffer_digest = packed.value().buffer.canonical_digest();
    if (!buffer_digest) {
        return core::Result<CaseParityReceipt>::failure(buffer_digest.error());
    }
    receipt.packed_buffer_digest = buffer_digest.value();
    receipt.device_profile = input.device_profile;
    receipt.tier = (input.device_profile && input.reference_is_device_measured)
        ? EvidenceTier::native_device
        : EvidenceTier::cpu_reference;
    if (auto result = receipt.validate(); !result) {
        return core::Result<CaseParityReceipt>::failure(result.error());
    }
    return core::Result<CaseParityReceipt>::success(std::move(receipt));
}

EvidenceTier effective_tier(const CaseParityReceipt& receipt) noexcept {
    if (receipt.device_profile && receipt.tier == EvidenceTier::native_device) {
        return EvidenceTier::native_device;
    }
    return EvidenceTier::cpu_reference;
}

core::Result<void> require_evidence_tier(
    const CaseParityReceipt& receipt,
    EvidenceTier requested) {
    switch (requested) {
    case EvidenceTier::structural:
    case EvidenceTier::cpu_reference:
        return core::Result<void>::success();
    case EvidenceTier::native_device:
        if (effective_tier(receipt) == EvidenceTier::native_device) {
            return core::Result<void>::success();
        }
        return core::Result<void>::failure(refused(
            "native-device case evidence requires an attached named-hardware "
            "profile and device-measured reference positions"));
    }
    return core::Result<void>::failure(refused(
        "requested evidence tier is beyond what a case parity receipt can hold"));
}

} // namespace carto::gpudef
