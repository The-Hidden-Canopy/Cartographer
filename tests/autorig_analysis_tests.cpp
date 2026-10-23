#include <carto/character/autorig_analysis.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

EditableMesh cube(double y_offset = 0.0) {
    EditableMesh mesh;
    const std::array<core::Vec3d, 8U> points{{
        {-1.0, y_offset - 1.0, -1.0}, {1.0, y_offset - 1.0, -1.0},
        {1.0, y_offset + 1.0, -1.0}, {-1.0, y_offset + 1.0, -1.0},
        {-1.0, y_offset - 1.0, 1.0}, {1.0, y_offset - 1.0, 1.0},
        {1.0, y_offset + 1.0, 1.0}, {-1.0, y_offset + 1.0, 1.0},
    }};
    std::array<VertexId, 8U> ids{};
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto added = mesh.add_vertex(points[index]);
        require(static_cast<bool>(added), "cube vertex should be accepted");
        ids[index] = added.value();
    }
    const std::array<std::array<std::size_t, 4U>, 6U> face_indices{{
        {{0U, 3U, 2U, 1U}}, {{4U, 5U, 6U, 7U}},
        {{0U, 1U, 5U, 4U}}, {{1U, 2U, 6U, 5U}},
        {{2U, 3U, 7U, 6U}}, {{3U, 0U, 4U, 7U}},
    }};
    for (const auto& face : face_indices) {
        const auto added = mesh.add_face({ids[face[0U]], ids[face[1U]],
            ids[face[2U]], ids[face[3U]]});
        require(static_cast<bool>(added), "cube face should be accepted");
    }
    require(static_cast<bool>(mesh.validate()), "cube fixture should be a valid closed mesh");
    return mesh;
}

EditableMesh open_sheet() {
    EditableMesh mesh;
    const auto left = mesh.add_vertex({-1.0, 0.0, 0.0});
    const auto right = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto center = mesh.add_vertex({0.0, 1.0, 0.0});
    require(left && right && center, "open-sheet vertices should be accepted");
    require(static_cast<bool>(mesh.add_face(
                {left.value(), right.value(), center.value()})),
        "open-sheet face should be accepted");
    require(static_cast<bool>(mesh.validate()), "open-sheet fixture should validate");
    return mesh;
}

AutorigAnalysisRequest request_for(
    const EditableMesh& mesh,
    std::vector<VertexId> seeds = {VertexId{1U}}) {
    return AutorigAnalysisRequest{
        MeshAssetRef{7U, digest("mixed-autorig-fixture")},
        mesh.revision(),
        AutorigAxis::x,
        0.0,
        1.0e-8,
        std::move(seeds),
        {},
    };
}

