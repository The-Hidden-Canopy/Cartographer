#include <carto/pose/ik_fk_authoring.hpp>

#include <carto/character/rig_record.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace carto::pose {
namespace {
core::Diagnostic invalid(const char* message) {
    return {core::ErrorCode::validation_failed, message};
}
bool token(std::istream& in, std::string_view expected) {
    std::string actual;
    return static_cast<bool>(in >> actual) && actual == expected;
}
bool digest(std::istream& in, assets::Sha256Digest& result) {
    std::string text;
    if (!(in >> text)) return false;
    auto parsed = assets::Sha256Digest::from_hex(text);
    if (!parsed) return false;
    result = parsed.value();
    return true;
}
} // namespace

core::Result<void> IkFkAuthoring::validate() const {
    if (!rig || rig_digest.is_zero() || graph_digest.is_zero() || limits_digest.is_zero() ||
        !chain.root || !chain.mid || !chain.effector || chain.root == chain.mid ||
        chain.root == chain.effector || chain.mid == chain.effector ||
        !fk_root || !fk_mid || !fk_effector || !ik_target || !mode_control ||
        (ik_pole && !*ik_pole) || mode_axis >= character::kAxisCount ||
        !std::isfinite(fk_mode_value) || !std::isfinite(ik_mode_value) ||
        fk_mode_value == ik_mode_value || !std::isfinite(max_position_jump) ||
        max_position_jump <= 0.0 || budget.max_seconds != 0.0)
        return core::Result<void>::failure(invalid("invalid IK/FK authoring identity, roles, mode or deterministic budget"));
    std::set<character::ControlId> roles{fk_root, fk_mid, fk_effector, ik_target, mode_control};
    if (roles.size() != 5U || (ik_pole && !roles.insert(*ik_pole).second))
        return core::Result<void>::failure(invalid("IK/FK control roles must be distinct"));
    return budget.validate();
}

namespace {
struct SharedIkFkContext {
    assets::Sha256Digest rig_hash;
    assets::Sha256Digest graph_hash;
    assets::Sha256Digest limits_hash;
};

core::Result<SharedIkFkContext> validate_shared_context(
    const character::RigDefinition& definition, const kinematic::KinematicGraph& graph,
    const kinematic::JointLimitSet& limits) {
    using Result = core::Result<SharedIkFkContext>;
    auto rig_hash = character::rig_definition_digest(definition);
    auto graph_hash = graph.canonical_digest();
    auto limits_hash = limits.canonical_digest();
    if (!rig_hash || !graph_hash || !limits_hash ||
        definition.kinematic_graph_digest != graph_hash.value() ||
        definition.joint_limits.digest != limits_hash.value())
        return Result::failure(invalid("IK/FK authoring does not match the exact rig, graph and limits"));
    if (auto checked = limits.validate_for_graph(graph); !checked)
        return Result::failure(checked.error());
    for (const auto& [id, control] : definition.control_graph.nodes()) {
        (void)id;
        for (const auto joint : control.affected_joints)
            if (!graph.find(joint))
                return Result::failure(invalid("rig control references a joint absent from the graph"));
    }
    for (const auto& pair : definition.symmetry)
        if (!graph.find(pair.left_joint) || !graph.find(pair.right_joint))
            return Result::failure(invalid("rig symmetry references a joint absent from the graph"));
    return Result::success({rig_hash.value(), graph_hash.value(), limits_hash.value()});
}

core::Result<void> validate_record_against_context(
    const IkFkAuthoring& record, const character::RigDefinition& definition,
    const kinematic::KinematicGraph& graph, const SharedIkFkContext& context) {
    if (definition.id != record.rig || context.rig_hash != record.rig_digest ||
        context.graph_hash != record.graph_digest || context.limits_hash != record.limits_digest)
        return core::Result<void>::failure(invalid("IK/FK authoring does not match the exact rig, graph and limits"));
    const auto* root = graph.find(record.chain.root);
    const auto* mid = graph.find(record.chain.mid);
    const auto* effector = graph.find(record.chain.effector);
    if (!root || !mid || !effector || !mid->parent || *mid->parent != record.chain.root ||
        !effector->parent || *effector->parent != record.chain.mid)
        return core::Result<void>::failure(invalid("IK/FK chain must be a contiguous parent-child chain"));
    const auto& controls = definition.control_graph;
    const auto drives = [&controls](character::ControlId id, kinematic::JointId joint) {
        const auto* control = controls.find(id);
        return control && std::find(control->affected_joints.begin(), control->affected_joints.end(), joint) != control->affected_joints.end();
    };
    if (!drives(record.fk_root, record.chain.root) || !drives(record.fk_mid, record.chain.mid) ||
        !drives(record.fk_effector, record.chain.effector) || !controls.find(record.ik_target) ||
        (record.ik_pole && !controls.find(*record.ik_pole)))
        return core::Result<void>::failure(invalid("IK/FK control mapping contains an absent or unrelated control"));
    const auto* mode = controls.find(record.mode_control);
    const auto mask = static_cast<std::uint8_t>(1U << record.mode_axis);
    if (!mode || (mode->allowed_axes & mask) == 0U ||
        record.fk_mode_value < mode->axis_minimum[record.mode_axis] || record.fk_mode_value > mode->axis_maximum[record.mode_axis] ||
        record.ik_mode_value < mode->axis_minimum[record.mode_axis] || record.ik_mode_value > mode->axis_maximum[record.mode_axis])
        return core::Result<void>::failure(invalid("mode values must be admitted by the declared control axis"));
    return core::Result<void>::success();
}
} // namespace

