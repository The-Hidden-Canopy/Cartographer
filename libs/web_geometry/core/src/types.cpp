#include <carto/web_geometry/types.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::web_geometry {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool add_overflows(std::uint64_t first, std::uint64_t second) noexcept {
    return second > std::numeric_limits<std::uint64_t>::max() - first;
}

std::string digest_hex(const assets::Sha256Digest& digest) {
    return digest.hex();
}

bool contains_bounds(const core::Bounds3d& outer, const core::Bounds3d& inner) noexcept {
    return outer.minimum.x <= inner.minimum.x && outer.minimum.y <= inner.minimum.y &&
           outer.minimum.z <= inner.minimum.z && outer.maximum.x >= inner.maximum.x &&
           outer.maximum.y >= inner.maximum.y && outer.maximum.z >= inner.maximum.z;
}

std::string digest_prefix_hex(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

bool has_whitespace(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](const char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    });
}

} // namespace

core::Result<void> WebGeometryCompileOptions::validate() const {
    if (target_triangles_per_leaf == 0U || hard_max_triangles_per_leaf == 0U ||
        target_triangles_per_leaf > hard_max_triangles_per_leaf) {
        return core::Result<void>::failure(invalid(
            "web geometry leaf triangle targets are invalid"));
    }
    if (hard_max_triangles_per_leaf > 1'048'576U) {
        return core::Result<void>::failure(validation(
            "web geometry leaf hard cap exceeds the bounded resource limit"));
    }
    if (max_children_per_node < 2U || max_children_per_node > 64U) {
        return core::Result<void>::failure(invalid(
            "web geometry child count must be between 2 and 64"));
    }
    if (target_page_payload_bytes < 12U || hard_max_page_payload_bytes < 12U ||
        target_page_payload_bytes > hard_max_page_payload_bytes) {
        return core::Result<void>::failure(invalid(
            "web geometry page payload targets are invalid"));
    }
    if (hard_max_page_payload_bytes > 64U * 1024U * 1024U) {
        return core::Result<void>::failure(validation(
            "web geometry page payload hard cap exceeds the bounded resource limit"));
    }
    return core::Result<void>::success();
}

std::string WebGeometryCompileOptions::canonical() const {
    std::ostringstream output;
    output << "target=" << target_triangles_per_leaf
           << ";hard=" << hard_max_triangles_per_leaf
           << ";children=" << max_children_per_node
           << ";page_target=" << target_page_payload_bytes
           << ";page_hard=" << hard_max_page_payload_bytes
           << ";material=" << preserve_material_boundaries
           << ";semantic=" << preserve_semantic_boundaries
           << ";normal=" << preserve_hard_normal_boundaries
           << ";authored=" << preserve_authored_break_boundaries;
    return output.str();
}

assets::Sha256Digest WebGeometryCompileOptions::digest() const {
    const std::string value = canonical();
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

core::Result<void> ClusterBounds::validate() const {
    if (!aabb.valid() || !sphere_center.finite() || !std::isfinite(sphere_radius) ||
        sphere_radius < 0.0) {
        return core::Result<void>::failure(
            validation("web geometry cluster bounds are invalid"));
    }
    return core::Result<void>::success();
}

std::string ClusterBounds::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << aabb.minimum.x << ',' << aabb.minimum.y << ',' << aabb.minimum.z << ';'
           << aabb.maximum.x << ',' << aabb.maximum.y << ',' << aabb.maximum.z << ';'
           << sphere_center.x << ',' << sphere_center.y << ',' << sphere_center.z << ';'
           << sphere_radius;
    return output.str();
}