void mixed_mesh_corpus_has_deterministic_topology_geodesic_and_symmetry_features() {
    std::vector<EditableMesh> corpus;
    corpus.push_back(cube());
    corpus.push_back(open_sheet());
    corpus.push_back(cube());
    require(static_cast<bool>(corpus.back().add_vertex({0.0, 0.0, 0.0})),
        "centerline point fixture should be accepted");
    EditableMesh asymmetrical = cube();
    require(static_cast<bool>(asymmetrical.set_vertex_position(
                VertexId{2U}, {1.25, -1.0, -1.0})),
        "asymmetric fixture perturbation should be accepted");
    corpus.push_back(std::move(asymmetrical));
    EditableMesh multi_component;
    const auto base_component = cube();
    const auto base_vertices = base_component.vertices_sorted();
    const auto base_faces = base_component.faces_sorted();
    std::vector<Vertex> appended_vertices;
    std::vector<Face> appended_faces;
    for (std::uint64_t component_index = 0U; component_index < 2U; ++component_index) {
        for (const auto& vertex : base_vertices) {
            auto position = vertex.position;
            position.y += static_cast<double>(component_index) * 5.0;
            appended_vertices.push_back(Vertex{
                VertexId{vertex.id.value + component_index * 8U}, position});
        }
        for (const auto& face : base_faces) {
            auto copied = face;
            copied.id = FaceId{copied.id.value + component_index * 6U};
            for (auto& vertex : copied.vertices) {
                vertex.value += component_index * 8U;
            }
            appended_faces.push_back(std::move(copied));
        }
    }
    require(static_cast<bool>(multi_component.insert_bulk(
                std::move(appended_vertices), std::move(appended_faces))),
        "multi-component fixture should be assembled");
    corpus.push_back(std::move(multi_component));

    std::vector<assets::Sha256Digest> first_pass;
    for (std::size_t index = 0U; index < corpus.size(); ++index) {
        auto request = request_for(corpus[index],
            index == 0U ? std::vector<VertexId>{VertexId{1U}, VertexId{7U}}
                        : std::vector<VertexId>{VertexId{1U}});
        const auto first = analyze_autorig_features(corpus[index], request);
        const auto second = analyze_autorig_features(corpus[index], request);
        if (!first) {
            std::cerr << "fixture " << index << " first analysis: "
                << first.error().message << '\n';
        } else if (const auto checked = first.value().validate(); !checked) {
            std::cerr << "fixture " << index << " first receipt: "
                << checked.error().message << '\n';
        }
        if (!second) {
            std::cerr << "fixture " << index << " second analysis: "
                << second.error().message << '\n';
        }
        require(first && second && first.value().validate() && second.value().validate(),
            "every mixed fixture should produce a valid analysis receipt");
        require(first.value().analysis_digest == second.value().analysis_digest &&
                first.value().mesh_geometry_digest == second.value().mesh_geometry_digest,
            "repeated analysis must be deterministic for exact source and settings");
        require(first.value().vertices.size() == corpus[index].vertex_count(),
            "topology feature output must account for every source vertex");
        first_pass.push_back(first.value().analysis_digest);
    }

    const auto symmetric = analyze_autorig_features(corpus[0U],
        request_for(corpus[0U], {VertexId{7U}, VertexId{1U}}));
    require(symmetric && symmetric.value().symmetry_pairs.size() == 4U,
        "closed cube fixture should yield four geometric mirror pairs");
    require(symmetric.value().components.size() == 1U &&
            symmetric.value().vertices.front().valence == 3U &&
            !symmetric.value().vertices.front().boundary,
        "closed-mesh topology features should report one component and interior valence");
    require(symmetric.value().geodesic_fields.size() == 2U &&
            symmetric.value().geodesic_fields.front().seed == VertexId{1U},
        "geodesic fields should be emitted in stable seed order");
    const auto& first_field = symmetric.value().geodesic_fields.front();
    const auto seed_neighbor = std::find_if(first_field.distances.begin(),
        first_field.distances.end(), [](const AutorigGeodesicDistance& item) {
            return item.vertex == VertexId{2U};
        });
    require(seed_neighbor != first_field.distances.end() && seed_neighbor->reachable &&
            std::abs(seed_neighbor->distance - 2.0) < 1.0e-12,
        "edge-weighted geodesic distance should equal the cube edge length");

    const auto changed_seed_order = analyze_autorig_features(corpus[0U],
        request_for(corpus[0U], {VertexId{1U}, VertexId{7U}}));
    require(changed_seed_order &&
            changed_seed_order.value().analysis_digest == symmetric.value().analysis_digest,
        "input seed order must not alter canonical analysis output");

    const auto sheet_analysis = analyze_autorig_features(
        corpus[1U], request_for(corpus[1U]));
    require(sheet_analysis && sheet_analysis.value().centerline_vertices.size() == 1U &&
            sheet_analysis.value().centerline_vertices.front() == VertexId{3U} &&
            std::all_of(sheet_analysis.value().vertices.begin(),
                sheet_analysis.value().vertices.end(),
                [](const AutorigVertexFeature& item) { return item.boundary; }),
        "open-sheet boundary and centerline evidence must remain explicit");

    const auto& disconnected = corpus.back();
    const auto disconnected_analysis = analyze_autorig_features(
        disconnected, request_for(disconnected));
    require(disconnected_analysis && disconnected_analysis.value().components.size() == 2U,
        "disconnected fixture should preserve two explicit components");
    const auto& distances = disconnected_analysis.value().geodesic_fields.front().distances;
    require(!distances.back().reachable,
        "geodesics must report vertices outside the seed component as unreachable");
}

void symmetry_ambiguity_and_centerline_signals_are_preserved() {
    auto mesh = cube();
    require(static_cast<bool>(mesh.add_vertex({-1.0, -1.0, -1.0})),
        "duplicate source position should be admitted as a distinct stable vertex");
    require(static_cast<bool>(mesh.add_vertex({0.0, 0.0, 0.0})),
        "centerline source vertex should be admitted");
    const auto analysis = analyze_autorig_features(mesh, request_for(mesh));
    require(analysis && !analysis.value().ambiguous_vertices.empty(),
        "multiple geometric counterparts must stay explicitly ambiguous");
    require(std::find(analysis.value().centerline_vertices.begin(),
                analysis.value().centerline_vertices.end(), VertexId{10U}) !=
            analysis.value().centerline_vertices.end(),
        "centerline vertices must be reported separately from mirror pairs");
    require(analysis.value().symmetry_pairs.size() < 4U,
        "ambiguous candidates must not be silently resolved into pairs");
}

