#include <carto/weights/auto_weight.hpp>

#include "weights_internal.hpp"

#include <carto/character/weights_record.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

namespace carto::weights {
namespace {

constexpr std::string_view kRecipeMagic = "CARTOGRAPHER_WEIGHTS_AUTO_RECIPE";
constexpr std::string_view kReceiptMagic = "CARTOGRAPHER_WEIGHTS_AUTO_RECEIPT";
constexpr double kDistanceEpsilon = 1e-12;
constexpr double kConvergenceEpsilon = 1e-10;
constexpr double kPruneEpsilon = 1e-12;

core::Result<std::vector<core::Vec3d>> anchors_for(
    const kinematic::KinematicGraph& graph,
    const AutoWeightRecipe& recipe,
    kinematic::JointId joint) {
    std::vector<core::Vec3d> anchors;
    const auto overridden = recipe.anchor_overrides.find(joint);
    if (overridden != recipe.anchor_overrides.end()) {
        if (!overridden->second.finite()) {
            return core::Result<std::vector<core::Vec3d>>::failure(detail::invalid(
                "an anchor override is not finite"));
        }
        anchors.push_back(overridden->second);
    }
    anchors.push_back(detail::joint_anchor(graph, joint));
    return core::Result<std::vector<core::Vec3d>>::success(std::move(anchors));
}

// Distance from every mesh vertex to the nearest of ONE joint's anchors
// (its rest anchor plus any additive overrides). Geodesic mode runs a
// multi-source Dijkstra over mesh edges; heat mode runs bounded explicit
// diffusion from every anchor seed. Unreachable vertices stay at infinity.
std::vector<double> compute_distance_field(
    const geometry::CompiledMesh& mesh,
    const std::vector<std::vector<std::uint32_t>>& adjacency,
    const std::vector<core::Vec3d>& anchors,
    const AutoWeightRecipe& recipe) {
    const std::size_t vertex_count = mesh.positions.size();

    // Nearest mesh vertex to each anchor, deterministic tie-break by index.
    std::vector<std::pair<double, std::uint32_t>> seeds;
    seeds.reserve(anchors.size());
    for (const core::Vec3d& anchor : anchors) {
        std::uint32_t seed = 0U;
        double seed_distance = std::numeric_limits<double>::infinity();
        for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
            const double distance = detail::distance_between(anchor, mesh.positions[vertex]);
            if (distance < seed_distance) {
                seed_distance = distance;
                seed = static_cast<std::uint32_t>(vertex);
            }
        }
        seeds.emplace_back(seed_distance, seed);
    }

    if (recipe.distance == DistanceMode::heat) {
        std::vector<double> heat(vertex_count, 0.0);
        std::vector<bool> is_seed(vertex_count, false);
        for (const auto& [seed_distance, seed] : seeds) {
            static_cast<void>(seed_distance);
            heat[seed] = 1.0;
            is_seed[seed] = true;
        }
        for (std::uint32_t step = 0U; step < recipe.heat_steps; ++step) {
            const std::vector<double> previous = heat;
            for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
                if (is_seed[vertex] || adjacency[vertex].empty()) continue;
                double average = 0.0;
                for (const std::uint32_t neighbor : adjacency[vertex]) {
                    average += previous[neighbor];
                }
                average /= static_cast<double>(adjacency[vertex].size());
                heat[vertex] = std::clamp(previous[vertex] + (average - previous[vertex]), 0.0, 1.0);
            }
        }
        std::vector<double> distances(vertex_count, 0.0);
        for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
            distances[vertex] = -std::log(std::max(heat[vertex], kDistanceEpsilon));
        }
        return distances;
    }

    std::vector<double> distances(vertex_count, std::numeric_limits<double>::infinity());
    using QueueEntry = std::pair<double, std::uint32_t>;
    std::set<QueueEntry> queue;
    for (const auto& [seed_distance, seed] : seeds) {
        if (seed_distance < distances[seed]) {
            distances[seed] = seed_distance;
            queue.insert({seed_distance, seed});
        }
    }
    while (!queue.empty()) {
        const auto [current_distance, vertex] = *queue.begin();
        queue.erase(queue.begin());
        if (current_distance > distances[vertex]) continue;
        for (const std::uint32_t neighbor : adjacency[vertex]) {
            const double edge = detail::distance_between(
                mesh.positions[vertex], mesh.positions[neighbor]);
            const double candidate = current_distance + edge;
            if (candidate < distances[neighbor]) {
                distances[neighbor] = candidate;
                queue.insert({candidate, neighbor});
            }
        }
    }
    return distances;
}

