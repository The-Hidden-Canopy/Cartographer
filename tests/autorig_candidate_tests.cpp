#include <carto/character/autorig_candidates.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace carto;
using namespace carto::character;
using namespace carto::geometry;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

AutorigConstraintProvenance provenance(std::string id, std::string rationale = "reviewed anchor") {
    return {std::move(id), "artist-1", std::move(rationale), "",
        AutorigConstraintOrigin::human_authored};
}

struct Fixture final {
    EditableMesh mesh;
    AutorigAnalysisRequest request;
    AutorigConstraintSet constraints;
};

Fixture fixture() {
    Fixture value;
    const auto v1 = value.mesh.add_vertex({0.0, 0.0, 0.0});
    const auto v2 = value.mesh.add_vertex({0.0, 2.0, 0.0});
    const auto v3 = value.mesh.add_vertex({0.1, 0.1, 0.0});
    const auto v4 = value.mesh.add_vertex({1.0, 2.0, 0.0});
    require(v1 && v2 && v3 && v4, "candidate source vertices should be admitted");
    require(static_cast<bool>(value.mesh.add_face({v1.value(), v2.value(), v4.value()})) &&
            static_cast<bool>(value.mesh.add_face({v1.value(), v4.value(), v3.value()})),
        "candidate source faces should be admitted");
    value.request = {MeshAssetRef{51U, digest("candidate-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {v1.value()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "candidate source analysis should pass");
    value.constraints = {
        analysis.value().source, analysis.value().mesh_revision,
        analysis.value().analysis_digest,
        {
            {"part.root", "role.root", std::nullopt, AutorigPartSide::center},
            {"part.spine", "role.segment", std::string("part.root"), AutorigPartSide::center},
            {"part.branch", "role.branch", std::string("part.spine"), AutorigPartSide::center},
            {"part.unanchored", "role.unresolved", std::string("part.root"), AutorigPartSide::center},
        },
        {
            {"anchor.root", "part.root", "role.root", AutorigPartSide::center,
                v1.value(), provenance("assert.root")},
            {"anchor.spine", "part.spine", "role.segment", AutorigPartSide::center,
                v2.value(), provenance("assert.spine")},
            {"anchor.branch.near", "part.branch", "role.branch.near", AutorigPartSide::center,
                v3.value(), provenance("assert.branch.near")},
            {"anchor.branch.far", "part.branch", "role.branch.far", AutorigPartSide::center,
                v4.value(), provenance("assert.branch.far")},
        },
        {},
        {},
    };
    return value;
}

Fixture mechanical_port_fixture(bool disconnected_components = true) {
    Fixture value;
    const auto root_port = value.mesh.add_vertex({0.0, 0.0, 0.0});
    const auto root_a = value.mesh.add_vertex({0.0, 0.3, 0.0});
    const auto root_b = value.mesh.add_vertex({0.3, 0.0, 0.0});
    const auto child_port = value.mesh.add_vertex({1.0, 0.0, 0.0});
    const auto child_a = value.mesh.add_vertex({1.0, 0.3, 0.0});
    const auto child_b = value.mesh.add_vertex({1.3, 0.0, 0.0});
    require(root_port && root_a && root_b && child_port && child_a && child_b,
        "mechanical port source vertices should be admitted");
    require(static_cast<bool>(value.mesh.add_face({root_port.value(), root_a.value(), root_b.value()})),
        "mechanical parent rigid component should be admitted");
    if (disconnected_components) {
        require(static_cast<bool>(value.mesh.add_face({child_port.value(), child_a.value(), child_b.value()})),
            "mechanical child rigid component should be admitted");
    } else {
        require(static_cast<bool>(value.mesh.add_face({root_port.value(), child_port.value(), root_a.value()})),
            "connected mechanical boundary fixture should be admitted");
    }
    value.request = {MeshAssetRef{54U, digest("mechanical-port-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {root_port.value()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "mechanical source analysis should pass");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    value.constraints.parts = {
        {"machine.base", "base", std::nullopt, AutorigPartSide::center},
        {"machine.arm", "link", std::string("machine.base"), AutorigPartSide::center},
    };
    value.constraints.landmarks = {
        {"port.base", "machine.base", "mount", AutorigPartSide::center,
            root_port.value(), provenance("assert.port.base", "reviewed base port")},
        {"port.arm", "machine.arm", "pivot", AutorigPartSide::center,
            child_port.value(), provenance("assert.port.arm", "reviewed arm port")},
    };
    value.constraints.allowed_regions = {
        {"region.base", "machine.base", {root_port.value()},
            provenance("assert.region.base", "reviewed base port region")},
        {"region.arm", "machine.arm", {child_port.value()},
            provenance("assert.region.arm", "reviewed arm port region")},
    };
    value.constraints.mechanical_ports = {
        {"port-hint.arm-hinge", "machine.base", "machine.arm", "port.base", "port.arm",
            kinematic::JointKind::hinge, {0.0, 0.0, 1.0}, -1.2, 1.2,
            provenance("assert.mechanical.arm", "reviewed rigid hinge port")},
    };
    return value;
}

Fixture six_limb_mechanical_fixture() {
    Fixture value;
    std::vector<VertexId> ports;
    for (std::size_t component = 0U; component < 7U; ++component) {
        const double angle = 6.2831853071795864769 *
            static_cast<double>(component) / 7.0;
        const core::Vec3d center = component == 0U
            ? core::Vec3d{0.0, 0.0, 0.0}
            : core::Vec3d{1.0, 1.5 * std::cos(angle), 1.5 * std::sin(angle)};
        const auto port = value.mesh.add_vertex(center);
        const auto a = value.mesh.add_vertex({center.x, center.y + 0.12, center.z});
        const auto b = value.mesh.add_vertex({center.x, center.y, center.z + 0.12});
        require(port && a && b,
            "each authored rigid part should have a bounded mesh component");
        require(static_cast<bool>(value.mesh.add_face({port.value(), a.value(), b.value()})),
            "each authored rigid part should be a valid mesh component");
        ports.push_back(port.value());
    }
    value.request = {MeshAssetRef{55U, digest("six-limb-mechanical-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {ports.front()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "six-limb mechanical analysis should pass");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    value.constraints.parts.push_back({
        "machine.base", "base", std::nullopt, AutorigPartSide::center});
    value.constraints.landmarks.push_back({
        "port.base", "machine.base", "mount", AutorigPartSide::center,
        ports.front(), provenance("assert.base", "reviewed central mount")});
    value.constraints.allowed_regions.push_back({
        "region.base", "machine.base", {ports.front()},
        provenance("assert.region.base", "reviewed central mount region")});
    for (std::size_t limb = 0U; limb < 6U; ++limb) {
        const auto part_id = "machine.limb." + std::to_string(limb + 1U);
        const auto landmark_id = "port.limb." + std::to_string(limb + 1U);
        value.constraints.parts.push_back({
            part_id, "link", std::string("machine.base"),
            limb % 2U == 0U ? AutorigPartSide::left : AutorigPartSide::right});
        value.constraints.landmarks.push_back({
            landmark_id, part_id, "pivot", value.constraints.parts.back().side,
            ports[limb + 1U], provenance("assert." + landmark_id, "reviewed limb pivot")});
        value.constraints.allowed_regions.push_back({
            "region." + part_id, part_id, {ports[limb + 1U]},
            provenance("assert.region." + part_id, "reviewed limb port region")});
        value.constraints.mechanical_ports.push_back({
            "hint." + part_id, "machine.base", part_id, "port.base", landmark_id,
            kinematic::JointKind::hinge, {0.0, 0.0, 1.0}, -0.75, 0.75,
            provenance("assert.hint." + part_id, "reviewed six-limb rigid hinge")});
    }
    return value;
}

Fixture bilateral_fixture() {
    Fixture value;
    const auto root = value.mesh.add_vertex({0.0, 0.0, 0.0});
    const auto support = value.mesh.add_vertex({0.0, 0.0, -1.0});
    const auto left = value.mesh.add_vertex({-1.0, 1.0, 0.0});
    const auto right_exact = value.mesh.add_vertex({1.0, 1.0, 0.0});
    const auto right_offset = value.mesh.add_vertex({1.2, 1.0, 0.0});
    require(root && support && left && right_exact && right_offset,
        "bilateral candidate fixture vertices should be admitted");
    require(static_cast<bool>(value.mesh.add_face({root.value(), left.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({left.value(), right_exact.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({right_exact.value(), right_offset.value(), support.value()})),
        "bilateral candidate fixture faces should be admitted");
    value.request = {MeshAssetRef{52U, digest("bilateral-candidate-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {root.value()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "bilateral source analysis should pass");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    value.constraints.parts = {
        {"part.root", "root", std::nullopt, AutorigPartSide::center},
        {"part.left", "limb", std::string("part.root"), AutorigPartSide::left},
        {"part.right", "limb", std::string("part.root"), AutorigPartSide::right},
    };
    value.constraints.landmarks = {
        {"anchor.root", "part.root", "root", AutorigPartSide::center,
            root.value(), provenance("assert.root")},
        {"anchor.left", "part.left", "limb.left", AutorigPartSide::left,
            left.value(), provenance("assert.left")},
        // The intentionally reversed lexical order makes the no-symmetry
        // ranking prefer the offset alternative on its stable tie break.
        {"z.anchor.right.exact", "part.right", "limb.exact", AutorigPartSide::right,
            right_exact.value(), provenance("assert.right.exact")},
        {"a.anchor.right.offset", "part.right", "limb.offset", AutorigPartSide::right,
            right_offset.value(), provenance("assert.right.offset")},
    };
    value.constraints.allowed_regions = {
        {"region.root", "part.root", {root.value()}, provenance("region.root")},
        {"region.left", "part.left", {left.value()}, provenance("region.left")},
        {"region.right", "part.right", {right_exact.value(), right_offset.value()},
            provenance("region.right")},
    };
    return value;
}

Fixture bend_fixture() {
    Fixture value;
    const auto support = value.mesh.add_vertex({0.0, 0.0, -1.0});
    const auto root = value.mesh.add_vertex({0.0, 0.0, 0.0});
    const auto segment1 = value.mesh.add_vertex({1.0, 0.0, 0.0});
    const auto segment2_straight = value.mesh.add_vertex({2.0, 0.0, 0.0});
    const auto segment2_bent = value.mesh.add_vertex({2.0, 1.0, 0.0});
    const auto segment3 = value.mesh.add_vertex({3.0, 0.0, 0.0});
    require(support && root && segment1 && segment2_straight && segment2_bent && segment3,
        "bend-arc fixture vertices should be admitted");
    require(static_cast<bool>(value.mesh.add_face({root.value(), segment1.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({segment1.value(), segment2_straight.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({segment2_straight.value(), segment2_bent.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({segment2_bent.value(), segment3.value(), support.value()})),
        "bend-arc fixture faces should be admitted");
    value.request = {MeshAssetRef{53U, digest("bend-arc-candidate-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {root.value()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "bend-arc source analysis should pass");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    value.constraints.parts = {
        {"part.root", "root", std::nullopt, AutorigPartSide::center},
        {"part.segment.1", "segment", std::string("part.root"), AutorigPartSide::center},
        {"part.segment.2", "segment", std::string("part.segment.1"), AutorigPartSide::center},
        {"part.segment.3", "segment", std::string("part.segment.2"), AutorigPartSide::center},
    };
    value.constraints.landmarks = {
        {"anchor.root", "part.root", "root", AutorigPartSide::center,
            root.value(), provenance("assert.root")},
        {"anchor.segment.1", "part.segment.1", "segment", AutorigPartSide::center,
            segment1.value(), provenance("assert.segment.1")},
        {"a.anchor.segment.2.bent", "part.segment.2", "segment.bent", AutorigPartSide::center,
            segment2_bent.value(), provenance("assert.segment.2.bent")},
        {"z.anchor.segment.2.straight", "part.segment.2", "segment.straight", AutorigPartSide::center,
            segment2_straight.value(), provenance("assert.segment.2.straight")},
        {"anchor.segment.3", "part.segment.3", "segment", AutorigPartSide::center,
            segment3.value(), provenance("assert.segment.3")},
    };
    value.constraints.allowed_regions = {
        {"region.root", "part.root", {root.value()}, provenance("region.root")},
        {"region.segment.1", "part.segment.1", {segment1.value()}, provenance("region.segment.1")},
        {"region.segment.2", "part.segment.2", {segment2_straight.value(), segment2_bent.value()},
            provenance("region.segment.2")},
        {"region.segment.3", "part.segment.3", {segment3.value()}, provenance("region.segment.3")},
    };
    return value;
}

Fixture overlap_fixture() {
    Fixture value;
    const auto root = value.mesh.add_vertex({0.0, 0.0, 0.0});
    const auto branch_a = value.mesh.add_vertex({0.0, 2.0, 0.0});
    const auto branch_b = value.mesh.add_vertex({2.0, 0.0, 0.0});
    const auto crossing = value.mesh.add_vertex({2.0, 2.0, 0.0});
    const auto clear = value.mesh.add_vertex({-1.0, 2.0, 0.0});
    const auto support = value.mesh.add_vertex({0.0, 0.0, -1.0});
    require(root && branch_a && branch_b && crossing && clear && support,
        "self-overlap fixture vertices should be admitted");
    require(static_cast<bool>(value.mesh.add_face({root.value(), branch_a.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({branch_a.value(), branch_b.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({branch_b.value(), crossing.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({crossing.value(), clear.value(), support.value()})) &&
            static_cast<bool>(value.mesh.add_face({clear.value(), root.value(), support.value()})),
        "self-overlap fixture source faces should be admitted");
    value.request = {MeshAssetRef{56U, digest("skeleton-overlap-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {root.value()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "self-overlap source analysis should pass");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    value.constraints.parts = {
        {"part.root", "root", std::nullopt, AutorigPartSide::center},
        {"part.branch.a", "limb", std::string("part.root"), AutorigPartSide::center},
        {"part.branch.b", "segment", std::string("part.branch.a"), AutorigPartSide::center},
        {"part.branch.c", "limb", std::string("part.root"), AutorigPartSide::center},
        {"part.unanchored", "unresolved", std::string("part.root"), AutorigPartSide::center},
    };
    value.constraints.landmarks = {
        {"anchor.root", "part.root", "root", AutorigPartSide::center,
            root.value(), provenance("assert.root")},
        {"anchor.branch.a", "part.branch.a", "limb", AutorigPartSide::center,
            branch_a.value(), provenance("assert.branch.a")},
        {"anchor.branch.b", "part.branch.b", "segment", AutorigPartSide::center,
            branch_b.value(), provenance("assert.branch.b")},
        {"a.anchor.branch.c.crossing", "part.branch.c", "limb.crossing", AutorigPartSide::center,
            crossing.value(), provenance("assert.branch.c.crossing")},
        {"z.anchor.branch.c.clear", "part.branch.c", "limb.clear", AutorigPartSide::center,
            clear.value(), provenance("assert.branch.c.clear")},
    };
    value.constraints.allowed_regions = {
        {"region.root", "part.root", {root.value()}, provenance("assert.region.root")},
        {"region.branch.a", "part.branch.a", {branch_a.value()}, provenance("assert.region.branch.a")},
        {"region.branch.b", "part.branch.b", {branch_b.value()}, provenance("assert.region.branch.b")},
        {"region.branch.c", "part.branch.c", {crossing.value(), clear.value()},
            provenance("assert.region.branch.c")},
    };
    return value;
}

Fixture source_surface_crossing_fixture() {
    auto value = overlap_fixture();
    const auto panel_a = value.mesh.add_vertex({1.0, 0.7, -0.5});
    const auto panel_b = value.mesh.add_vertex({1.0, 1.3, -0.5});
    const auto panel_c = value.mesh.add_vertex({1.0, 1.0, 0.5});
    require(panel_a && panel_b && panel_c &&
            static_cast<bool>(value.mesh.add_face(
                {panel_a.value(), panel_b.value(), panel_c.value()})),
        "surface-crossing fixture should add a bounded source triangle");
    value.request.expected_mesh_revision = value.mesh.revision();
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis),
        "surface-crossing fixture analysis should bind the modified source");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    return value;
}

Fixture mechanical_fixture() {
    Fixture value;
    const auto base_port = value.mesh.add_vertex({0.0, 0.0, 0.0});
    const auto base_b = value.mesh.add_vertex({1.0, 0.0, 0.0});
    const auto base_c = value.mesh.add_vertex({0.0, 1.0, 0.0});
    const auto elbow_port = value.mesh.add_vertex({3.0, 0.0, 0.0});
    const auto elbow_b = value.mesh.add_vertex({4.0, 0.0, 0.0});
    const auto elbow_c = value.mesh.add_vertex({3.0, 1.0, 0.0});
    const auto tool_port = value.mesh.add_vertex({6.0, 0.0, 0.0});
    const auto tool_b = value.mesh.add_vertex({7.0, 0.0, 0.0});
    const auto tool_c = value.mesh.add_vertex({6.0, 1.0, 0.0});
    require(base_port && base_b && base_c && elbow_port && elbow_b && elbow_c &&
            tool_port && tool_b && tool_c,
        "mechanical rigid-component vertices should be admitted");
    require(static_cast<bool>(value.mesh.add_face({base_port.value(), base_b.value(), base_c.value()})) &&
            static_cast<bool>(value.mesh.add_face({elbow_port.value(), elbow_b.value(), elbow_c.value()})) &&
            static_cast<bool>(value.mesh.add_face({tool_port.value(), tool_b.value(), tool_c.value()})),
        "mechanical rigid-component faces should be admitted");
    value.request = {MeshAssetRef{54U, digest("mechanical-port-candidate-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7, {base_port.value()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis), "mechanical source analysis should pass");
    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    value.constraints.parts = {
        {"part.base", "rigid_base", std::nullopt, AutorigPartSide::center},
        {"part.elbow", "rigid_link", std::string("part.base"), AutorigPartSide::center},
        {"part.tool", "rigid_tool", std::string("part.elbow"), AutorigPartSide::center},
    };
    value.constraints.landmarks = {
        {"landmark.base.port", "part.base", "base_port", AutorigPartSide::center,
            base_port.value(), provenance("assert.landmark.base")},
        {"landmark.elbow.port", "part.elbow", "elbow_port", AutorigPartSide::center,
            elbow_port.value(), provenance("assert.landmark.elbow")},
        {"landmark.tool.port", "part.tool", "tool_port", AutorigPartSide::center,
            tool_port.value(), provenance("assert.landmark.tool")},
    };
    value.constraints.allowed_regions = {
        {"region.base", "part.base", {base_port.value()}, provenance("assert.region.base")},
        {"region.elbow", "part.elbow", {elbow_port.value()}, provenance("assert.region.elbow")},
        {"region.tool", "part.tool", {tool_port.value()}, provenance("assert.region.tool")},
    };
    value.constraints.mechanical_ports = {
        {"port.hinge", "part.base", "part.elbow",
            "landmark.base.port", "landmark.elbow.port", kinematic::JointKind::hinge,
            {0.0, 0.0, 1.0}, -1.2, 1.2,
            provenance("assert.port.hinge", "reviewed hinge axis and limits")},
        {"port.slider", "part.elbow", "part.tool",
            "landmark.elbow.port", "landmark.tool.port", kinematic::JointKind::slider,
            {1.0, 0.0, 0.0}, 0.0, 2.0,
            provenance("assert.port.slider", "reviewed slider axis and limits")},
    };
    return value;
}

struct AuthoredPart final {
    std::string id;
    std::string role;
    std::optional<std::string> parent;
    AutorigPartSide side = AutorigPartSide::unspecified;
    std::size_t anchor_alternatives = 1U;
};

Fixture archetype_fixture(
    std::string_view archetype,
    const std::vector<AuthoredPart>& parts,
    std::size_t asset_variant = 0U) {
    require(parts.size() >= 3U && !parts.front().parent.has_value(),
        "each archetype fixture needs a root and enough authored parts");
    const std::size_t anchor_count = [&parts] {
        std::size_t count = 0U;
        for (const auto& part : parts) count += part.anchor_alternatives;
        return count;
    }();
    require(anchor_count >= 3U,
        "each archetype fixture needs enough anchors for a connected mesh");

    Fixture value;
    const auto variant_scale = static_cast<double>(asset_variant);
    const auto support = value.mesh.add_vertex({0.0, 0.0, -0.25 - 0.1 * variant_scale});
    require(static_cast<bool>(support), "archetype support vertex should be admitted");
    std::vector<std::vector<VertexId>> anchors_by_part;
    anchors_by_part.reserve(parts.size());
    std::vector<VertexId> ordered_anchors;
    ordered_anchors.reserve(anchor_count);
    std::size_t ordinal = 0U;
    for (const auto& part : parts) {
        require(part.anchor_alternatives > 0U,
            "archetype part must have at least one explicitly authored anchor");
        std::vector<VertexId> anchors;
        anchors.reserve(part.anchor_alternatives);
        for (std::size_t alternative = 0U;
             alternative < part.anchor_alternatives; ++alternative) {
            const double angle = 6.2831853071795864769 *
                (static_cast<double>(ordinal) + 0.5 + 0.13 * variant_scale) /
                static_cast<double>(anchor_count);
            const double radius = 1.0 + 0.04 * static_cast<double>(ordinal) +
                0.03 * variant_scale;
            const double side_extent = 0.6 + 0.05 * variant_scale;
            const double x = part.side == AutorigPartSide::left ? -side_extent :
                part.side == AutorigPartSide::right ? side_extent : 0.0;
            const auto vertex = value.mesh.add_vertex({
                x,
                radius * std::cos(angle),
                radius * std::sin(angle)});
            require(static_cast<bool>(vertex),
                "authored archetype landmark vertex should be admitted");
            anchors.push_back(vertex.value());
            ordered_anchors.push_back(vertex.value());
            ++ordinal;
        }
        anchors_by_part.push_back(std::move(anchors));
    }
    for (std::size_t index = 0U; index < ordered_anchors.size(); ++index) {
        const auto next = (index + 1U) % ordered_anchors.size();
        require(static_cast<bool>(value.mesh.add_face({
            support.value(), ordered_anchors[index], ordered_anchors[next]})),
            "archetype landmarks should be connected by valid source faces");
    }

    const auto source_name = std::string("autorig-") + std::string(archetype) +
        "-asset-" + std::to_string(asset_variant + 1U);
    value.request = {
        MeshAssetRef{100U + static_cast<std::uint64_t>(archetype.size()) +
                1000U * static_cast<std::uint64_t>(asset_variant),
            digest(source_name + "-source")},
        value.mesh.revision(), AutorigAxis::x, 0.0, 1.0e-7,
        {anchors_by_part.front().front()}, {}};
    const auto analysis = analyze_autorig_features(value.mesh, value.request);
    require(static_cast<bool>(analysis),
        "authored archetype mesh should produce a source-bound feature analysis");

    value.constraints.source = analysis.value().source;
    value.constraints.mesh_revision = analysis.value().mesh_revision;
    value.constraints.analysis_digest = analysis.value().analysis_digest;
    for (std::size_t part_index = 0U; part_index < parts.size(); ++part_index) {
        const auto& part = parts[part_index];
        value.constraints.parts.push_back({
            part.id, part.role, part.parent, part.side});
        value.constraints.allowed_regions.push_back({
            "region." + part.id, part.id, anchors_by_part[part_index],
            provenance("assert.region." + part.id, "reviewed joint placement region")});
        for (std::size_t alternative = 0U;
             alternative < anchors_by_part[part_index].size(); ++alternative) {
            const auto landmark_id = "anchor." + part.id + "." +
                std::to_string(alternative + 1U);
            const auto landmark_role = anchors_by_part[part_index].size() == 1U
                ? part.role : part.role + ".anchor_" +
                    std::to_string(alternative + 1U);
            value.constraints.landmarks.push_back({
                landmark_id, part.id, landmark_role, part.side,
                anchors_by_part[part_index][alternative],
                provenance("assert." + part.id + "." +
                    std::to_string(alternative + 1U))});
        }
    }
    return value;
}

void authored_candidate_receipts_cover_four_structure_classes() {
    struct Archetype final {
        std::string_view name;
        std::vector<AuthoredPart> parts;
        std::string_view alternative_part;
    };
    const std::vector<Archetype> archetypes = {
        {"humanoid", {
            {"human.pelvis", "pelvis", std::nullopt, AutorigPartSide::center},
            {"human.spine", "spine", "human.pelvis", AutorigPartSide::center},
            {"human.head", "head", "human.spine", AutorigPartSide::center},
            {"human.arm.left", "arm", "human.spine", AutorigPartSide::left},
            {"human.hand.left", "hand", "human.arm.left", AutorigPartSide::left, 2U},
            {"human.arm.right", "arm", "human.spine", AutorigPartSide::right},
            {"human.hand.right", "hand", "human.arm.right", AutorigPartSide::right},
            {"human.leg.left", "leg", "human.pelvis", AutorigPartSide::left},
            {"human.leg.right", "leg", "human.pelvis", AutorigPartSide::right},
        }, "human.hand.left"},
        {"quadruped", {
            {"quad.torso", "torso", std::nullopt, AutorigPartSide::center},
            {"quad.neck", "neck", "quad.torso", AutorigPartSide::center},
            {"quad.head", "head", "quad.neck", AutorigPartSide::center},
            {"quad.front.left", "forelimb", "quad.torso", AutorigPartSide::left},
            {"quad.front.right", "forelimb", "quad.torso", AutorigPartSide::right},
            {"quad.rear.left", "hindlimb", "quad.torso", AutorigPartSide::left},
            {"quad.rear.right", "hindlimb", "quad.torso", AutorigPartSide::right},
            {"quad.tail", "tail", "quad.torso", AutorigPartSide::center, 2U},
        }, "quad.tail"},
        {"creature", {
            {"creature.core", "core", std::nullopt, AutorigPartSide::center},
            {"creature.segment.a", "segment", "creature.core", AutorigPartSide::center},
            {"creature.segment.b", "segment", "creature.segment.a", AutorigPartSide::center},
            {"creature.appendage.1", "appendage", "creature.segment.b", AutorigPartSide::left},
            {"creature.appendage.2", "appendage", "creature.segment.b", AutorigPartSide::right},
            {"creature.appendage.3", "appendage", "creature.segment.a", AutorigPartSide::left},
            {"creature.appendage.4", "appendage", "creature.segment.a", AutorigPartSide::right},
            {"creature.appendage.5", "appendage", "creature.core", AutorigPartSide::left},
            {"creature.appendage.6", "appendage", "creature.core", AutorigPartSide::right},
            {"creature.crest", "crest", "creature.core", AutorigPartSide::center, 2U},
        }, "creature.crest"},
        {"mechanical", {
            {"machine.chassis", "chassis", std::nullopt, AutorigPartSide::center},
            {"machine.mount", "mount", "machine.chassis", AutorigPartSide::center},
            {"machine.arm.left", "link", "machine.mount", AutorigPartSide::left},
            {"machine.tool.left", "end_effector", "machine.arm.left", AutorigPartSide::left},
            {"machine.arm.right", "link", "machine.mount", AutorigPartSide::right},
            {"machine.tool.right", "end_effector", "machine.arm.right", AutorigPartSide::right, 2U},
            {"machine.axle.front", "axle", "machine.chassis", AutorigPartSide::center},
            {"machine.axle.rear", "axle", "machine.chassis", AutorigPartSide::center},
        }, "machine.tool.right"},
    };

    std::vector<assets::Sha256Digest> receipt_digests;
    for (const auto& archetype : archetypes) {
        std::optional<assets::Sha256Digest> first_variant_geometry;
        std::optional<assets::Sha256Digest> first_variant_source;
        for (std::size_t asset_variant = 0U; asset_variant < 2U; ++asset_variant) {
        auto value = archetype_fixture(archetype.name, archetype.parts, asset_variant);
        const auto source_revision = value.mesh.revision();
        const auto source_vertex_count = value.mesh.vertex_count();
        const auto candidates = generate_autorig_candidates(
            value.mesh, value.request, value.constraints);
        if (!candidates) {
            std::cerr << "FAIL: authored " << archetype.name
                << " candidate receipt: " << candidates.error().message << '\n';
            for (const auto& context : candidates.error().context)
                std::cerr << "  " << context << '\n';
            std::exit(EXIT_FAILURE);
        }
        const auto expected_analysis = analyze_autorig_features(value.mesh, value.request);
        require(static_cast<bool>(expected_analysis),
            "each archetype should retain its explicit source analysis");
        const std::size_t expected_candidate_count = archetype.alternative_part.empty()
            ? 1U : 2U;
        if (candidates.value().ranked_candidates.size() != expected_candidate_count) {
            std::cerr << "FAIL: authored " << archetype.name << " expected "
                << expected_candidate_count << " ranked candidates, received "
                << candidates.value().ranked_candidates.size() << '\n';
            std::exit(EXIT_FAILURE);
        }
        require(candidates.value().source == value.request.source &&
                candidates.value().mesh_revision == source_revision &&
                candidates.value().mesh_geometry_digest ==
                    expected_analysis.value().mesh_geometry_digest &&
                candidates.value().analysis_digest == expected_analysis.value().analysis_digest,
            "candidate receipt should bind the exact analyzed source and revision");
        if (asset_variant == 0U) {
            first_variant_geometry = candidates.value().mesh_geometry_digest;
            first_variant_source = candidates.value().source.content_digest;
        } else {
            require(first_variant_geometry.has_value() &&
                    *first_variant_geometry != candidates.value().mesh_geometry_digest &&
                    first_variant_source.has_value() &&
                    *first_variant_source != candidates.value().source.content_digest,
                "each structure class should exercise two distinct source meshes and content identities");
        }
        require(!candidates.value().mesh_geometry_digest.is_zero() &&
                !candidates.value().analysis_digest.is_zero() &&
                !candidates.value().constraints_digest.is_zero() &&
                !candidates.value().policy_digest.is_zero() &&
                !candidates.value().digest.is_zero(),
            "candidate receipt should carry non-zero source, constraint, and policy digests");

        std::map<std::string, const AuthoredPart*, std::less<>> parts_by_id;
        std::map<std::string, const AutorigLandmarkConstraint*, std::less<>> landmarks_by_id;
        for (const auto& part : archetype.parts) parts_by_id.emplace(part.id, &part);
        for (const auto& landmark : value.constraints.landmarks)
            landmarks_by_id.emplace(landmark.id, &landmark);
        double previous_score = std::numeric_limits<double>::infinity();
        for (const auto& candidate : candidates.value().ranked_candidates) {
            require(candidate.joints.size() == archetype.parts.size() &&
                    candidate.unrepresented_parts.empty() &&
                    candidate.selected_landmark_ids.size() == archetype.parts.size(),
                "authored partonomy and landmarks should be represented without inference");
            require(candidate.score.ranking_score <= previous_score &&
                    std::isfinite(candidate.score.ranking_score) &&
                    candidate.score.ranking_score >= 0.0 &&
                    candidate.score.ranking_score <= 1.0,
                "candidate receipts should be ordered by their finite declared score");
            previous_score = candidate.score.ranking_score;
            require(!candidate.digest.is_zero(),
                "each candidate should carry its own deterministic digest");
            if (archetype.name == "creature") {
                require(candidate.score.evaluated_bilateral_pairs == 0U &&
                        candidate.score.evaluated_bend_arc_transitions == 0U,
                    "repeated authored appendage roles and branch points must remain unpaired and unscored");
            }

            std::map<std::string, const AutorigCandidateJoint*, std::less<>> joints_by_part;
            for (const auto& joint : candidate.joints) {
                const auto part = parts_by_id.find(joint.part_id);
                require(part != parts_by_id.end() &&
                        joints_by_part.emplace(joint.part_id, &joint).second,
                    "candidate joints should map to caller-authored partonomy nodes");
                const auto landmark = std::find_if(
                    landmarks_by_id.begin(), landmarks_by_id.end(),
                    [&joint](const auto& item) {
                        return item.second->id == joint.landmark_id;
                    });
                const auto* source_vertex = landmark == landmarks_by_id.end()
                    ? nullptr : value.mesh.find_vertex(landmark->second->vertex);
                require(landmark != landmarks_by_id.end() &&
                        source_vertex != nullptr &&
                        landmark->second->part_id == joint.part_id &&
                        landmark->second->semantic_role == joint.semantic_role &&
                        landmark->second->vertex == joint.source_vertex &&
                        source_vertex->position.x == joint.position.x &&
                        source_vertex->position.y == joint.position.y &&
                        source_vertex->position.z == joint.position.z &&
                        landmark->second->provenance.assertion_id ==
                            joint.provenance.assertion_id,
                    "candidate joints should retain exact landmark and author provenance");
            }
            for (const auto& part : archetype.parts) {
                const auto* joint = joints_by_part.at(part.id);
                const auto region = std::find_if(
                    value.constraints.allowed_regions.begin(),
                    value.constraints.allowed_regions.end(), [&part](const auto& item) {
                        return item.part_id == part.id;
                    });
                require(region != value.constraints.allowed_regions.end() &&
                        std::binary_search(region->vertices.begin(), region->vertices.end(),
                            joint->source_vertex),
                    "candidate source anchors should stay within authored allowed regions");
                if (!part.parent.has_value()) {
                    require(!joint->parent.has_value(),
                        "the authored root must remain the candidate root");
                } else {
                    const auto* parent = joints_by_part.at(*part.parent);
                    require(joint->parent == parent->id,
                        "candidate joints should preserve authored parent relationships");
                }
            }
        }
        require(value.mesh.revision() == source_revision &&
                value.mesh.vertex_count() == source_vertex_count,
            "candidate generation must leave each source mesh unchanged");
        const auto repeated = generate_autorig_candidates(
            value.mesh, value.request, value.constraints);
        require(repeated && repeated.value().digest == candidates.value().digest,
            "each authored archetype receipt should reproduce deterministically");
        if (!archetype.alternative_part.empty()) {
            std::vector<std::string> choices;
            for (const auto& candidate : candidates.value().ranked_candidates) {
                const auto joint = std::find_if(candidate.joints.begin(), candidate.joints.end(),
                    [&archetype](const auto& value) {
                        return value.part_id == archetype.alternative_part;
                    });
                require(joint != candidate.joints.end(),
                    "the authored alternative part should have a candidate joint");
                choices.push_back(joint->landmark_id);
            }
            std::sort(choices.begin(), choices.end());
            require(choices.size() == 2U && choices[0U] != choices[1U],
                "candidate set should preserve both human-authored landmark alternatives");
        }
        receipt_digests.push_back(candidates.value().digest);
        }
    }
    std::sort(receipt_digests.begin(), receipt_digests.end());
    require(std::adjacent_find(receipt_digests.begin(), receipt_digests.end()) ==
            receipt_digests.end(),
        "different authored structure classes should produce distinct receipts");
}

void six_limb_creature_and_mechanical_receipts_preserve_authored_links() {
    const std::vector<AuthoredPart> creature_parts = {
        {"creature.core", "core", std::nullopt, AutorigPartSide::center},
        {"creature.limb.1", "limb", "creature.core", AutorigPartSide::left},
        {"creature.limb.2", "limb", "creature.core", AutorigPartSide::right},
        {"creature.limb.3", "limb", "creature.core", AutorigPartSide::left},
        {"creature.limb.4", "limb", "creature.core", AutorigPartSide::right},
        {"creature.limb.5", "limb", "creature.core", AutorigPartSide::left},
        {"creature.limb.6", "limb", "creature.core", AutorigPartSide::right},
        {"creature.wing.left", "wing", "creature.core", AutorigPartSide::left},
        {"creature.wing.right", "wing", "creature.core", AutorigPartSide::right},
    };
    const auto creature = archetype_fixture("six-limb-creature", creature_parts, 0U);
    const auto creature_variant = archetype_fixture("six-limb-creature", creature_parts, 1U);
    const auto creature_candidates = generate_autorig_candidates(
        creature.mesh, creature.request, creature.constraints);
    const auto variant_candidates = generate_autorig_candidates(
        creature_variant.mesh, creature_variant.request, creature_variant.constraints);
    require(creature_candidates && creature_candidates.value().ranked_candidates.size() == 1U &&
            creature_candidates.value().ranked_candidates.front().joints.size() == 9U &&
            variant_candidates && variant_candidates.value().ranked_candidates.size() == 1U &&
            creature_candidates.value().mesh_geometry_digest !=
                variant_candidates.value().mesh_geometry_digest &&
            creature_candidates.value().source.content_digest !=
                variant_candidates.value().source.content_digest,
        "two distinct six-limb-with-wings source assets should preserve all authored parts");
    const auto& creature_candidate = creature_candidates.value().ranked_candidates.front();
    require(creature_candidate.unrepresented_parts.empty() &&
            creature_candidate.score.evaluated_bilateral_pairs == 1U,
        "the unique authored wing pair may be compared while repeated limb roles remain ambiguous");

    auto mechanical = six_limb_mechanical_fixture();
    const auto source_revision = mechanical.mesh.revision();
    const auto source_count = mechanical.mesh.vertex_count();
    const auto mechanical_candidates = generate_autorig_candidates(
        mechanical.mesh, mechanical.request, mechanical.constraints);
    require(mechanical_candidates && mechanical_candidates.value().validate(),
        "six-limb mechanical receipt should validate against exact source evidence");
    const auto& candidate = mechanical_candidates.value().ranked_candidates.front();
    require(candidate.joints.size() == 7U && candidate.mechanical_connections.size() == 6U,
        "six distinct rigid-port hints should create six explicit candidate links");
    std::set<std::string> hints;
    for (const auto& connection : candidate.mechanical_connections) {
        require(hints.insert(connection.port_hint_id).second &&
                connection.kind == kinematic::JointKind::hinge &&
                connection.parent_port_landmark_id == "port.base" &&
                connection.provenance.rationale == "reviewed six-limb rigid hinge",
            "each mechanical limb should preserve its own hinge, port, and rationale");
    }
    require(hints.size() == 6U && mechanical.mesh.revision() == source_revision &&
            mechanical.mesh.vertex_count() == source_count,
        "six-limb mechanical admission must be complete and source-preserving");
}

void generates_deterministic_ranked_alternatives_without_mutation() {
    auto value = fixture();
    const auto source_revision = value.mesh.revision();
    AutorigCandidatePolicy policy;
    policy.minimum_joint_separation = 0.5;
    const auto first = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, policy);
    require(static_cast<bool>(first), "authored candidates should generate");
    require(static_cast<bool>(first.value().validate()),
        "generated candidate set should pass structural and digest validation");
    require(first.value().ranked_candidates.size() == 2U,
        "each explicit anchor alternative should produce a separate candidate");
    require(first.value().ranked_candidates[0U].score.ranking_score >=
            first.value().ranked_candidates[1U].score.ranking_score,
        "candidate ranking should sort by the declared policy score");
    require(first.value().ranked_candidates.front().selected_landmark_ids.back() ==
            "anchor.branch.far",
        "the candidate with stronger nonadjacent clearance should rank first");
    require(first.value().ranked_candidates.front().unrepresented_parts ==
            std::vector<std::string>{"part.unanchored"},
        "unanchored parts remain explicit omissions");
    require(first.value().ranked_candidates.front().joints.size() == 3U &&
            first.value().ranked_candidates.front().joints[0U].id.value == 1U &&
            first.value().ranked_candidates.front().joints[1U].parent.value().value == 1U &&
            first.value().ranked_candidates.front().joints[2U].parent.value().value == 2U,
        "preview-local joints preserve the authored hierarchy");
    require(first.value().ranked_candidates.front().joints[0U].provenance.assertion_id ==
            "assert.root",
        "candidate joints preserve authored assertion provenance");
    require(first.value().source == value.request.source &&
            first.value().mesh_revision == source_revision &&
            !first.value().analysis_digest.is_zero() &&
            !first.value().constraints_digest.is_zero() &&
            !first.value().policy_digest.is_zero() && !first.value().digest.is_zero(),
        "the candidate receipt binds source, analysis, constraints, and policy");
    const auto repeated = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, policy);
    require(repeated && repeated.value().digest == first.value().digest,
        "equal source-bound proposals reproduce an identical digest");
    require(value.mesh.revision() == source_revision && value.mesh.vertex_count() == 4U,
        "candidate generation leaves the source mesh unchanged");

    auto tampered = first.value();
    tampered.ranked_candidates[0U].joints[0U].provenance.author_id = "other-author";
    require(!tampered.validate(),
        "mutating candidate provenance after ranking must invalidate its digest");
    tampered = first.value();
    tampered.ranked_candidates[0U].joints[2U].parent = AutorigCandidateJointId{999U};
    require(!tampered.validate(),
        "a dangling preview parent must fail candidate validation");
    tampered = first.value();
    tampered.ranked_candidates[0U].score.ranking_score = 0.0;
    require(!tampered.validate(),
        "mutating a ranking result must invalidate its digest");
}

void mechanical_ports_require_distinct_rigid_components_and_survive_receipts() {
    auto value = mechanical_port_fixture();
    const auto source_revision = value.mesh.revision();
    const auto source_vertices = value.mesh.vertex_count();
    const auto generated = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(generated && generated.value().validate(),
        "authored rigid ports on distinct components should produce a valid receipt");
    const auto& candidate = generated.value().ranked_candidates.front();
    require(candidate.mechanical_connections.size() == 1U,
        "every admitted mechanical port hint should appear in the preview candidate");
    const auto& connection = candidate.mechanical_connections.front();
    require(connection.port_hint_id == "port-hint.arm-hinge" &&
            connection.parent_joint == candidate.joints[0U].id &&
            connection.child_joint == candidate.joints[1U].id &&
            connection.parent_port_landmark_id == "port.base" &&
            connection.child_port_landmark_id == "port.arm" &&
            connection.parent_port_vertex == VertexId{1U} &&
            connection.child_port_vertex == VertexId{4U} &&
            connection.kind == kinematic::JointKind::hinge &&
            connection.axis.z == 1.0 && connection.lower_limit == -1.2 &&
            connection.upper_limit == 1.2 &&
            connection.provenance.assertion_id == "assert.mechanical.arm",
        "mechanical proposal should preserve exact joints, ports, limits, and author evidence");
    require(value.mesh.revision() == source_revision &&
            value.mesh.vertex_count() == source_vertices,
        "mechanical candidate generation must leave the source unchanged");

    auto tampered = generated.value();
    tampered.ranked_candidates.front().mechanical_connections.front().axis = {1.0, 0.0, 0.0};
    require(!tampered.validate(),
        "post-generation mechanical port edits must invalidate the candidate receipt");

    auto connected = mechanical_port_fixture(false);
    const auto connected_analysis = analyze_autorig_features(connected.mesh, connected.request);
    require(connected_analysis &&
            !connected.constraints.validate_against(connected_analysis.value()),
        "ports in one connected mesh component must not masquerade as separate rigid parts");

    auto invalid = mechanical_port_fixture();
    invalid.constraints.mechanical_ports.front().kind = kinematic::JointKind::ball;
    require(!generate_autorig_candidates(invalid.mesh, invalid.request, invalid.constraints),
        "unsupported ball-port semantics must fail closed");

    invalid = mechanical_port_fixture();
    invalid.constraints.mechanical_ports.front().axis = {0.0, 0.0, 2.0};
    require(!generate_autorig_candidates(invalid.mesh, invalid.request, invalid.constraints),
        "non-normalized authored port axes must fail closed");

    invalid = mechanical_port_fixture();
    invalid.constraints.mechanical_ports.front().provenance.rationale.clear();
    require(!generate_autorig_candidates(invalid.mesh, invalid.request, invalid.constraints),
        "mechanical port hints without human justification must fail closed");

    invalid = mechanical_port_fixture();
    invalid.constraints.mechanical_ports.front().child_landmark_id = "port.base";
    require(!generate_autorig_candidates(invalid.mesh, invalid.request, invalid.constraints),
        "a port landmark owned by the wrong part must fail closed");

    invalid = mechanical_port_fixture();
    invalid.constraints.analysis_digest.bytes[0U] ^= 0x80U;
    require(!generate_autorig_candidates(invalid.mesh, invalid.request, invalid.constraints),
        "stale mechanical port analysis must fail closed without fallback data");
}

void bilateral_reflection_is_evidence_and_an_opt_in_ranking_preference() {
    auto value = bilateral_fixture();
    const auto source_revision = value.mesh.revision();
    const auto neutral = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(neutral && neutral.value().ranked_candidates.size() == 2U,
        "both explicitly authored bilateral alternatives should be retained");
    require(neutral.value().ranked_candidates.front().selected_landmark_ids.back() ==
            "a.anchor.right.offset",
        "default ranking must not silently prefer symmetry over authored asymmetry");
    const auto& exact = neutral.value().ranked_candidates[1U];
    const auto& offset = neutral.value().ranked_candidates[0U];
    require(exact.score.evaluated_bilateral_pairs == 1U &&
            offset.score.evaluated_bilateral_pairs == 1U &&
            exact.score.bilateral_reflection_fit == 1.0 &&
            offset.score.bilateral_reflection_fit < exact.score.bilateral_reflection_fit,
        "reflection evidence should report exact and asymmetric authored pairs");

    AutorigCandidatePolicy symmetry_review;
    symmetry_review.part_coverage_weight = 0.30;
    symmetry_review.nonadjacent_clearance_weight = 0.20;
    symmetry_review.length_reasonableness_weight = 0.20;
    symmetry_review.joint_budget_weight = 0.10;
    symmetry_review.bilateral_reflection_weight = 0.20;
    const auto ranked = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, symmetry_review);
    require(ranked && ranked.value().ranked_candidates.front().selected_landmark_ids.back() ==
            "z.anchor.right.exact",
        "caller-selected bilateral weight should rank the reflected authored alternative first");
    auto tampered = ranked.value();
    tampered.ranked_candidates.front().score.bilateral_reflection_fit = 0.0;
    require(!tampered.validate(),
        "candidate receipts must bind bilateral evidence against post-ranking edits");
    require(value.mesh.revision() == source_revision,
        "bilateral candidate ranking must leave the source mesh unchanged");
}

void bend_arc_smoothness_is_source_bound_and_opt_in() {
    auto value = bend_fixture();
    const auto neutral = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(neutral && neutral.value().ranked_candidates.size() == 2U,
        "both authored bend alternatives should be retained");
    const auto& default_top = neutral.value().ranked_candidates.front();
    require(default_top.selected_landmark_ids[2U] == "a.anchor.segment.2.bent",
        "default ranking must not silently reshape an authored bend toward a straight arc");
    require(default_top.score.evaluated_bend_arc_transitions == 1U &&
            neutral.value().ranked_candidates.back().score.evaluated_bend_arc_transitions == 1U &&
            default_top.score.bend_arc_regularity <
                neutral.value().ranked_candidates.back().score.bend_arc_regularity &&
            neutral.value().ranked_candidates.back().score.bend_arc_regularity == 1.0,
        "the receipt should measure adjacent bend-angle consistency along an authored chain");

    AutorigCandidatePolicy bend_review;
    bend_review.part_coverage_weight = 0.30;
    bend_review.nonadjacent_clearance_weight = 0.20;
    bend_review.length_reasonableness_weight = 0.20;
    bend_review.joint_budget_weight = 0.10;
    bend_review.bend_arc_regularity_weight = 0.20;
    const auto ranked = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, bend_review);
    require(ranked && ranked.value().ranked_candidates.front().selected_landmark_ids[2U] ==
            "z.anchor.segment.2.straight",
        "caller-selected bend regularity weight should affect ranking explicitly");
    auto tampered = ranked.value();
    tampered.ranked_candidates.front().score.bend_arc_regularity = 0.0;
    require(!tampered.validate(),
        "candidate receipts must bind bend-arc evidence against post-ranking edits");
}

void branch_coverage_and_segment_self_overlap_are_explicit_ranking_evidence() {
    auto value = overlap_fixture();
    AutorigCandidatePolicy policy;
    policy.minimum_bone_clearance = 0.05;
    const auto neutral = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, policy);
    require(neutral && neutral.value().ranked_candidates.size() == 2U,
        "both authored branch placements should be retained");
    const auto& crossing = neutral.value().ranked_candidates[0U];
    const auto& clear = neutral.value().ranked_candidates[1U];
    require(crossing.selected_landmark_ids[2U] == "a.anchor.branch.c.crossing" &&
            crossing.score.evaluated_nonadjacent_bones == 1U &&
            crossing.score.overlapping_nonadjacent_bones == 1U &&
            crossing.score.skeleton_self_clearance == 0.0 &&
            clear.score.evaluated_nonadjacent_bones == 1U &&
            clear.score.overlapping_nonadjacent_bones == 0U &&
            clear.score.skeleton_self_clearance == 1.0,
        "segment-interior crossings must be detected while endpoint-only separation is insufficient");
    require(crossing.score.branch_coverage == 2.0 / 3.0 &&
            clear.score.branch_coverage == 2.0 / 3.0,
        "branch coverage should count represented children of authored branch nodes");

    policy.part_coverage_weight = 0.25;
    policy.nonadjacent_clearance_weight = 0.15;
    policy.length_reasonableness_weight = 0.15;
    policy.joint_budget_weight = 0.15;
    policy.branch_coverage_weight = 0.10;
    policy.skeleton_self_clearance_weight = 0.20;
    const auto ranked = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, policy);
    require(ranked && ranked.value().ranked_candidates.front().selected_landmark_ids[2U] ==
            "z.anchor.branch.c.clear",
        "an explicit self-clearance weight should rank the non-overlapping branch alternative first");
}

void source_surface_crossings_are_opt_in_and_bind_ranking_evidence() {
    auto value = source_surface_crossing_fixture();
    const auto default_policy = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(default_policy && default_policy.value().ranked_candidates.size() == 2U &&
            default_policy.value().ranked_candidates[0U].score
                .evaluated_source_surface_bones == 0U &&
            default_policy.value().ranked_candidates[0U].score
                .source_surface_clearance == 0.0,
        "default ranking must not imply source-surface evidence when its opt-in weight is absent");

    AutorigCandidatePolicy policy;
    policy.part_coverage_weight = 0.8;
    policy.nonadjacent_clearance_weight = 0.0;
    policy.length_reasonableness_weight = 0.0;
    policy.joint_budget_weight = 0.0;
    policy.source_surface_crossing_weight = 0.2;
    const auto ranked = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, policy);
    require(ranked && ranked.value().ranked_candidates.size() == 2U,
        "bounded source-surface queries should retain every authored alternative");
    const auto crossing = std::find_if(
        ranked.value().ranked_candidates.begin(), ranked.value().ranked_candidates.end(),
        [](const auto& candidate) {
            return candidate.selected_landmark_ids[2U] ==
                "a.anchor.branch.c.crossing";
        });
    const auto clear = std::find_if(
        ranked.value().ranked_candidates.begin(), ranked.value().ranked_candidates.end(),
        [](const auto& candidate) {
            return candidate.selected_landmark_ids[2U] ==
                "z.anchor.branch.c.clear";
        });
    require(crossing != ranked.value().ranked_candidates.end() &&
            clear != ranked.value().ranked_candidates.end() &&
            crossing->score.evaluated_source_surface_bones == 3U &&
            crossing->score.source_surface_crossing_bones == 2U &&
            std::abs(crossing->score.source_surface_clearance - 1.0 / 3.0) < 1.0e-12 &&
            clear->score.evaluated_source_surface_bones == 3U &&
            clear->score.source_surface_crossing_bones == 1U &&
            std::abs(clear->score.source_surface_clearance - 2.0 / 3.0) < 1.0e-12,
        "the receipt must distinguish a branch crossing one additional source triangle");
    require(ranked.value().ranked_candidates.front().digest == clear->digest &&
            clear->digest != crossing->digest,
        "an explicitly weighted source-surface crossing measure must change ranking and receipt identity");
}

void authored_mechanical_ports_produce_exact_component_bound_proposals() {
    auto value = mechanical_fixture();
    const auto source_revision = value.mesh.revision();
    const auto candidates = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(candidates && candidates.value().ranked_candidates.size() == 1U,
        "valid authored rigid ports should produce a complete candidate receipt");
    const auto& candidate = candidates.value().ranked_candidates.front();
    require(candidate.mechanical_connections.size() == 2U &&
            candidate.mechanical_connections[0U].port_hint_id == "port.hinge" &&
            candidate.mechanical_connections[0U].kind == kinematic::JointKind::hinge &&
            candidate.mechanical_connections[0U].lower_limit == -1.2 &&
            candidate.mechanical_connections[1U].port_hint_id == "port.slider" &&
            candidate.mechanical_connections[1U].kind == kinematic::JointKind::slider &&
            candidate.mechanical_connections[1U].upper_limit == 2.0,
        "mechanical proposals should preserve authored port IDs, joint kinds, and limits");
    require(candidate.mechanical_connections[0U].parent_port_vertex == VertexId{1U} &&
            candidate.mechanical_connections[0U].child_port_vertex == VertexId{4U} &&
            candidate.mechanical_connections[1U].parent_port_vertex == VertexId{4U} &&
            candidate.mechanical_connections[1U].child_port_vertex == VertexId{7U},
        "mechanical proposals should retain exact source port vertices across rigid components");
    auto tampered = candidates.value();
    tampered.ranked_candidates[0U].mechanical_connections[0U].upper_limit = 3.0;
    require(!tampered.validate(),
        "candidate receipts must bind mechanical port limits against post-generation edits");
    const auto repeated = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(repeated && repeated.value().digest == candidates.value().digest,
        "mechanical port candidate receipts should reproduce deterministically");

    require(candidate.score.evaluated_control_links == 2U &&
            candidate.score.control_cost_fraction == 1.0,
        "control cost should count only explicitly authored actuated port links");
    auto tampered_control_cost = candidates.value();
    tampered_control_cost.ranked_candidates.front().score.control_cost_fraction = 0.0;
    require(!tampered_control_cost.validate(),
        "candidate receipts must bind control-cost evidence against post-ranking edits");
    auto fixed_variant = value;
    fixed_variant.constraints.mechanical_ports[0U].kind = kinematic::JointKind::fixed;
    fixed_variant.constraints.mechanical_ports[0U].axis = {};
    fixed_variant.constraints.mechanical_ports[0U].lower_limit = 0.0;
    fixed_variant.constraints.mechanical_ports[0U].upper_limit = 0.0;
    AutorigCandidatePolicy control_review;
    control_review.part_coverage_weight = 0.15;
    control_review.control_cost_weight = 0.20;
    const auto actuator_ranked = generate_autorig_candidates(
        value.mesh, value.request, value.constraints, control_review);
    const auto fixed_ranked = generate_autorig_candidates(
        fixed_variant.mesh, fixed_variant.request, fixed_variant.constraints, control_review);
    require(actuator_ranked && fixed_ranked &&
            fixed_ranked.value().ranked_candidates.front().score.control_cost_fraction == 0.5 &&
            fixed_ranked.value().ranked_candidates.front().score.ranking_score >
                actuator_ranked.value().ranked_candidates.front().score.ranking_score,
        "an explicit control-cost weight should rank fewer actuated authored links transparently");

    auto no_control_evidence = fixture();
    const auto neutral_cost = generate_autorig_candidates(
        no_control_evidence.mesh, no_control_evidence.request,
        no_control_evidence.constraints, control_review);
    require(neutral_cost &&
            neutral_cost.value().ranked_candidates.front().score.evaluated_control_links == 0U &&
            neutral_cost.value().ranked_candidates.front().score.control_cost_fraction == 0.0,
        "missing control-link evidence should stay explicitly unevaluated");

    auto same_component = value;
    same_component.constraints.landmarks[1U].vertex = VertexId{2U};
    same_component.constraints.allowed_regions[1U].vertices = {VertexId{2U}};
    require(!generate_autorig_candidates(
        same_component.mesh, same_component.request, same_component.constraints),
        "a mechanical port that collapses onto one analyzed component must refuse");
    auto invalid_axis = value;
    invalid_axis.constraints.mechanical_ports[0U].axis = {0.0, 0.0, 2.0};
    require(!generate_autorig_candidates(
        invalid_axis.mesh, invalid_axis.request, invalid_axis.constraints),
        "non-normalized mechanical axes must fail closed");
    auto missing_port = value;
    missing_port.constraints.mechanical_ports[0U].provenance.rationale.clear();
    require(!generate_autorig_candidates(
        missing_port.mesh, missing_port.request, missing_port.constraints),
        "mechanical hints without human justification must fail closed");
    require(value.mesh.revision() == source_revision,
        "mechanical proposal generation must not mutate source rigid components");
}

void stale_sources_bad_provenance_and_budgets_fail_closed() {
    auto value = fixture();
    auto moved = value.mesh.set_vertex_position(VertexId{4U}, {1.1, 2.0, 0.0});
    require(static_cast<bool>(moved), "stale-source fixture should move");
    require(!generate_autorig_candidates(value.mesh, value.request, value.constraints),
        "stale mesh revision must not produce fallback candidates");

    value = fixture();
    value.constraints.landmarks[0U].provenance.rationale.clear();
    require(!generate_autorig_candidates(value.mesh, value.request, value.constraints),
        "missing human justification must refuse candidate generation");

    value = fixture();
    AutorigCandidatePolicy too_small;
    too_small.max_candidates = 1U;
    require(!generate_autorig_candidates(value.mesh, value.request, value.constraints, too_small),
        "candidate alternatives above the configured budget must refuse atomically");

    value = fixture();
    AutorigCandidatePolicy invalid_weights;
    invalid_weights.part_coverage_weight = 0.5;
    require(!generate_autorig_candidates(
        value.mesh, value.request, value.constraints, invalid_weights),
        "invalid ranking weights must not silently fall back to defaults");

    value = fixture();
    value.constraints.allowed_regions.push_back({
        "region.branch", "part.branch", {VertexId{4U}},
        provenance("assert.region.branch", "reviewed branch region")});
    require(!generate_autorig_candidates(value.mesh, value.request, value.constraints),
        "an authored landmark outside its part's allowed region must refuse the full set");

    value = fixture();
    value.constraints.allowed_regions.push_back({
        "region.branch", "part.branch", {VertexId{4U}, VertexId{3U}},
        provenance("assert.region.branch", "reviewed branch region")});
    require(!generate_autorig_candidates(value.mesh, value.request, value.constraints),
        "non-canonical allowed-region vertex order must fail closed");
}

void missing_root_anchor_is_not_guessed() {
    auto value = fixture();
    value.constraints.landmarks.erase(
        std::remove_if(value.constraints.landmarks.begin(), value.constraints.landmarks.end(),
            [](const auto& item) { return item.part_id == "part.root"; }),
        value.constraints.landmarks.end());
    const auto result = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(!result && result.error().code == core::ErrorCode::unsupported,
        "candidate generation must refuse when no explicit root anchor exists");
}

class comma_decimal final : public std::numpunct<char> {
protected:
    [[nodiscard]] char do_decimal_point() const override { return ','; }
    [[nodiscard]] char do_thousands_sep() const override { return '.'; }
    [[nodiscard]] std::string do_grouping() const override { return "\3"; }
};

void source_and_candidate_receipts_are_locale_independent() {
    auto value = fixture();
    const auto baseline_analysis = analyze_autorig_features(value.mesh, value.request);
    const auto baseline_candidates = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    require(baseline_analysis && baseline_candidates,
        "canonical receipt baseline should be available");

    const auto previous_locale = std::locale::global(
        std::locale(std::locale::classic(), new comma_decimal()));
    std::ostringstream locale_probe;
    locale_probe << 1234.5;
    const auto localized_analysis = analyze_autorig_features(value.mesh, value.request);
    const auto localized_candidates = generate_autorig_candidates(
        value.mesh, value.request, value.constraints);
    (void)std::locale::global(previous_locale);

    require(locale_probe.str() == "1.234,5",
        "the test must demonstrate a non-classic process numeric locale");
    require(localized_analysis && localized_candidates,
        "receipt generation should remain available under a non-classic locale");
    require(localized_analysis.value().mesh_geometry_digest ==
                baseline_analysis.value().mesh_geometry_digest &&
            localized_analysis.value().analysis_digest ==
                baseline_analysis.value().analysis_digest &&
            localized_candidates.value().digest == baseline_candidates.value().digest,
        "geometry, analysis, policy, and candidate receipts must not depend on process locale");
}

void candidate_review_locks_edits_and_replans_without_source_mutation() {
    auto value = fixture();
    const auto source_revision = value.mesh.revision();
    const auto source_vertex_count = value.mesh.vertex_count();
    auto opened = AutorigCandidateReview::begin(
        value.mesh, value.request, value.constraints);
    require(opened && opened.value().candidates().ranked_candidates.size() == 2U,
        "review should open over the complete source-bound alternative set");
    auto review = std::move(opened).value();
    const auto original_constraints_digest = review.candidates().constraints_digest;
    require(!review.accept_review(provenance("review.initial")),
        "review acceptance must fail until a current candidate is selected");
    require(!review.select_candidate(digest("forged-candidate")),
        "review must reject a candidate digest outside the current proposal set");

    const auto selected = review.candidates().ranked_candidates.front();
    require(static_cast<bool>(review.select_candidate(selected.digest)),
        "review should select only a candidate from its current set");
    require(static_cast<bool>(review.lock_landmark("part.branch", "anchor.branch.far")),
        "review should lock the selected candidate's exact authored anchor");
    require(!review.lock_landmark("part.branch", "anchor.branch.near"),
        "review must reject a lock that contradicts the selected candidate");
    auto missing_decision = provenance("", "");
    require(!review.accept_review(missing_decision),
        "review acceptance must reject missing reviewer identity and rationale");
    auto imported_decision = provenance("review.imported", "external interpretation");
    imported_decision.origin = AutorigConstraintOrigin::imported_reference;
    require(!review.accept_review(imported_decision),
        "an imported reference cannot stand in for the human acceptance decision");
    const auto first_acceptance = review.accept_review(
        provenance("review.selected", "selected after reviewing alternatives"));
    require(first_acceptance && first_acceptance.value().validate() &&
            first_acceptance.value().candidate_set_digest == review.candidates().digest,
        "a current selected candidate should produce a receipt bound to its exact proposal set");
    auto tampered_acceptance = first_acceptance.value();
    tampered_acceptance.reviewer_decision.rationale = "changed after review";
    require(!tampered_acceptance.validate(),
        "review acceptance receipt must detect tampered reviewer evidence");

    require(static_cast<bool>(review.replan(value.mesh)),
        "review should replan against the unchanged exact source");
    require(review.candidates().ranked_candidates.size() == 1U &&
            review.candidates().ranked_candidates.front().joints.back().landmark_id ==
                "anchor.branch.far" && !review.selected_candidate_digest(),
        "locked anchor should filter alternatives and force a fresh human selection");
    require(!review.accept_review(provenance("review.stale-selection")),
        "replanning must invalidate the previous selection until it is reviewed again");

    const auto far_anchor = std::find_if(value.constraints.landmarks.begin(),
        value.constraints.landmarks.end(), [](const auto& landmark) {
            return landmark.id == "anchor.branch.far";
        });
    const auto near_anchor = std::find_if(value.constraints.landmarks.begin(),
        value.constraints.landmarks.end(), [](const auto& landmark) {
            return landmark.id == "anchor.branch.near";
        });
    require(far_anchor != value.constraints.landmarks.end() &&
            near_anchor != value.constraints.landmarks.end(),
        "fixture should expose both reviewed anchor alternatives");
    auto unjustified_anchor = *far_anchor;
    unjustified_anchor.provenance = {};
    require(!review.edit_landmark(std::move(unjustified_anchor)),
        "review must reject anchor edits without valid provenance");
    auto edited_anchor = *far_anchor;
    edited_anchor.vertex = near_anchor->vertex;
    edited_anchor.provenance = provenance("assert.branch.edited", "artist-edited anchor");
    require(static_cast<bool>(review.edit_landmark(std::move(edited_anchor))),
        "review should admit an explicit provenance-bearing anchor edit");
    require(static_cast<bool>(review.replan(value.mesh)),
        "review should replan after the explicit anchor edit");
    const auto edited_set = review.candidates();
    require(edited_set.ranked_candidates.size() == 1U &&
            edited_set.ranked_candidates.front().joints.back().source_vertex == near_anchor->vertex &&
            edited_set.constraints_digest != original_constraints_digest,
        "edited anchor should change the candidate and bind new constraint evidence");
    require(static_cast<bool>(review.select_candidate(
                edited_set.ranked_candidates.front().digest)),
        "edited candidate should require an explicit post-replan selection");
    const auto edited_acceptance = review.accept_review(
        provenance("review.edited", "accepted the edited anchor proposal"));
    require(edited_acceptance &&
            edited_acceptance.value().constraints_digest != original_constraints_digest,
        "acceptance receipt should bind the edited constraint set");

    auto stale_mesh = value.mesh;
    require(static_cast<bool>(stale_mesh.add_vertex({0.25, 0.25, 0.0})),
        "stale-source fixture mutation should be admitted locally");
    const auto unchanged_review_digest = review.candidates().digest;
    require(!review.replan(stale_mesh) &&
            review.candidates().digest == unchanged_review_digest,
        "stale mesh revision must fail without replacing the current review set");
    require(value.mesh.revision() == source_revision &&
            value.mesh.vertex_count() == source_vertex_count,
        "selection, locks, edits, and replanning must leave the source mesh unchanged");
}

} // namespace

int main() {
    generates_deterministic_ranked_alternatives_without_mutation();
    mechanical_ports_require_distinct_rigid_components_and_survive_receipts();
    bilateral_reflection_is_evidence_and_an_opt_in_ranking_preference();
    bend_arc_smoothness_is_source_bound_and_opt_in();
    branch_coverage_and_segment_self_overlap_are_explicit_ranking_evidence();
    source_surface_crossings_are_opt_in_and_bind_ranking_evidence();
    authored_mechanical_ports_produce_exact_component_bound_proposals();
    stale_sources_bad_provenance_and_budgets_fail_closed();
    missing_root_anchor_is_not_guessed();
    authored_candidate_receipts_cover_four_structure_classes();
    six_limb_creature_and_mechanical_receipts_preserve_authored_links();
    source_and_candidate_receipts_are_locale_independent();
    candidate_review_locks_edits_and_replans_without_source_mutation();
    std::cout << "autorig candidate checks passed\n";
    return EXIT_SUCCESS;
}