core::Result<void> ClusterHierarchy::validate() const {
    if (source_revision.exhausted() || clusters.empty() || source_faces.empty()) {
        return core::Result<void>::failure(
            validation("web geometry hierarchy is empty or has an invalid source revision"));
    }
    if (triangle_indices.size() != source_faces.size()) {
        return core::Result<void>::failure(
            validation("web geometry triangle/source-face cardinalities differ"));
    }
    if (digest.is_zero()) {
        return core::Result<void>::failure(
            validation("web geometry hierarchy digest is missing"));
    }

    std::vector<std::size_t> child_references(clusters.size(), 0U);
    std::vector<bool> triangle_references(triangle_indices.size(), false);
    std::size_t root_count = 0U;
    for (std::size_t index = 0U; index < clusters.size(); ++index) {
        const auto& cluster = clusters[index];
        if (cluster.id != index ||
            cluster.first_child > child_ids.size() ||
            cluster.child_count > child_ids.size() - cluster.first_child) {
            return core::Result<void>::failure(
                validation("web geometry cluster IDs or child range are invalid"));
        }
        if (cluster.first_triangle > triangle_indices.size() ||
            cluster.triangle_count > triangle_indices.size() - cluster.first_triangle) {
            return core::Result<void>::failure(
                validation("web geometry cluster triangle range is invalid"));
        }
        if (cluster.child_count == 0U && cluster.triangle_count == 0U) {
            return core::Result<void>::failure(
                validation("web geometry hierarchy contains an empty cluster"));
        }
        if (auto result = cluster.bounds.validate(); !result) return result;
        if (!std::isfinite(cluster.geometric_error) || cluster.geometric_error < 0.0) {
            return core::Result<void>::failure(
                validation("web geometry cluster error is invalid"));
        }
        if (cluster.content_digest == 0U) {
            return core::Result<void>::failure(
                validation("web geometry cluster content digest is missing"));
        }
        if (cluster.parent.has_value() && *cluster.parent >= clusters.size()) {
            return core::Result<void>::failure(
                validation("web geometry cluster parent is out of range"));
        }
        if (!cluster.parent.has_value()) ++root_count;
        for (std::uint32_t offset = 0U; offset < cluster.child_count; ++offset) {
            const ClusterId child_id = child_ids.at(cluster.first_child + offset);
            if (child_id >= clusters.size() || !clusters[child_id].parent.has_value() ||
                *clusters[child_id].parent != cluster.id) {
                return core::Result<void>::failure(
                    validation("web geometry parent/child links are inconsistent"));
            }
            ++child_references[child_id];
            if (!contains_bounds(cluster.bounds.aabb, clusters[child_id].bounds.aabb)) {
                return core::Result<void>::failure(
                    validation("web geometry parent bounds do not contain child bounds"));
            }
        }
        for (std::uint32_t offset = 0U; offset < cluster.triangle_count; ++offset) {
            const auto triangle = triangle_indices.at(cluster.first_triangle + offset);
            if (triangle >= source_faces.size() || triangle_references[triangle]) {
                return core::Result<void>::failure(
                    validation("web geometry cluster triangle coverage is invalid"));
            }
            triangle_references[triangle] = true;
        }
    }
    if (root_count != 1U) {
        return core::Result<void>::failure(
            validation("web geometry hierarchy must have exactly one root"));
    }
    for (std::size_t index = 0U; index < clusters.size(); ++index) {
        const bool is_root = !clusters[index].parent.has_value();
        if (is_root && child_references[index] != 0U) {
            return core::Result<void>::failure(
                validation("web geometry root cannot be referenced as a child"));
        }
        if (!is_root && child_references[index] != 1U) {
            return core::Result<void>::failure(
                validation("web geometry non-root cluster is not referenced exactly once"));
        }
    }
    if (std::any_of(triangle_references.begin(), triangle_references.end(),
                    [](const bool referenced) { return !referenced; })) {
        return core::Result<void>::failure(
            validation("web geometry hierarchy does not cover every source triangle"));
    }
    for (const auto& cluster : clusters) {
        std::set<ClusterId> seen;
        std::optional<ClusterId> current = cluster.parent;
        while (current.has_value()) {
            if (!seen.insert(*current).second || *current == cluster.id) {
                return core::Result<void>::failure(
                    validation("web geometry hierarchy contains a parent cycle"));
            }
            current = clusters[*current].parent;
        }
    }
    ClusterHierarchy without_digest = *this;
    without_digest.digest = {};
    const std::string hierarchy_canonical = without_digest.canonical();
    const std::string expected_digest = digest_hex(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(hierarchy_canonical.data()),
            hierarchy_canonical.size()}));
    if (expected_digest != digest_hex(digest)) {
        return core::Result<void>::failure(
            validation("web geometry hierarchy digest does not match canonical content"));
    }
    return core::Result<void>::success();
}

std::string ClusterHierarchy::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "revision=" << source_revision.value() << '\n';
    output << "digest=" << digest_hex(digest) << '\n';
    for (const auto& cluster : clusters) {
        output << "cluster=" << cluster.id << ':';
        if (cluster.parent.has_value()) output << *cluster.parent;
        else output << '-';
        output << ':' << cluster.first_child << ':' << cluster.child_count
               << ':' << cluster.first_triangle << ':' << cluster.triangle_count
               << ':' << cluster.bounds.canonical() << ':' << cluster.geometric_error
               << ':' << cluster.material_group << ':' << cluster.semantic_group
               << ':' << cluster.page << ':' << cluster.content_digest << '\n';
    }
    output << "children=";
    for (const auto child : child_ids) output << child << ',';
    output << "\ntriangles=";
    for (const auto triangle : triangle_indices) output << triangle << ',';
    output << "\nfaces=";
    for (const auto face : source_faces) output << face.value << ',';
    output << '\n';
    return output.str();
}

