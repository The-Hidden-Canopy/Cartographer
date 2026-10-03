#include <carto/web_geometry/evaluation.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::web_geometry {

namespace {

using Stage = EvaluationStage;

constexpr std::array<std::string_view, kEvaluationStageCount> kStageTypeIds = {
    "carto.web_geometry.compile_mesh",
    "carto.web_geometry.surface_metadata",
    "carto.web_geometry.leaf_clusters",
    "carto.web_geometry.hierarchy",
    "carto.web_geometry.page_pack",
    "carto.web_geometry.package",
};

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::string stage_digest(std::string_view label, std::string_view body) {
    std::ostringstream canonical;
    canonical << label << '\n' << body;
    return digest_text(canonical.str()).hex();
}

std::string cluster_digest_body(const WebGeometryPackage& package) {
    std::ostringstream body;
    body << "source=" << package.source_revision.value() << '\n'
         << "options=" << package.options.digest().hex() << '\n';
    for (const auto& trace : package.provenance) {
        body << trace.cluster << ':' << trace.content_digest << '\n';
    }
    return body.str();
}

std::string page_digest_body(const WebGeometryPackage& package) {
    std::ostringstream body;
    body << "source=" << package.source_revision.value() << '\n';
    for (const auto& page : package.pages) {
        body << page.id << ':' << page.first_cluster << ':' << page.cluster_count << ':'
             << page.payload_offset << ':' << page.payload_bytes << ':'
             << page.digest.hex() << '\n';
    }
    return body.str();
}

std::string source_digest_body(const WebGeometryPackage& package) {
    std::ostringstream body;
    body << "revision=" << package.source_revision.value() << '\n'
         << "hierarchy_source=" << package.hierarchy.source_revision.value() << '\n';
    for (const auto face : package.hierarchy.source_faces) body << face.value << ',';
    return body.str();
}

eval::NodeTypeDefinition node_type(
    std::string id,
    bool has_input,
    bool has_output) {
    std::vector<eval::PortDescriptor> ports;
    if (has_input) {
        ports.push_back(eval::PortDescriptor{
            eval::PortId{"source"},
            eval::PortDirection::input,
            eval::ValueKind::evaluated_mesh});
    }
    if (has_output) {
        ports.push_back(eval::PortDescriptor{
            eval::PortId{"geometry"},
            eval::PortDirection::output,
            eval::ValueKind::evaluated_mesh});
    }
    return eval::NodeTypeDefinition{eval::NodeTypeId{std::move(id)}, std::move(ports)};
}

core::Result<void> validate_stage_array(
    const std::array<std::string, kEvaluationStageCount>& digests) {
    for (const auto& digest : digests) {
        if (digest.size() != 64U || !assets::Sha256Digest::from_hex(digest)) {
            return core::Result<void>::failure(validation(
                "web geometry evaluation contains an invalid stage digest"));
        }
    }
    return core::Result<void>::success();
}

std::size_t stage_index(Stage stage) noexcept {
    return static_cast<std::size_t>(stage);
}

void append_downstream_stages(
    std::vector<Stage>& stages,
    std::size_t first_dirty) {
    stages.clear();
    for (std::size_t index = first_dirty; index < kEvaluationStageCount; ++index) {
        stages.push_back(static_cast<Stage>(index));
    }
}

} // namespace

bool WebGeometryInvalidation::stage_dirty(Stage stage) const noexcept {
    return std::find(stages.begin(), stages.end(), stage) != stages.end();
}