// Renormalize one influence list to an exact sum of one with compensated
// residual placement on the largest weight, prune zeros, and sort by joint id.
// Callers run character::validate_influences afterwards.
void normalize_influence_list(std::vector<kinematic::JointInfluence>& influences) {
    influences.erase(
        std::remove_if(influences.begin(), influences.end(), [](const kinematic::JointInfluence& entry) {
            return entry.weight <= kPruneEpsilon;
        }),
        influences.end());
    if (influences.empty()) return;
    double sum = 0.0;
    for (const kinematic::JointInfluence& entry : influences) sum += entry.weight;
    auto largest = std::max_element(influences.begin(), influences.end(), [](const auto& a, const auto& b) {
        if (a.weight != b.weight) return a.weight < b.weight;
        return b.joint < a.joint;
    });
    largest->weight += 1.0 - sum;
    std::sort(influences.begin(), influences.end(), [](const auto& a, const auto& b) {
        return a.joint < b.joint;
    });
}

} // namespace

std::string_view distance_mode_name(DistanceMode mode) noexcept {
    switch (mode) {
    case DistanceMode::geodesic: return "geodesic";
    case DistanceMode::heat: return "heat";
    }
    return "unknown";
}

core::Result<void> AutoWeightRecipe::validate() const {
    if (!output_layer_id) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe requires a layer identity"));
    }
    if (max_influences_per_vertex < kMinInfluencesPerVertex ||
        max_influences_per_vertex > kMaxInfluencesPerVertex) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe influence count is outside 1..8"));
    }
    if (nearest_joint_candidates < max_influences_per_vertex) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe candidate joints must cover the influence count"));
    }
    if (nearest_joint_candidates > kMaxCandidateJoints) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe candidate joints exceed the bound"));
    }
    if (!std::isfinite(distance_falloff) || distance_falloff <= 0.0) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe falloff must be positive and finite"));
    }
    if (smoothing_iterations > kMaxSmoothingIterations) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe smoothing iterations exceed the bound"));
    }
    if (!std::isfinite(smoothing_strength) || smoothing_strength <= 0.0 ||
        smoothing_strength >= 1.0) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe smoothing strength must be inside (0, 1)"));
    }
    if (heat_steps == 0U || heat_steps > kMaxHeatSteps) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe heat steps are outside the bound"));
    }
    if (anchor_overrides.size() > kMaxAnchorOverrides) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight recipe anchor overrides exceed the bound"));
    }
    for (const auto& [joint, anchor] : anchor_overrides) {
        if (!joint || !anchor.finite()) {
            return core::Result<void>::failure(detail::invalid(
                "auto weight recipe anchor override is invalid"));
        }
    }
    return core::Result<void>::success();
}