void unfamiliar_topology_and_unriggable_signals_remain_explicit() {
    EditableMesh mesh;
    const auto left_a = mesh.add_vertex({-1.0, 0.0, 0.0});
    const auto left_b = mesh.add_vertex({-1.0, 1.0, 0.0});
    const auto left_c = mesh.add_vertex({-1.0, 0.0, 1.0});
    const auto right_a = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto right_b = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto right_c = mesh.add_vertex({1.0, 0.0, 1.0});
    const auto right_extra = mesh.add_vertex({1.0, 1.0, 1.0});
    const auto isolated = mesh.add_vertex({3.0, 0.0, 0.0});
    require(left_a && left_b && left_c && right_a && right_b && right_c &&
            right_extra && isolated,
        "unfamiliar asymmetric topology vertices should be accepted");
    require(static_cast<bool>(mesh.add_face(
                {left_a.value(), left_b.value(), left_c.value()})),
        "left source patch should be accepted");
    require(static_cast<bool>(mesh.add_face(
                {right_a.value(), right_b.value(), right_c.value()})),
        "right source patch should be accepted");
    require(static_cast<bool>(mesh.add_face(
                {right_b.value(), right_a.value(), right_extra.value()})),
        "right-only topology branch should be accepted");
    require(static_cast<bool>(mesh.validate()),
        "unfamiliar disconnected fixture should be structurally valid");

    const auto analysis = analyze_autorig_features(
        mesh, request_for(mesh, {left_a.value()}));
    require(analysis && analysis.value().validate(),
        "unfamiliar geometry should yield a source-bound evidence receipt");
    require(analysis.value().components.size() == 3U,
        "disconnected patches and the isolated vertex must remain separate components");
    const auto mismatched_pair = std::find_if(
        analysis.value().symmetry_pairs.begin(), analysis.value().symmetry_pairs.end(),
        [&left_a, &right_a](const AutorigSymmetryPair& pair) {
            return pair.negative_side == left_a.value() &&
                pair.positive_side == right_a.value();
        });
    require(mismatched_pair != analysis.value().symmetry_pairs.end() &&
            !mismatched_pair->topology_compatible,
        "geometric correspondence with unequal local topology must remain flagged");
    require(std::find(analysis.value().unmatched_vertices.begin(),
                analysis.value().unmatched_vertices.end(), right_extra.value()) !=
            analysis.value().unmatched_vertices.end() &&
            std::find(analysis.value().unmatched_vertices.begin(),
                analysis.value().unmatched_vertices.end(), isolated.value()) !=
            analysis.value().unmatched_vertices.end(),
        "unpaired branch and isolated geometry must remain explicitly unmatched");
    const auto& distances = analysis.value().geodesic_fields.front().distances;
    const auto isolated_distance = std::find_if(distances.begin(), distances.end(),
        [&isolated](const AutorigGeodesicDistance& item) {
            return item.vertex == isolated.value();
        });
    require(isolated_distance != distances.end() && !isolated_distance->reachable,
        "isolated geometry must not receive a fabricated geodesic path");
}

void stale_sources_invalid_seeds_and_budget_exhaustion_fail_closed() {
    auto mesh = cube();
    auto request = request_for(mesh);
    auto stale_mesh = mesh;
    require(static_cast<bool>(stale_mesh.set_vertex_position(
                VertexId{1U}, {-1.1, -1.0, -1.0})),
        "stale fixture mutation should advance mesh revision");
    require(!analyze_autorig_features(stale_mesh, request),
        "analysis must refuse a mesh changed since the expected revision");

    auto missing_seed = request_for(mesh, {VertexId{999U}});
    require(!analyze_autorig_features(mesh, missing_seed),
        "missing geodesic seeds must fail before emitting partial analysis");
    auto duplicate_seed = request_for(mesh, {VertexId{1U}, VertexId{1U}});
    require(!duplicate_seed.validate(), "duplicate geodesic seeds must be rejected");

    auto small_budget = request_for(mesh);
    small_budget.budget.max_vertices = 4U;
    require(!analyze_autorig_features(mesh, small_budget),
        "mesh over the requested vertex budget must fail closed");
    auto zero_tolerance = request_for(mesh);
    zero_tolerance.symmetry_tolerance = 0.0;
    require(!zero_tolerance.validate(), "zero symmetry tolerance is invalid");
    auto no_source_digest = request_for(mesh);
    no_source_digest.source.content_digest = {};
    require(!no_source_digest.validate(), "missing source digest must be rejected");
}

} // namespace

int main() {
    mixed_mesh_corpus_has_deterministic_topology_geodesic_and_symmetry_features();
    symmetry_ambiguity_and_centerline_signals_are_preserved();
    unfamiliar_topology_and_unriggable_signals_remain_explicit();
    stale_sources_invalid_seeds_and_budget_exhaustion_fail_closed();
    std::cout << "autorig analysis checks passed\n";
    return EXIT_SUCCESS;
}
