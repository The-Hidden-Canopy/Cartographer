#include <carto/character/autorig_constraints.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
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

AutorigFeatureAnalysis analysis_fixture() {
    EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    require(a && b && c, "landmark mesh vertices should be accepted");
    require(static_cast<bool>(mesh.add_face({a.value(), b.value(), c.value()})),
        "landmark source face should be accepted");
    AutorigAnalysisRequest request{
        MeshAssetRef{19U, digest("autorig-landmark-source")},
        mesh.revision(), AutorigAxis::x, 0.0, 1.0e-6,
        {a.value()}, {}};
    const auto result = analyze_autorig_features(mesh, request);
    require(static_cast<bool>(result), "exact source analysis should be available");
    return result.value();
}

AutorigConstraintProvenance provenance(
    std::string assertion,
    std::string author,
    std::string rationale) {
    return AutorigConstraintProvenance{
        std::move(assertion), std::move(author), std::move(rationale), "",
        AutorigConstraintOrigin::human_authored};
}

AutorigConstraintSet valid_constraints(const AutorigFeatureAnalysis& analysis) {
    return AutorigConstraintSet{
        analysis.source,
        analysis.mesh_revision,
        analysis.analysis_digest,
        {
            {"part.root", "root", std::nullopt, AutorigPartSide::center},
            {"part.torso", "torso", std::string("part.root"), AutorigPartSide::center},
            {"part.head", "head", std::string("part.torso"), AutorigPartSide::center},
        },
        {
            {"landmark.root", "part.root", "root", AutorigPartSide::center,
                VertexId{1U}, provenance("assertion.root", "artist-1", "Selected the reviewed root anchor.")},
            {"landmark.spine", "part.torso", "spine", AutorigPartSide::center,
                VertexId{2U}, provenance("assertion.spine", "artist-1", "Selected the reviewed spine anchor.")},
            {"landmark.head", "part.head", "head", AutorigPartSide::center,
                VertexId{3U}, provenance("assertion.head", "artist-2", "Selected the reviewed head anchor.")},
        },
        {},
        {},
    };
}

void explicit_source_bound_hierarchy_and_landmarks_are_admitted() {
    const auto analysis = analysis_fixture();
    auto constraints = valid_constraints(analysis);
    require(static_cast<bool>(constraints.validate_against(analysis)),
        "valid explicit landmarks and partonomy should be admitted");

    auto stale = constraints;
    stale.mesh_revision = core::Revision(stale.mesh_revision.value() + 1U);
    require(!stale.validate_against(analysis),
        "stale mesh revision must refuse all constraints");
    stale = constraints;
    stale.analysis_digest.bytes[0U] ^= 0x80U;
    require(!stale.validate_against(analysis),
        "analysis digest mismatch must refuse all constraints");
    stale = constraints;
    stale.landmarks.back().vertex = VertexId{999U};
    require(!stale.validate_against(analysis),
        "landmark anchors must exist in the exact analyzed mesh");
}

void conflicting_landmarks_refuse_with_both_provenance_records() {
    const auto analysis = analysis_fixture();
    auto constraints = valid_constraints(analysis);
    constraints.landmarks.push_back({
        "landmark.head.alternate", "part.head", "head", AutorigPartSide::center,
        VertexId{2U}, provenance(
            "assertion.head.alt", "artist-3", "Contested interpretation of the head anchor.")});
    const auto refused = constraints.validate_against(analysis);
    require(!refused && refused.error().context.size() == 2U,
        "conflicting landmark admission must preserve both assertion contexts");
    require(refused.error().context[0U].find("assertion.head") != std::string::npos &&
            refused.error().context[1U].find("assertion.head.alt") != std::string::npos &&
            refused.error().context[0U].find("artist-2") != std::string::npos &&
            refused.error().context[1U].find("artist-3") != std::string::npos,
        "conflict diagnostics must identify both authors and assertions");
}

void malformed_partonomy_and_missing_provenance_fail_closed() {
    const auto analysis = analysis_fixture();
    auto constraints = valid_constraints(analysis);
    constraints.parts[1U].parent_id = std::string("part.head");
    require(!constraints.validate_against(analysis),
        "partonomy cycles must be refused");

    constraints = valid_constraints(analysis);
    constraints.parts.push_back({"part.orphan-root", "root", std::nullopt,
        AutorigPartSide::center});
    require(!constraints.validate_against(analysis),
        "multiple partonomy roots must be refused");

    constraints = valid_constraints(analysis);
    constraints.landmarks[0U].provenance.rationale.clear();
    require(!constraints.validate_against(analysis),
        "landmark without rationale must be refused");

    constraints = valid_constraints(analysis);
    constraints.landmarks[0U].provenance.origin =
        static_cast<AutorigConstraintOrigin>(255U);
    require(!constraints.validate_against(analysis),
        "unknown provenance origins must be refused");

    constraints = valid_constraints(analysis);
    constraints.landmarks[0U].side = AutorigPartSide::left;
    require(!constraints.validate_against(analysis),
        "landmark side conflicting with its partonomy node must be refused");
}

void imported_landmarks_require_a_stable_reference() {
    auto provenance_value = provenance(
        "assertion.imported", "reviewer-1", "Imported from an explicit reference.");
    provenance_value.origin = AutorigConstraintOrigin::imported_reference;
    require(!provenance_value.validate(),
        "imported landmark evidence must name its source reference");
    provenance_value.evidence_reference = "drawing.sheet.2";
    require(static_cast<bool>(provenance_value.validate()),
        "bounded stable imported evidence reference should validate");
}

} // namespace

int main() {
    explicit_source_bound_hierarchy_and_landmarks_are_admitted();
    conflicting_landmarks_refuse_with_both_provenance_records();
    malformed_partonomy_and_missing_provenance_fail_closed();
    imported_landmarks_require_a_stable_reference();
    std::cout << "autorig constraints checks passed\n";
    return EXIT_SUCCESS;
}