std::string AutoWeightRecipe::serialize() const {
    std::ostringstream output;
    output << kRecipeMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "LAYER " << output_layer_id.value << '\n';
    output << "INFLUENCES " << max_influences_per_vertex << '\n';
    output << "CANDIDATES " << nearest_joint_candidates << '\n';
    output << "FALLOFF " << distance_falloff << '\n';
    output << "SMOOTHING " << smoothing_iterations << ' ' << smoothing_strength << '\n';
    output << "DISTANCE " << distance_mode_name(distance) << '\n';
    output << "HEAT_STEPS " << heat_steps << '\n';
    output << "ANCHORS " << anchor_overrides.size() << '\n';
    for (const auto& [joint, anchor] : anchor_overrides) {
        output << "ANCHOR " << joint.value << ' ';
        detail::write_vec3(output, anchor);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<AutoWeightRecipe> AutoWeightRecipe::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<AutoWeightRecipe>::failure(detail::parse_error(
            "auto weight recipe text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kRecipeMagic ||
        !detail::read_integer(input, version)) {
        return core::Result<AutoWeightRecipe>::failure(detail::parse_error(
            "auto weight recipe framing is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<AutoWeightRecipe>::failure(detail::version_error(
            "auto weight recipe schema version is not supported"));
    }
    AutoWeightRecipe recipe;
    std::string distance_token;
    std::size_t anchor_count = 0U;
    if (!detail::require_token(input, "LAYER") ||
        !detail::read_integer(input, recipe.output_layer_id.value) ||
        !detail::require_token(input, "INFLUENCES") ||
        !detail::read_integer(input, recipe.max_influences_per_vertex) ||
        !detail::require_token(input, "CANDIDATES") ||
        !detail::read_integer(input, recipe.nearest_joint_candidates) ||
        !detail::require_token(input, "FALLOFF") ||
        !detail::read_double(input, recipe.distance_falloff) ||
        !detail::require_token(input, "SMOOTHING") ||
        !detail::read_integer(input, recipe.smoothing_iterations) ||
        !detail::read_double(input, recipe.smoothing_strength) ||
        !detail::require_token(input, "DISTANCE") ||
        !(input >> distance_token) ||
        !detail::require_token(input, "HEAT_STEPS") ||
        !detail::read_integer(input, recipe.heat_steps) ||
        !detail::require_token(input, "ANCHORS") ||
        !detail::read_count(input, kMaxAnchorOverrides, anchor_count)) {
        return core::Result<AutoWeightRecipe>::failure(detail::parse_error(
            "auto weight recipe body is invalid"));
    }
    if (distance_token == "geodesic") {
        recipe.distance = DistanceMode::geodesic;
    } else if (distance_token == "heat") {
        recipe.distance = DistanceMode::heat;
    } else {
        return core::Result<AutoWeightRecipe>::failure(detail::parse_error(
            "auto weight recipe distance mode is unknown"));
    }
    for (std::size_t index = 0U; index < anchor_count; ++index) {
        kinematic::JointId joint;
        core::Vec3d anchor;
        if (!detail::require_token(input, "ANCHOR") ||
            !detail::read_integer(input, joint.value) ||
            !detail::read_vec3(input, anchor)) {
            return core::Result<AutoWeightRecipe>::failure(detail::parse_error(
                "auto weight recipe anchor is invalid"));
        }
        recipe.anchor_overrides.emplace(joint, anchor);
    }
    if (!detail::require_token(input, "END")) {
        return core::Result<AutoWeightRecipe>::failure(detail::parse_error(
            "auto weight recipe terminator is missing"));
    }
    if (auto result = recipe.validate(); !result) {
        return core::Result<AutoWeightRecipe>::failure(result.error());
    }
    return core::Result<AutoWeightRecipe>::success(std::move(recipe));
}

core::Result<assets::Sha256Digest> AutoWeightRecipe::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<void> AutoWeightReceipt::validate() const {
    if (recipe_digest.is_zero() || output_binding_digest.is_zero()) {
        return core::Result<void>::failure(detail::invalid(
            "auto weight receipt requires recipe and output digests"));
    }
    if (!std::isfinite(final_residual) || final_residual < 0.0) {
        return core::Result<void>::failure(detail::validation(
            "auto weight receipt residual must be finite and non-negative"));
    }
    if (influenced_vertex_count == 0U ||
        influenced_vertex_count > character::SkinWeightLayer::kMaxInfluencedVertices ||
        total_influence_count < influenced_vertex_count ||
        total_influence_count >
            influenced_vertex_count *
                static_cast<std::uint64_t>(kinematic::SkinBinding::kMaxInfluencesPerVertex)) {
        return core::Result<void>::failure(detail::validation(
            "auto weight receipt influence population is invalid"));
    }
    return core::Result<void>::success();
}

std::string AutoWeightReceipt::serialize() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "RECIPE " << recipe_digest.hex() << '\n';
    output << "DISTANCE " << distance_mode_name(distance) << '\n';
    output << "ITERATIONS_USED " << iterations_used << '\n';
    output << "FINAL_RESIDUAL " << final_residual << '\n';
    output << "JOINTS " << joint_count << '\n';
    output << "INFLUENCED_VERTICES " << influenced_vertex_count << '\n';
    output << "TOTAL_INFLUENCES " << total_influence_count << '\n';
    output << "OUTPUT " << output_binding_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<AutoWeightReceipt> AutoWeightReceipt::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<AutoWeightReceipt>::failure(detail::parse_error(
            "auto weight receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kReceiptMagic ||
        !detail::read_integer(input, version) || version != kSchemaVersion) {
        return core::Result<AutoWeightReceipt>::failure(detail::parse_error(
            "auto weight receipt framing is invalid"));
    }
    AutoWeightReceipt receipt;
    std::string distance_token;
    if (!detail::require_token(input, "RECIPE") ||
        !detail::read_digest(input, receipt.recipe_digest) ||
        !detail::require_token(input, "DISTANCE") ||
        !(input >> distance_token) ||
        !detail::require_token(input, "ITERATIONS_USED") ||
        !detail::read_integer(input, receipt.iterations_used) ||
        !detail::require_token(input, "FINAL_RESIDUAL") ||
        !detail::read_double(input, receipt.final_residual) ||
        !detail::require_token(input, "JOINTS") ||
        !detail::read_integer(input, receipt.joint_count) ||
        !detail::require_token(input, "INFLUENCED_VERTICES") ||
        !detail::read_integer(input, receipt.influenced_vertex_count) ||
        !detail::require_token(input, "TOTAL_INFLUENCES") ||
        !detail::read_integer(input, receipt.total_influence_count) ||
        !detail::require_token(input, "OUTPUT") ||
        !detail::read_digest(input, receipt.output_binding_digest) ||
        !detail::require_token(input, "END")) {
        return core::Result<AutoWeightReceipt>::failure(detail::parse_error(
            "auto weight receipt body is invalid"));
    }
    if (distance_token == "geodesic") {
        receipt.distance = DistanceMode::geodesic;
    } else if (distance_token == "heat") {
        receipt.distance = DistanceMode::heat;
    } else {
        return core::Result<AutoWeightReceipt>::failure(detail::parse_error(
            "auto weight receipt distance mode is unknown"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<AutoWeightReceipt>::failure(result.error());
    }
    return core::Result<AutoWeightReceipt>::success(std::move(receipt));
}

core::Result<assets::Sha256Digest> AutoWeightReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(serialize()));
}

core::Result<AutoWeightOutput> generate_auto_weights(
    const geometry::CompiledMesh& mesh,
    const kinematic::KinematicGraph& graph,
    const assets::Sha256Digest& mesh_digest,
    std::uint64_t mesh_asset,
    const AutoWeightRecipe& recipe) {
    if (auto result = mesh.valid() ? core::Result<void>::success()
                                   : core::Result<void>::failure(detail::invalid(
                                         "auto weight input mesh is not valid"));
        !result) {
        return core::Result<AutoWeightOutput>::failure(result.error());
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<AutoWeightOutput>::failure(result.error());
    }
    if (auto result = recipe.validate(); !result) {
        return core::Result<AutoWeightOutput>::failure(result.error());
    }
    if (mesh_digest.is_zero() || mesh_asset == 0U) {
        return core::Result<AutoWeightOutput>::failure(detail::invalid(
            "auto weight generation requires a content-addressed mesh identity"));
    }

    const auto& nodes = graph.nodes();
    if (nodes.empty()) {
        return core::Result<AutoWeightOutput>::failure(detail::invalid(
            "auto weight generation requires at least one joint"));
    }
    if (nodes.size() > AutoWeightRecipe::kMaxSolverJoints) {
        return core::Result<AutoWeightOutput>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "auto weight generation supports at most 1024 skinned joints in "
            "this first-pass solver; split the rig or refine the recipe"));
    }
    const std::size_t vertex_count = mesh.positions.size();

    // Deterministic joint order: sorted by joint id.
    std::vector<kinematic::JointId> joints;
    joints.reserve(nodes.size());
    for (const auto& [joint, node] : nodes) {
        joints.push_back(joint);
        static_cast<void>(node);
    }

    std::vector<std::vector<core::Vec3d>> anchors;
    anchors.reserve(joints.size());
    for (const kinematic::JointId joint : joints) {
        const auto joint_anchors = anchors_for(graph, recipe, joint);
        if (!joint_anchors) {
            return core::Result<AutoWeightOutput>::failure(joint_anchors.error());
        }
        anchors.push_back(joint_anchors.value());
    }

    // Per-joint distance fields merged into per-vertex candidate rankings:
    // the k nearest joints by distance, ties resolved to the lower joint id.
    const auto& adjacency = detail::build_adjacency(vertex_count, mesh.indices);
    const std::size_t candidate_count = std::min<std::size_t>(
        recipe.nearest_joint_candidates, joints.size());
    // Stable tie-breaking (ch08 "stable tie-breaking after each solver step"):
    // geodesic distances that are equal in exact arithmetic can differ by
    // ~1e-16 depending on Dijkstra relaxation order. Quantizing to a
    // mesh-relative quantum makes those ties compare equal so they resolve
    // deterministically to the lower joint id.
    const double extent = detail::distance_between(
        mesh.bounds.minimum, mesh.bounds.maximum);
    const double tie_quantum = 1e-9 * std::max(1.0, extent);
    std::vector<std::vector<std::pair<double, std::size_t>>> ranked(vertex_count);
    for (std::size_t joint = 0U; joint < joints.size(); ++joint) {
        const std::vector<double> field =
            compute_distance_field(mesh, adjacency, anchors[joint], recipe);
        for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
            auto& candidates = ranked[vertex];
            const double snapped = std::isfinite(field[vertex])
                ? std::round(field[vertex] / tie_quantum) * tie_quantum
                : field[vertex];
            candidates.emplace_back(snapped, joint);
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });
            if (candidates.size() > candidate_count) candidates.pop_back();
        }
    }

    // Seed: normalized inverse-distance falloff over the k nearest joints.
    std::vector<std::vector<kinematic::JointInfluence>> per_vertex(vertex_count);
    std::size_t unreachable = 0U;
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
        const auto& candidates = ranked[vertex];
        if (candidates.empty() || !std::isfinite(candidates.front().first)) {
            ++unreachable;
            continue;
        }
        std::vector<kinematic::JointInfluence>& influences = per_vertex[vertex];
        for (std::size_t index = 0U; index < candidates.size(); ++index) {
            const double distance = std::max(candidates[index].first, kDistanceEpsilon);
            const double weight = 1.0 / std::pow(distance, recipe.distance_falloff);
            influences.push_back(kinematic::JointInfluence{joints[candidates[index].second], weight});
        }
        normalize_influence_list(influences);
    }
    if (unreachable != 0U) {
        return core::Result<AutoWeightOutput>::failure(
            detail::validation(
                "auto weight generation refused: mesh has vertices unreachable "
                "from every joint anchor (disconnected regions require an "
                "explicit anchor override)")
                .with_context("unreachable_vertices=" + std::to_string(unreachable)));
    }

    // Bounded Laplacian smoothing over mesh adjacency.
    std::uint32_t iterations_used = 0U;
    double final_residual = 0.0;
    for (std::uint32_t iteration = 0U; iteration < recipe.smoothing_iterations; ++iteration) {
        double max_change = 0.0;
        std::vector<std::vector<kinematic::JointInfluence>> next = per_vertex;
        for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
            if (adjacency[vertex].empty()) continue;
            std::vector<kinematic::JointInfluence>& updated = next[vertex];
            for (kinematic::JointInfluence& influence : updated) {
                double neighbor_mass = 0.0;
                for (const std::uint32_t neighbor : adjacency[vertex]) {
                    const auto& neighbor_influences = per_vertex[neighbor];
                    const auto found = std::find_if(
                        neighbor_influences.begin(), neighbor_influences.end(),
                        [&](const kinematic::JointInfluence& entry) {
                            return entry.joint == influence.joint;
                        });
                    if (found != neighbor_influences.end()) neighbor_mass += found->weight;
                }
                const double average = neighbor_mass / static_cast<double>(adjacency[vertex].size());
                const double smoothed = influence.weight +
                    recipe.smoothing_strength * (average - influence.weight);
                max_change = std::max(max_change, std::abs(smoothed - influence.weight));
                influence.weight = std::max(smoothed, 0.0);
            }
            normalize_influence_list(updated);
        }
        per_vertex = std::move(next);
        iterations_used = iteration + 1U;
        final_residual = max_change;
        if (max_change < kConvergenceEpsilon) break;
    }

    // Admit every vertex through the shared contract and publish one complete
    // layer; a single failure refuses the whole generation.
    kinematic::SkinBinding binding;
    binding.mesh_asset = mesh_asset;
    binding.mesh_digest = mesh_digest;
    const auto graph_digest = graph.canonical_digest();
    if (!graph_digest) {
        return core::Result<AutoWeightOutput>::failure(graph_digest.error());
    }
    binding.graph_digest = graph_digest.value();
    std::uint64_t total_influences = 0U;
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
        const auto& influences = per_vertex[vertex];
        if (influences.empty()) {
            return core::Result<AutoWeightOutput>::failure(detail::validation(
                "auto weight generation produced a vertex with no influences"));
        }
        const auto admitted = character::validate_influences(influences);
        if (!admitted) {
            return core::Result<AutoWeightOutput>::failure(
                admitted.error().with_context(
                    "vertex=" + std::to_string(vertex)));
        }
        total_influences += influences.size();
        binding.influences.emplace(
            static_cast<std::uint32_t>(vertex), influences);
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<AutoWeightOutput>::failure(result.error());
    }
    const auto binding_digest = binding.canonical_digest();
    if (!binding_digest) {
        return core::Result<AutoWeightOutput>::failure(binding_digest.error());
    }
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) {
        return core::Result<AutoWeightOutput>::failure(recipe_digest.error());
    }

    AutoWeightReceipt receipt;
    receipt.recipe_digest = recipe_digest.value();
    receipt.distance = recipe.distance;
    receipt.iterations_used = iterations_used;
    receipt.final_residual = final_residual;
    receipt.joint_count = joints.size();
    receipt.influenced_vertex_count = vertex_count;
    receipt.total_influence_count = total_influences;
    receipt.output_binding_digest = binding_digest.value();

    character::SkinWeightLayer layer;
    layer.layer_id = recipe.output_layer_id;
    layer.mesh_topology_digest = mesh_digest;
    layer.joint_graph_digest = binding.graph_digest;
    layer.influenced_vertex_count = vertex_count;
    layer.total_influence_count = total_influences;
    layer.provenance = character::WeightProvenance::automatic_proposal;
    layer.provenance_digest = recipe_digest.value();
    if (auto result = layer.validate(); !result) {
        return core::Result<AutoWeightOutput>::failure(result.error());
    }

    AutoWeightOutput output;
    output.binding = std::move(binding);
    output.layer = std::move(layer);
    output.receipt = std::move(receipt);
    return core::Result<AutoWeightOutput>::success(std::move(output));
}

} // namespace carto::weights