core::Result<void> ClusterProvenance::validate() const {
    if (source_faces.empty() || source_revision.exhausted() ||
        compiler_options_digest.empty() || content_digest.empty()) {
        return core::Result<void>::failure(
            validation("web geometry cluster provenance is incomplete"));
    }
    std::set<geometry::FaceId> unique(source_faces.begin(), source_faces.end());
    if (unique.size() != source_faces.size() || compiler_options_digest.size() != 64U ||
        content_digest.size() != 64U ||
        !assets::Sha256Digest::from_hex(compiler_options_digest) ||
        !assets::Sha256Digest::from_hex(content_digest)) {
        return core::Result<void>::failure(
            validation("web geometry cluster provenance contains invalid faces or digests"));
    }
    return core::Result<void>::success();
}

std::string ClusterProvenance::canonical() const {
    std::ostringstream output;
    output << "cluster=" << cluster << ";revision=" << source_revision.value()
           << ";options=" << compiler_options_digest << ";content=" << content_digest
           << ";faces=";
    for (const auto face : source_faces) output << face.value << ',';
    return output.str();
}

core::Result<void> RuntimeGeometryPage::validate(std::size_t cluster_total) const {
    if (first_cluster > cluster_total ||
        cluster_count > cluster_total - first_cluster ||
        !bounds.valid() || digest.is_zero() ||
        add_overflows(payload_offset, payload_bytes)) {
        return core::Result<void>::failure(
            validation("web geometry runtime page is invalid"));
    }
    return core::Result<void>::success();
}

std::string RuntimeGeometryPage::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "id=" << id << ";clusters=" << first_cluster << ',' << cluster_count
           << ";payload=" << payload_offset << ',' << payload_bytes
           << ";bounds=" << bounds.minimum.x << ',' << bounds.minimum.y << ','
           << bounds.minimum.z << ',' << bounds.maximum.x << ',' << bounds.maximum.y
           << ',' << bounds.maximum.z << ";digest=" << digest_hex(digest);
    return output.str();
}

