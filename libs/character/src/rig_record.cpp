#include <carto/character/rig_record.hpp>

#include <iomanip>
#include <locale>
#include <span>
#include <sstream>
#include <utility>

namespace carto::character {
namespace {
core::Diagnostic invalid(const char* message) {
    return {core::ErrorCode::validation_failed, message};
}
bool token(std::istream& in, std::string_view expected) {
    std::string actual;
    return static_cast<bool>(in >> actual) && actual == expected;
}
bool count(std::istream& in, std::size_t max, std::size_t& result) {
    std::uint64_t n = 0;
    if (!(in >> n) || n > max) return false;
    result = static_cast<std::size_t>(n);
    return true;
}
void write_transform(std::ostream& out, const core::Transform& t) {
    out << t.translation.x << ' ' << t.translation.y << ' ' << t.translation.z << ' '
        << t.rotation.x << ' ' << t.rotation.y << ' ' << t.rotation.z << ' ' << t.rotation.w << ' '
        << t.scale.x << ' ' << t.scale.y << ' ' << t.scale.z;
}
bool read_transform(std::istream& in, core::Transform& t) {
    return static_cast<bool>(in >> t.translation.x >> t.translation.y >> t.translation.z >>
        t.rotation.x >> t.rotation.y >> t.rotation.z >> t.rotation.w >>
        t.scale.x >> t.scale.y >> t.scale.z);
}
} // namespace

core::Result<std::string> serialize_rig_definition(const RigDefinition& rig) {
    if (auto valid = rig.validate(); !valid)
        return core::Result<std::string>::failure(valid.error());
    // CoordinatePolicy::validate currently does not check winding; the wire
    // format must reject unknown enum values before emitting an unreadable rig.
    if (rig.rest_basis.winding != WindingOrder::counter_clockwise &&
        rig.rest_basis.winding != WindingOrder::clockwise)
        return core::Result<std::string>::failure(invalid("rig winding is invalid"));
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "CARTO_RIG_DEFINITION 1\n"
        << "IDENTITY " << rig.id.value << ' ' << rig.kinematic_graph_digest.hex() << ' '
        << rig.joint_limits.digest.hex() << '\n';
    const auto& p = rig.rest_basis;
    out << "BASIS " << static_cast<unsigned>(p.handedness) << ' ' << static_cast<unsigned>(p.up)
        << ' ' << static_cast<unsigned>(p.forward) << ' ' << p.meters_per_unit << ' '
        << static_cast<unsigned>(p.winding) << ' ' << static_cast<unsigned>(p.rotation) << ' '
        << static_cast<unsigned>(p.inverse_bind) << '\n';
    out << "TAGS " << rig.semantic_tags.size() << '\n';
    for (const auto& tag : rig.semantic_tags) out << "TAG " << std::quoted(tag) << '\n';
    out << "CONTROLS " << rig.control_graph.nodes().size() << '\n';
    for (const auto& [id, c] : rig.control_graph.nodes()) {
        out << "CONTROL " << id.value << ' ' << static_cast<unsigned>(c.shape) << ' ';
        write_transform(out, c.transform);
        out << ' ' << static_cast<unsigned>(c.allowed_axes);
        for (std::size_t i = 0; i < kAxisCount; ++i)
            out << ' ' << c.axis_minimum[i] << ' ' << c.axis_maximum[i];
        out << ' ' << c.affected_joints.size();
        for (auto joint : c.affected_joints) out << ' ' << joint.value;
        out << ' ' << (c.follow_space ? c.follow_space->value : 0U) << ' '
            << std::quoted(c.control_set) << '\n';
    }
    out << "EDGES " << rig.control_graph.edges().size() << '\n';
    for (const auto& e : rig.control_graph.edges())
        out << "EDGE " << e.driver.value << ' ' << e.follower.value << '\n';
    out << "SYMMETRY " << rig.symmetry.size() << '\n';
    for (const auto& s : rig.symmetry)
        out << "PAIR " << s.left_joint.value << ' ' << s.right_joint.value << '\n';
    out << "END\n";
    auto result = out.str();
    if (result.size() > kMaxRigDefinitionBytes)
        return core::Result<std::string>::failure(invalid("rig definition exceeds byte limit"));
    return core::Result<std::string>::success(std::move(result));
}

core::Result<RigDefinition> deserialize_rig_definition(std::string_view text) {
    const auto fail = [] { return core::Result<RigDefinition>::failure(
        invalid("rig definition is malformed, noncanonical, or exceeds a bound")); };
    if (text.empty() || text.size() > kMaxRigDefinitionBytes) return fail();
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    unsigned version = 0;
    RigDefinition rig;
    std::string graph, limits;
    if (!token(in, "CARTO_RIG_DEFINITION") || !(in >> version) || version != 1 ||
        !token(in, "IDENTITY") || !(in >> rig.id.value >> graph >> limits)) return fail();
    auto graph_digest = assets::Sha256Digest::from_hex(graph);
    auto limit_digest = assets::Sha256Digest::from_hex(limits);
    if (!graph_digest || !limit_digest) return fail();
    rig.kinematic_graph_digest = graph_digest.value();
    rig.joint_limits.digest = limit_digest.value();
    unsigned hand, up, forward, winding, rotation, bind;
    if (!token(in, "BASIS") || !(in >> hand >> up >> forward >> rig.rest_basis.meters_per_unit
            >> winding >> rotation >> bind) || hand > 1 || up > 5 || forward > 5 ||
        winding > 1 || rotation > 0 || bind > 1) return fail();
    rig.rest_basis.handedness = static_cast<Handedness>(hand);
    rig.rest_basis.up = static_cast<Axis>(up);
    rig.rest_basis.forward = static_cast<Axis>(forward);
    rig.rest_basis.winding = static_cast<WindingOrder>(winding);
    rig.rest_basis.rotation = static_cast<RotationRepresentation>(rotation);
    rig.rest_basis.inverse_bind = static_cast<InverseBindSpace>(bind);
    std::size_t n = 0;
    if (!token(in, "TAGS") || !count(in, RigDefinition::kMaxSemanticTags, n)) return fail();
    for (std::size_t i = 0; i < n; ++i) {
        std::string tag;
        if (!token(in, "TAG") || !(in >> std::quoted(tag))) return fail();
        rig.semantic_tags.push_back(std::move(tag));
    }
    if (!token(in, "CONTROLS") || !count(in, ControlGraph::kMaxControls, n)) return fail();
    for (std::size_t i = 0; i < n; ++i) {
        ControlDefinition c;
        unsigned shape, axes;
        if (!token(in, "CONTROL") || !(in >> c.id.value >> shape) || shape > 3 ||
            !read_transform(in, c.transform) || !(in >> axes) || axes > 255) return fail();
        c.shape = static_cast<ControlShape>(shape);
        c.allowed_axes = static_cast<std::uint8_t>(axes);
        for (std::size_t axis = 0; axis < kAxisCount; ++axis)
            if (!(in >> c.axis_minimum[axis] >> c.axis_maximum[axis])) return fail();
        std::size_t joints = 0;
        if (!count(in, ControlGraph::kMaxAffectedJointsPerControl, joints)) return fail();
        for (std::size_t j = 0; j < joints; ++j) {
            kinematic::JointId joint;
            if (!(in >> joint.value)) return fail();
            c.affected_joints.push_back(joint);
        }
        std::uint64_t follow = 0;
        if (!(in >> follow >> std::quoted(c.control_set))) return fail();
        if (follow) c.follow_space = ControlId{follow};
        if (!rig.control_graph.insert(std::move(c))) return fail();
    }
    if (!token(in, "EDGES") || !count(in, ControlGraph::kMaxEdges, n)) return fail();
    for (std::size_t i = 0; i < n; ++i) {
        ControlEdge edge;
        if (!token(in, "EDGE") || !(in >> edge.driver.value >> edge.follower.value) ||
            !rig.control_graph.insert_edge(edge)) return fail();
    }
    if (!token(in, "SYMMETRY") || !count(in, RigDefinition::kMaxSymmetryPairs, n)) return fail();
    for (std::size_t i = 0; i < n; ++i) {
        SymmetryPair pair;
        if (!token(in, "PAIR") || !(in >> pair.left_joint.value >> pair.right_joint.value)) return fail();
        rig.symmetry.push_back(pair);
    }
    if (!token(in, "END") || !rig.validate()) return fail();
    const auto canonical = serialize_rig_definition(rig);
    if (!canonical || canonical.value() != text) return fail();
    return core::Result<RigDefinition>::success(std::move(rig));
}

core::Result<assets::Sha256Digest> rig_definition_digest(const RigDefinition& rig) {
    auto encoded = serialize_rig_definition(rig);
    if (!encoded) return core::Result<assets::Sha256Digest>::failure(encoded.error());
    const auto& text = encoded.value();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}
} // namespace carto::character