core::Result<void> validate_ik_fk_authoring_set(
    std::span<const IkFkAuthoring> records, const character::RigDefinition& definition,
    const kinematic::KinematicGraph& graph, const kinematic::JointLimitSet& limits) {
    if (records.size() > IkFkAuthoring::kMaxSetSize)
        return core::Result<void>::failure(invalid("IK/FK authoring set exceeds its bound"));
    for (const auto& record : records)
        if (auto checked = record.validate(); !checked) return checked;
    if (records.empty()) return core::Result<void>::success();
    auto context = validate_shared_context(definition, graph, limits);
    if (!context) return core::Result<void>::failure(context.error());
    for (const auto& record : records)
        if (auto checked = validate_record_against_context(record, definition, graph, context.value()); !checked)
            return checked;
    return core::Result<void>::success();
}

core::Result<void> IkFkAuthoring::validate_against(
    const character::RigDefinition& definition, const kinematic::KinematicGraph& graph,
    const kinematic::JointLimitSet& limits) const {
    return validate_ik_fk_authoring_set(std::span<const IkFkAuthoring>(this, 1U), definition, graph, limits);
}

std::string IkFkAuthoring::serialize() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "CARTO_IK_FK_AUTHORING 1\nRIG " << rig.value << ' '
        << rig_digest.hex() << ' ' << graph_digest.hex() << ' ' << limits_digest.hex()
        << "\nCHAIN " << chain.root.value << ' ' << chain.mid.value << ' ' << chain.effector.value
        << "\nCONTROLS " << fk_root.value << ' ' << fk_mid.value << ' ' << fk_effector.value << ' '
        << ik_target.value << ' ' << (ik_pole ? ik_pole->value : 0U) << ' ' << mode_control.value << ' '
        << static_cast<unsigned>(mode_axis) << ' ' << fk_mode_value << ' ' << ik_mode_value
        << "\nSOLVER " << budget.max_iterations << ' ' << budget.convergence_tolerance << ' '
        << budget.damping << ' ' << budget.max_seconds << ' ' << max_position_jump << "\nEND\n";
    return out.str();
}