core::Result<void> WebGeometryPackage::validate() const {
    if (schema_version != current_schema_version || compiler_version.empty() ||
        compiler_version.size() > 128U || has_whitespace(compiler_version) || digest.is_zero()) {
        return core::Result<void>::failure(
            validation("web geometry package header is invalid"));
    }
    if (auto result = options.validate(); !result) return result;
    if (source_revision != hierarchy.source_revision) {
        return core::Result<void>::failure(
            validation("web geometry package source revisions differ"));
    }
    if (auto result = hierarchy.validate(); !result) return result;
    if (provenance.size() != hierarchy.clusters.size()) {
        return core::Result<void>::failure(
            validation("web geometry package provenance cardinality differs"));
    }
    const std::string options_digest = options.digest().hex();
    std::vector<std::set<geometry::FaceId>> expected_provenance(hierarchy.clusters.size());
    std::vector<bool> provenance_visiting(hierarchy.clusters.size(), false);
    std::vector<bool> provenance_resolved(hierarchy.clusters.size(), false);
    std::function<core::Result<void>(ClusterId)> resolve_provenance =
        [&](const ClusterId cluster_id) -> core::Result<void> {
        if (provenance_resolved[cluster_id]) return core::Result<void>::success();
        if (provenance_visiting[cluster_id]) {
            return core::Result<void>::failure(
                validation("web geometry provenance contains a parent cycle"));
        }
        provenance_visiting[cluster_id] = true;
        const auto& cluster = hierarchy.clusters[cluster_id];
        for (std::uint32_t offset = 0U; offset < cluster.triangle_count; ++offset) {
            const auto triangle = hierarchy.triangle_indices[cluster.first_triangle + offset];
            expected_provenance[cluster_id].insert(hierarchy.source_faces[triangle]);
        }
        for (std::uint32_t offset = 0U; offset < cluster.child_count; ++offset) {
            const auto child_id = hierarchy.child_ids[cluster.first_child + offset];
            if (auto result = resolve_provenance(child_id); !result) return result;
            expected_provenance[cluster_id].insert(
                expected_provenance[child_id].begin(), expected_provenance[child_id].end());
        }
        provenance_visiting[cluster_id] = false;
        provenance_resolved[cluster_id] = true;
        return core::Result<void>::success();
    };
    for (ClusterId cluster_id = 0U;
         cluster_id < static_cast<ClusterId>(hierarchy.clusters.size()); ++cluster_id) {
        if (auto result = resolve_provenance(cluster_id); !result) return result;
    }
    for (std::size_t index = 0U; index < provenance.size(); ++index) {
        if (auto result = provenance[index].validate(); !result) return result;
        if (provenance[index].cluster != index ||
            provenance[index].source_revision != source_revision) {
            return core::Result<void>::failure(
                validation("web geometry package provenance identity is inconsistent"));
        }
        const std::set<geometry::FaceId> actual(
            provenance[index].source_faces.begin(), provenance[index].source_faces.end());
        if (provenance[index].compiler_options_digest != options_digest ||
            actual != expected_provenance[index]) {
            return core::Result<void>::failure(
                validation("web geometry package provenance does not match cluster content"));
        }
        if (provenance[index].content_digest.substr(0U, 16U) !=
            digest_prefix_hex(hierarchy.clusters[index].content_digest)) {
            return core::Result<void>::failure(
                validation("web geometry package provenance content digest is inconsistent"));
        }
    }

    std::vector<RuntimePageId> cluster_pages(hierarchy.clusters.size(), kUnassignedRuntimePage);
    std::set<RuntimePageId> page_ids;
    for (const auto& page : pages) {
        if (page.id == kUnassignedRuntimePage || !page_ids.insert(page.id).second ||
            page.first_cluster > hierarchy.clusters.size() ||
            page.cluster_count > hierarchy.clusters.size() - page.first_cluster) {
            return core::Result<void>::failure(
                validation("web geometry runtime page identity or range is invalid"));
        }
        if (auto result = page.validate(hierarchy.clusters.size()); !result) return result;
        if (page.payload_offset > payload.size() ||
            page.payload_bytes > payload.size() - static_cast<std::size_t>(page.payload_offset)) {
            return core::Result<void>::failure(
                validation("web geometry runtime page exceeds package payload"));
        }
        const auto payload_offset = static_cast<std::size_t>(page.payload_offset);
        const auto payload_bytes = static_cast<std::size_t>(page.payload_bytes);
        const auto actual_page_digest = assets::sha256(
            std::span<const std::uint8_t>{payload}.subspan(payload_offset, payload_bytes));
        if (actual_page_digest != page.digest) {
            return core::Result<void>::failure(
                validation("web geometry runtime page digest does not match payload"));
        }
        for (std::uint32_t offset = 0U; offset < page.cluster_count; ++offset) {
            const auto cluster_id = page.first_cluster + offset;
            if (cluster_pages[cluster_id] != kUnassignedRuntimePage) {
                return core::Result<void>::failure(
                    validation("web geometry runtime page ranges overlap"));
            }
            cluster_pages[cluster_id] = page.id;
            if (hierarchy.clusters[cluster_id].page != page.id) {
                return core::Result<void>::failure(
                    validation("web geometry cluster page assignment is inconsistent"));
            }
        }
    }
    for (std::size_t index = 0U; index < hierarchy.clusters.size(); ++index) {
        if (hierarchy.clusters[index].page != kUnassignedRuntimePage &&
            cluster_pages[index] != hierarchy.clusters[index].page) {
            return core::Result<void>::failure(
                validation("web geometry cluster references a missing runtime page"));
        }
    }
    const std::string package_canonical = canonical_without_digest();
    const std::string expected_digest = digest_hex(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(package_canonical.data()),
            package_canonical.size()}));
    if (expected_digest != digest_hex(digest)) {
        return core::Result<void>::failure(
            validation("web geometry package digest does not match canonical content"));
    }
    return core::Result<void>::success();
}

std::string WebGeometryPackage::canonical_without_digest() const {
    std::ostringstream output;
    output << "schema=" << schema_version << "\n"
           << "compiler=" << compiler_version << "\n"
           << "revision=" << source_revision.value() << "\n"
           << "options=" << options.canonical() << "\n"
           << "hierarchy=" << hierarchy.canonical();
    for (const auto& item : provenance) output << "provenance=" << item.canonical() << '\n';
    for (const auto& page : pages) output << "page=" << page.canonical() << '\n';
    output << "payload=" << payload.size() << ':';
    for (const auto byte : payload) output << static_cast<unsigned int>(byte) << ',';
    output << '\n';
    return output.str();
}

core::Result<void> WebGeometryPackage::refresh_digest() {
    if (auto result = options.validate(); !result) return result;
    hierarchy.digest = {};
    const std::string hierarchy_canonical = hierarchy.canonical();
    hierarchy.digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(hierarchy_canonical.data()),
        hierarchy_canonical.size()});
    if (auto result = hierarchy.validate(); !result) return result;
    for (auto& page : pages) {
        if (page.payload_offset > payload.size() ||
            page.payload_bytes > payload.size() - static_cast<std::size_t>(page.payload_offset)) {
            return core::Result<void>::failure(
                validation("web geometry runtime page exceeds package payload"));
        }
        page.digest = assets::sha256(std::span<const std::uint8_t>{payload}.subspan(
            static_cast<std::size_t>(page.payload_offset),
            static_cast<std::size_t>(page.payload_bytes)));
    }
    const std::string canonical = canonical_without_digest();
    digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    return core::Result<void>::success();
}

} // namespace carto::web_geometry