core::Result<void> WebGeometryEvaluation::validate() const {
    if (source_revision.exhausted() || options_digest.empty() || package_digest.empty()) {
        return core::Result<void>::failure(validation(
            "web geometry evaluation identity is incomplete"));
    }
    if (graph.validate().has_value() == false || graph.node_count() != kEvaluationStageCount ||
        graph.connection_count() != kEvaluationStageCount - 1U) {
        return core::Result<void>::failure(validation(
            "web geometry evaluation graph structure is invalid"));
    }
    std::set<eval::NodeId> unique_nodes;
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const auto node = nodes[index];
        const auto* instance = graph.find_node(node);
        if (!node || instance == nullptr || !unique_nodes.insert(node).second ||
            instance->type.value != kStageTypeIds[index]) {
            return core::Result<void>::failure(validation(
                "web geometry evaluation graph node identity is invalid"));
        }
    }
    if (auto result = validate_stage_array(output_digests); !result) return result;
    if (options_digest.size() != 64U || !assets::Sha256Digest::from_hex(options_digest) ||
        package_digest.size() != 64U || !assets::Sha256Digest::from_hex(package_digest)) {
        return core::Result<void>::failure(validation(
            "web geometry evaluation contains an invalid package identity"));
    }
    if (output_digests.back() != package_digest || cluster_digests.empty()) {
        return core::Result<void>::failure(validation(
            "web geometry evaluation package or cluster identity is incomplete"));
    }
    for (const auto& digest : cluster_digests) {
        if (digest.size() != 64U || !assets::Sha256Digest::from_hex(digest)) {
            return core::Result<void>::failure(validation(
                "web geometry evaluation contains an invalid cluster digest"));
        }
    }
    std::set<RuntimePageId> pages;
    for (const auto& page : page_digests) {
        if (page.page == kUnassignedRuntimePage || page.digest.is_zero() ||
            !pages.insert(page.page).second) {
            return core::Result<void>::failure(validation(
                "web geometry evaluation contains an invalid page identity"));
        }
    }
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        const auto* instance = graph.find_node(nodes[index]);
        if (instance == nullptr || instance->parameters_digest != output_digests[index]) {
            return core::Result<void>::failure(validation(
                "web geometry evaluation node parameter identity is stale"));
        }
    }
    const auto snapshot = graph.snapshot();
    if (!snapshot || snapshot.value().connections.size() != kEvaluationStageCount - 1U) {
        return core::Result<void>::failure(validation(
            "web geometry evaluation graph snapshot is invalid"));
    }
    for (std::size_t index = 1U; index < kEvaluationStageCount; ++index) {
        const eval::Connection expected{
            nodes[index - 1U],
            eval::PortId{"geometry"},
            nodes[index],
            eval::PortId{"source"},
        };
        if (std::find(
                snapshot.value().connections.begin(),
                snapshot.value().connections.end(),
                expected) == snapshot.value().connections.end()) {
            return core::Result<void>::failure(validation(
                "web geometry evaluation graph chain is incomplete"));
        }
    }
    if (graph.content_digest().size() != 64U) {
        return core::Result<void>::failure(validation(
            "web geometry evaluation graph digest is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<WebGeometryEvaluation> publish_evaluation(
    const WebGeometryPackage& package) {
    if (auto result = package.validate(); !result) {
        return core::Result<WebGeometryEvaluation>::failure(result.error());
    }

    WebGeometryEvaluation publication;
    publication.source_revision = package.source_revision;
    publication.options_digest = package.options.digest().hex();
    publication.package_digest = package.digest.hex();
    publication.cluster_digests.reserve(package.provenance.size());
    for (const auto& trace : package.provenance) {
        publication.cluster_digests.push_back(trace.content_digest);
    }
    publication.page_digests.reserve(package.pages.size());
    for (const auto& page : package.pages) {
        publication.page_digests.push_back(PageDigest{page.id, page.digest});
    }

    publication.output_digests[stage_index(Stage::compile_mesh)] = stage_digest(
        "carto.web_geometry.compile_mesh", source_digest_body(package));
    publication.output_digests[stage_index(Stage::surface_metadata)] = stage_digest(
        "carto.web_geometry.surface_metadata",
        publication.output_digests[stage_index(Stage::compile_mesh)] +
            "\noptions=" + publication.options_digest);
    publication.output_digests[stage_index(Stage::leaf_clusters)] = stage_digest(
        "carto.web_geometry.leaf_clusters", cluster_digest_body(package));
    publication.output_digests[stage_index(Stage::hierarchy)] = package.hierarchy.digest.hex();
    publication.output_digests[stage_index(Stage::page_pack)] = stage_digest(
        "carto.web_geometry.page_pack", page_digest_body(package));
    publication.output_digests[stage_index(Stage::package)] = publication.package_digest;

    for (std::size_t index = 0U; index < kStageTypeIds.size(); ++index) {
        const auto type = node_type(
            std::string(kStageTypeIds[index]),
            index != 0U,
            index + 1U < kStageTypeIds.size());
        if (auto result = publication.graph.register_type(type); !result) {
            return core::Result<WebGeometryEvaluation>::failure(result.error());
        }
    }
    for (std::size_t index = 0U; index < kEvaluationStageCount; ++index) {
        const auto node = publication.graph.create_node(
            eval::NodeTypeId{std::string(kStageTypeIds[index])},
            publication.output_digests[index]);
        if (!node) return core::Result<WebGeometryEvaluation>::failure(node.error());
        publication.nodes[index] = node.value();
        if (index == 0U) continue;
        const auto connection = publication.graph.connect(eval::Connection{
            publication.nodes[index - 1U],
            eval::PortId{"geometry"},
            publication.nodes[index],
            eval::PortId{"source"},
        });
        if (!connection) {
            return core::Result<WebGeometryEvaluation>::failure(connection.error());
        }
    }
    if (auto result = publication.validate(); !result) {
        return core::Result<WebGeometryEvaluation>::failure(result.error());
    }
    return core::Result<WebGeometryEvaluation>::success(std::move(publication));
}

core::Result<WebGeometryInvalidation> compute_invalidation(
    const WebGeometryEvaluation& before,
    const WebGeometryEvaluation& after) {
    if (auto result = before.validate(); !result) {
        return core::Result<WebGeometryInvalidation>::failure(result.error());
    }
    if (auto result = after.validate(); !result) {
        return core::Result<WebGeometryInvalidation>::failure(result.error());
    }

    WebGeometryInvalidation invalidation;
    std::size_t first_dirty = kEvaluationStageCount;
    for (std::size_t index = 0U; index < kEvaluationStageCount; ++index) {
        if (before.output_digests[index] != after.output_digests[index]) {
            first_dirty = std::min(first_dirty, index);
        }
    }
    if (first_dirty < kEvaluationStageCount) {
        append_downstream_stages(invalidation.stages, first_dirty);
    }

    const std::size_t cluster_count = std::max(
        before.cluster_digests.size(), after.cluster_digests.size());
    for (std::size_t index = 0U; index < cluster_count; ++index) {
        const bool before_present = index < before.cluster_digests.size();
        const bool after_present = index < after.cluster_digests.size();
        if (!before_present || !after_present ||
            before.cluster_digests[index] != after.cluster_digests[index]) {
            if (index > std::numeric_limits<ClusterId>::max()) {
                return core::Result<WebGeometryInvalidation>::failure(validation(
                    "web geometry cluster invalidation exceeds the identity bound"));
            }
            invalidation.clusters.push_back(static_cast<ClusterId>(index));
        }
    }

    std::map<RuntimePageId, assets::Sha256Digest> before_pages;
    std::map<RuntimePageId, assets::Sha256Digest> after_pages;
    for (const auto& page : before.page_digests) before_pages.emplace(page.page, page.digest);
    for (const auto& page : after.page_digests) after_pages.emplace(page.page, page.digest);
    std::set<RuntimePageId> page_ids;
    for (const auto& [page, ignored] : before_pages) {
        static_cast<void>(ignored);
        page_ids.insert(page);
    }
    for (const auto& [page, ignored] : after_pages) {
        static_cast<void>(ignored);
        page_ids.insert(page);
    }
    for (const auto page : page_ids) {
        const auto before_page = before_pages.find(page);
        const auto after_page = after_pages.find(page);
        if (before_page == before_pages.end() || after_page == after_pages.end() ||
            before_page->second != after_page->second) {
            invalidation.pages.push_back(page);
        }
    }
    return core::Result<WebGeometryInvalidation>::success(std::move(invalidation));
}

} // namespace carto::web_geometry