core::Result<IkFkAuthoring> IkFkAuthoring::deserialize(std::string_view text) {
    using Result = core::Result<IkFkAuthoring>;
    const auto fail = [] { return Result::failure(invalid("malformed or noncanonical IK/FK authoring record")); };
    if (text.empty() || text.size() > kMaxSerializedBytes) return fail();
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    IkFkAuthoring value;
    unsigned version = 0U, axis = 0U;
    std::uint64_t pole = 0U;
    if (!token(in, "CARTO_IK_FK_AUTHORING") || !(in >> version) || version != 1U ||
        !token(in, "RIG") || !(in >> value.rig.value) || !digest(in, value.rig_digest) ||
        !digest(in, value.graph_digest) || !digest(in, value.limits_digest) ||
        !token(in, "CHAIN") || !(in >> value.chain.root.value >> value.chain.mid.value >> value.chain.effector.value) ||
        !token(in, "CONTROLS") || !(in >> value.fk_root.value >> value.fk_mid.value >> value.fk_effector.value >>
            value.ik_target.value >> pole >> value.mode_control.value >> axis >> value.fk_mode_value >> value.ik_mode_value) ||
        axis >= character::kAxisCount || !token(in, "SOLVER") || !(in >> value.budget.max_iterations >>
            value.budget.convergence_tolerance >> value.budget.damping >> value.budget.max_seconds >> value.max_position_jump) ||
        !token(in, "END")) return fail();
    value.mode_axis = static_cast<std::uint8_t>(axis);
    if (pole) value.ik_pole = character::ControlId{pole};
    if (!value.validate() || value.serialize() != text) return fail();
    return Result::success(std::move(value));
}

core::Result<assets::Sha256Digest> IkFkAuthoring::canonical_digest() const {
    if (auto checked = validate(); !checked)
        return core::Result<assets::Sha256Digest>::failure(checked.error());
    const auto text = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}

core::Result<IkFkSwitchProposal> propose_ik_fk_switch(
    const IkFkAuthoring& authored, const character::RigDefinition& rig,
    const kinematic::KinematicGraph& graph, const kinematic::JointLimitSet& limits,
    IkFkMode target, const LocalPose& locals, const std::optional<IkControlState>& ik) {
    using Result = core::Result<IkFkSwitchProposal>;
    if (auto checked = authored.validate_against(rig, graph, limits); !checked)
        return Result::failure(checked.error());
    if (!(rig.rest_basis == character::CoordinatePolicy{}))
        return Result::failure(invalid("IK/FK proposals currently require the default coordinate policy"));
    auto parent = graph.find(authored.chain.root)->parent;
    while (parent) {
        if (locals.contains(*parent))
            return Result::failure(invalid("IK/FK proposals do not support posed ancestors outside the chain"));
        parent = graph.find(*parent)->parent;
    }
    if ((target != IkFkMode::fk && target != IkFkMode::ik) ||
        (target == IkFkMode::fk && !ik) || (target == IkFkMode::ik && ik))
        return Result::failure(invalid("switch requires explicit IK inputs exactly when switching to FK"));
    if (ik && ik->pole_vector && !authored.ik_pole)
        return Result::failure(invalid("a pole direction requires an authored pole control"));
    const IkFkMatchRequest request{authored.chain, authored.budget, authored.max_position_jump};
    auto match = target == IkFkMode::ik
        ? match_ik_to_fk(graph, locals, &limits, request)
        : match_fk_to_ik(graph, locals, &limits, *ik, request);
    if (!match) return Result::failure(match.error());
    if (match.value().ik.pole_vector && !authored.ik_pole)
        return Result::failure(invalid("matched pole direction has no authored pole control"));
    return Result::success({target, authored.mode_control, authored.mode_axis,
        target == IkFkMode::ik ? authored.ik_mode_value : authored.fk_mode_value,
        std::move(match.value())});
}
} // namespace carto::pose
