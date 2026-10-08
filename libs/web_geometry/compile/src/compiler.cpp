#include <carto/web_geometry/compiler.hpp>

#include <carto/geometry/compiled_mesh.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace carto::web_geometry {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

struct TriangleInfo {
    std::size_t index = 0U;
    geometry::FaceId face;
    core::Bounds3d bounds;
    core::Vec3d centroid{};
    core::Vec3d normal{};
    double area = 0.0;
    std::uint32_t material_group = 0U;
    std::uint32_t semantic_group = 0U;
    std::uint32_t lock_group = 0U;
    std::uint32_t break_group = 0U;
    bool allow_merge = true;
};

const attributes::AttributeLayer* find_one_layer(
    const attributes::AttributeSet& set,
    std::initializer_list<std::string_view> ids,
    core::Result<void>& status) {
    const attributes::AttributeLayer* found = nullptr;
    for (const auto id : ids) {
        const auto* candidate = set.find(id);
        if (candidate == nullptr) continue;
        if (found != nullptr) {
            status = core::Result<void>::failure(validation(
                "web geometry attribute aliases are ambiguous"));
            return nullptr;
        }
        found = candidate;
    }
    return found;
}

core::Result<std::uint32_t> discrete_value(
    const attributes::AttributeLayer* layer,
    std::size_t index,
    std::string_view label) {
    if (layer == nullptr) return core::Result<std::uint32_t>::success(0U);
    if (index >= layer->values.size()) {
        return core::Result<std::uint32_t>::failure(validation(
            std::string("web geometry attribute index is out of range: ") +
            std::string(label)));
    }
    const auto& value = layer->values[index];
    if (const auto* number = std::get_if<std::uint32_t>(&value); number != nullptr) {
        return core::Result<std::uint32_t>::success(*number);
    }
    if (const auto* number = std::get_if<std::int32_t>(&value); number != nullptr) {
        if (*number < 0) {
            return core::Result<std::uint32_t>::failure(invalid(
                std::string("web geometry attribute is negative: ") + std::string(label)));
        }
        return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(*number));
    }
    if (const auto* number = std::get_if<std::uint64_t>(&value); number != nullptr) {
        if (*number > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<std::uint32_t>::failure(validation(
                std::string("web geometry attribute exceeds uint32: ") + std::string(label)));
        }
        return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(*number));
    }
    return core::Result<std::uint32_t>::failure(invalid(
        std::string("web geometry attribute must be a discrete integer: ") +
        std::string(label)));
}

core::Result<bool> boolean_value(
    const attributes::AttributeLayer* layer,
    std::size_t index,
    std::string_view label) {
    if (layer == nullptr) return core::Result<bool>::success(true);
    if (index >= layer->values.size()) {
        return core::Result<bool>::failure(validation(
            std::string("web geometry attribute index is out of range: ") +
            std::string(label)));
    }
    const auto& value = layer->values[index];
    if (const auto* boolean = std::get_if<bool>(&value); boolean != nullptr) {
        return core::Result<bool>::success(*boolean);
    }
    const auto discrete = discrete_value(layer, index, label);
    if (!discrete) return core::Result<bool>::failure(discrete.error());
    return core::Result<bool>::success(discrete.value() != 0U);
}

std::uint64_t digest_prefix(const assets::Sha256Digest& digest) {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value = (value << 8U) | digest.bytes[index];
    }
    return value;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

ClusterBounds bounds_for_triangles(
    const geometry::CompiledMesh& mesh,
    const std::vector<std::size_t>& triangles) {
    ClusterBounds result;
    core::Vec3d center_sum{};
    std::size_t point_count = 0U;
    for (const auto triangle : triangles) {
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const auto point = mesh.positions[mesh.indices[triangle * 3U + corner]];
            result.aabb.include(point);
            center_sum = center_sum + point;
            ++point_count;
        }
    }
    result.sphere_center = center_sum * (1.0 / static_cast<double>(point_count));
    result.sphere_radius = 0.0;
    for (const auto triangle : triangles) {
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const auto point = mesh.positions[mesh.indices[triangle * 3U + corner]];
            result.sphere_radius = std::max(
                result.sphere_radius, (point - result.sphere_center).length());
        }
    }
    return result;
}

bool compatible(
    const TriangleInfo& candidate,
    const std::vector<const TriangleInfo*>& current,
    const WebGeometryCompileOptions& options) {
    if (!candidate.allow_merge || current.empty()) return current.empty();
    for (const auto* existing : current) {
        if (options.preserve_material_boundaries &&
            candidate.material_group != existing->material_group) return false;
        if (options.preserve_semantic_boundaries &&
            candidate.semantic_group != existing->semantic_group) return false;
        if (options.preserve_authored_break_boundaries &&
            (candidate.break_group != existing->break_group) &&
            (candidate.break_group != 0U || existing->break_group != 0U)) return false;
        if (options.preserve_hard_normal_boundaries &&
            core::dot(candidate.normal, existing->normal) < 0.999999) return false;
        if (!existing->allow_merge) return false;
    }
    return true;
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_f64(std::vector<std::uint8_t>& output, double value) {
    append_u64(output, std::bit_cast<std::uint64_t>(value));
}

void write_u32_at(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.at(offset + shift / 8U) =
            static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }
}

std::vector<std::uint8_t> encode_cluster_payload(
    const geometry::CompiledMesh& mesh,
    const ClusterHierarchy& hierarchy,
    const ClusterRecord& cluster) {
    std::vector<std::uint8_t> output;
    output.reserve(8U + static_cast<std::size_t>(cluster.triangle_count) * 160U);
    append_u32(output, cluster.id);
    append_u32(output, cluster.triangle_count);
    for (std::uint32_t offset = 0U; offset < cluster.triangle_count; ++offset) {
        const auto triangle = hierarchy.triangle_indices[cluster.first_triangle + offset];
        append_u32(output, triangle);
        append_u64(output, hierarchy.source_faces[triangle].value);
        for (std::size_t corner = 0U; corner < 3U; ++corner) {
            const auto vertex = mesh.indices[triangle * 3U + corner];
            const auto& position = mesh.positions[vertex];
            const auto& normal = mesh.normals[vertex];
            append_f64(output, position.x);
            append_f64(output, position.y);
            append_f64(output, position.z);
            append_f64(output, normal.x);
            append_f64(output, normal.y);
            append_f64(output, normal.z);
        }
    }
    return output;
}

core::Result<void> pack_runtime_pages(
    const geometry::CompiledMesh& mesh,
    WebGeometryPackage& package) {
    constexpr std::size_t kPageHeaderBytes = 12U;
    constexpr std::size_t kMaxPackagePayloadBytes = 256U * 1024U * 1024U;
    std::vector<std::uint8_t> current_payload;
    std::size_t current_first_cluster = 0U;
    std::size_t current_cluster_count = 0U;
    core::Bounds3d current_bounds;
    RuntimePageId next_page_id = 0U;

    const auto finish_page = [&]() -> core::Result<void> {
        if (current_cluster_count == 0U) return core::Result<void>::success();
        if (package.payload.size() > kMaxPackagePayloadBytes - current_payload.size()) {
            return core::Result<void>::failure(validation(
                "web geometry runtime page payload exceeds the package bound"));
        }
        RuntimeGeometryPage page;
        page.id = next_page_id++;
        page.first_cluster = static_cast<std::uint32_t>(current_first_cluster);
        page.cluster_count = static_cast<std::uint32_t>(current_cluster_count);
        page.payload_offset = static_cast<std::uint64_t>(package.payload.size());
        page.payload_bytes = static_cast<std::uint64_t>(current_payload.size());
        page.bounds = current_bounds;
        package.payload.insert(
            package.payload.end(), current_payload.begin(), current_payload.end());
        package.pages.push_back(page);
        current_payload.clear();
        current_cluster_count = 0U;
        current_bounds = {};
        return core::Result<void>::success();
    };

    for (std::size_t cluster_index = 0U;
         cluster_index < package.hierarchy.clusters.size(); ++cluster_index) {
        auto& cluster = package.hierarchy.clusters[cluster_index];
        if (cluster.triangle_count == 0U) {
            if (auto result = finish_page(); !result) return result;
            continue;
        }
        const auto cluster_payload = encode_cluster_payload(mesh, package.hierarchy, cluster);
        if (cluster_payload.size() > package.options.hard_max_page_payload_bytes - kPageHeaderBytes) {
            return core::Result<void>::failure(validation(
                "web geometry cluster payload exceeds the page hard cap"));
        }
        if (current_cluster_count == 0U) {
            current_first_cluster = cluster_index;
            current_payload.insert(
                current_payload.end(), kRuntimePagePayloadMagic.begin(),
                kRuntimePagePayloadMagic.end());
            append_u32(current_payload, kRuntimePagePayloadVersion);
            append_u32(current_payload, 0U);
        } else if (current_payload.size() > package.options.hard_max_page_payload_bytes ||
                   cluster_payload.size() >
                       package.options.hard_max_page_payload_bytes - current_payload.size() ||
                   current_payload.size() + cluster_payload.size() >
                       package.options.target_page_payload_bytes) {
            if (auto result = finish_page(); !result) return result;
            current_first_cluster = cluster_index;
            current_payload.insert(
                current_payload.end(), kRuntimePagePayloadMagic.begin(),
                kRuntimePagePayloadMagic.end());
            append_u32(current_payload, kRuntimePagePayloadVersion);
            append_u32(current_payload, 0U);
        }
        current_payload.insert(
            current_payload.end(), cluster_payload.begin(), cluster_payload.end());
        ++current_cluster_count;
        write_u32_at(current_payload, 8U, static_cast<std::uint32_t>(current_cluster_count));
        current_bounds.include(cluster.bounds.aabb.minimum);
        current_bounds.include(cluster.bounds.aabb.maximum);
        cluster.page = next_page_id;
    }
    return finish_page();
}

} // namespace

core::Result<ProvenanceContext> ProvenanceContext::from_compiled_mesh(
    const geometry::CompiledMesh& mesh) {
    if (!mesh.valid()) {
        return core::Result<ProvenanceContext>::failure(
            validation("cannot derive web geometry provenance from an invalid compiled mesh"));
    }
    std::set<geometry::FaceId> unique(mesh.triangle_faces.begin(), mesh.triangle_faces.end());
    ProvenanceContext context;
    context.source_revision = mesh.source_revision;
    context.source_faces.assign(unique.begin(), unique.end());
    return core::Result<ProvenanceContext>::success(std::move(context));
}

core::Result<void> ProvenanceContext::validate(const geometry::CompiledMesh& mesh) const {
    if (!mesh.valid()) {
        return core::Result<void>::failure(
            validation("web geometry provenance source mesh is invalid"));
    }
    if (source_revision != mesh.source_revision) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::stale_data,
                "web geometry provenance revision does not match compiled mesh"));
    }
    if (source_faces.empty()) {
        return core::Result<void>::failure(
            invalid("web geometry provenance must name source faces"));
    }
    std::set<geometry::FaceId> expected(mesh.triangle_faces.begin(), mesh.triangle_faces.end());
    std::set<geometry::FaceId> actual(source_faces.begin(), source_faces.end());
    if (expected != actual || actual.size() != source_faces.size()) {
        return core::Result<void>::failure(
            validation("web geometry provenance face coverage is incomplete or duplicated"));
    }
    return core::Result<void>::success();
}

core::Result<WebGeometryPackage> compile_web_geometry(
    const geometry::CompiledMesh& mesh,
    const attributes::AttributeSet& attributes,
    const WebGeometryCompileOptions& options,
    const ProvenanceContext& provenance) {
    if (auto result = options.validate(); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (!mesh.valid()) {
        return core::Result<WebGeometryPackage>::failure(
            validation("web geometry compiler received an invalid compiled mesh"));
    }
    if (auto result = attributes.validate(); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (auto result = provenance.validate(mesh); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (mesh.triangle_faces.empty()) {
        return core::Result<WebGeometryPackage>::failure(
            validation("web geometry compiler does not publish empty meshes"));
    }

    std::map<geometry::FaceId, std::size_t> face_indices;
    const std::set<geometry::FaceId> unique_faces(
        mesh.triangle_faces.begin(), mesh.triangle_faces.end());
    std::size_t face_index = 0U;
    for (const auto face : unique_faces) face_indices.emplace(face, face_index++);
    if (const auto count = attributes.domain_count(attributes::AttributeDomain::face);
        count.has_value() && *count != face_indices.size()) {
        return core::Result<WebGeometryPackage>::failure(validation(
            "web geometry face attribute cardinality does not match compiled faces"));
    }

    core::Result<void> layer_status = core::Result<void>::success();
    const auto* material_layer = find_one_layer(
        attributes, {"web.cluster.material_class", "material.class_id"}, layer_status);
    const auto* semantic_layer = find_one_layer(
        attributes, {"web.cluster.semantic_class", "semantic.class_id"}, layer_status);
    const auto* lock_layer = find_one_layer(
        attributes, {"web.cluster.lock_group"}, layer_status);
    const auto* break_layer = find_one_layer(
        attributes, {"web.cluster.break_group", "web.break_boundary"}, layer_status);
    const auto* allow_layer = find_one_layer(
        attributes, {"web.cluster.allow_merge"}, layer_status);
    if (!layer_status) return core::Result<WebGeometryPackage>::failure(layer_status.error());
    const auto read_layer_domain = [](const attributes::AttributeLayer* layer) -> core::Result<void> {
        if (layer != nullptr && layer->descriptor.domain != attributes::AttributeDomain::face) {
            return core::Result<void>::failure(invalid(
                "web geometry cluster control layers must use the face domain"));
        }
        return core::Result<void>::success();
    };
    for (const auto* layer : {material_layer, semantic_layer, lock_layer, break_layer, allow_layer}) {
        if (auto result = read_layer_domain(layer); !result) {
            return core::Result<WebGeometryPackage>::failure(result.error());
        }
    }

    std::vector<TriangleInfo> triangles;
    triangles.reserve(mesh.triangle_faces.size());
    std::map<geometry::EdgeId, std::vector<std::size_t>> edge_triangles;
    std::map<geometry::FaceId, std::vector<std::size_t>> face_triangles;
    for (std::size_t index = 0U; index < mesh.triangle_faces.size(); ++index) {
        const auto first = mesh.indices[index * 3U];
        const auto second = mesh.indices[index * 3U + 1U];
        const auto third = mesh.indices[index * 3U + 2U];
        const core::Vec3d a = mesh.positions[first];
        const core::Vec3d b = mesh.positions[second];
        const core::Vec3d c = mesh.positions[third];
        const core::Vec3d cross = core::cross(b - a, c - a);
        const double cross_length = cross.length();
        if (!std::isfinite(cross_length) || cross_length <= 1e-12) {
            return core::Result<WebGeometryPackage>::failure(validation(
                "web geometry compiler received a zero-area triangle"));
        }
        TriangleInfo info;
        info.index = index;
        info.face = mesh.triangle_faces[index];
        info.bounds.include(a);
        info.bounds.include(b);
        info.bounds.include(c);
        info.centroid = (a + b + c) * (1.0 / 3.0);
        info.normal = cross * (1.0 / cross_length);
        info.area = cross_length * 0.5;
        const auto face_attribute_index = face_indices.at(info.face);
        if (auto value = discrete_value(material_layer, face_attribute_index, "material"); !value) {
            return core::Result<WebGeometryPackage>::failure(value.error());
        } else info.material_group = value.value();
        if (auto value = discrete_value(semantic_layer, face_attribute_index, "semantic"); !value) {
            return core::Result<WebGeometryPackage>::failure(value.error());
        } else info.semantic_group = value.value();
        if (auto value = discrete_value(lock_layer, face_attribute_index, "lock_group"); !value) {
            return core::Result<WebGeometryPackage>::failure(value.error());
        } else info.lock_group = value.value();
        if (auto value = discrete_value(break_layer, face_attribute_index, "break_group"); !value) {
            return core::Result<WebGeometryPackage>::failure(value.error());
        } else info.break_group = value.value();
        if (auto value = boolean_value(allow_layer, face_attribute_index, "allow_merge"); !value) {
            return core::Result<WebGeometryPackage>::failure(value.error());
        } else info.allow_merge = value.value();
        triangles.push_back(info);
        face_triangles[info.face].push_back(index);
        for (const auto& edge : mesh.triangle_edges[index]) {
            if (!edge.has_value()) continue;
            edge_triangles[*edge].push_back(index);
        }
    }
    for (const auto& [edge, connected] : edge_triangles) {
        static_cast<void>(edge);
        if (connected.size() > 2U) {
            return core::Result<WebGeometryPackage>::failure(validation(
                "web geometry compiler rejects non-manifold triangle adjacency"));
        }
    }
    std::vector<std::set<std::size_t>> adjacency(triangles.size());
    for (const auto& [ignored_face, connected] : face_triangles) {
        static_cast<void>(ignored_face);
        for (std::size_t first = 0U; first < connected.size(); ++first) {
            for (std::size_t second = first + 1U; second < connected.size(); ++second) {
                adjacency[connected[first]].insert(connected[second]);
                adjacency[connected[second]].insert(connected[first]);
            }
        }
    }
    for (const auto& [ignored_edge, connected] : edge_triangles) {
        static_cast<void>(ignored_edge);
        if (connected.size() != 2U) continue;
        adjacency[connected[0]].insert(connected[1]);
        adjacency[connected[1]].insert(connected[0]);
    }

    std::map<std::uint32_t, std::size_t> lock_counts;
    for (const auto& triangle : triangles) {
        if (triangle.lock_group != 0U) ++lock_counts[triangle.lock_group];
    }
    for (const auto& [group, count] : lock_counts) {
        static_cast<void>(group);
        if (count > options.hard_max_triangles_per_leaf) {
            return core::Result<WebGeometryPackage>::failure(validation(
                "web geometry authored lock group exceeds the leaf hard cap"));
        }
    }

    std::vector<std::vector<std::size_t>> leaf_triangles;
    std::vector<bool> assigned(triangles.size(), false);
    for (std::size_t seed = 0U; seed < triangles.size(); ++seed) {
        if (assigned[seed]) continue;
        std::vector<std::size_t> members;
        if (triangles[seed].lock_group != 0U) {
            for (std::size_t index = 0U; index < triangles.size(); ++index) {
                if (triangles[index].lock_group == triangles[seed].lock_group) {
                    members.push_back(index);
                }
            }
            std::sort(members.begin(), members.end());
            std::vector<const TriangleInfo*> controls;
            for (const auto index : members) controls.push_back(&triangles[index]);
            for (const auto* member : controls) {
                if (!compatible(*member, controls, options)) {
                    return core::Result<WebGeometryPackage>::failure(validation(
                        "web geometry lock group crosses a hard cluster boundary"));
                }
            }
        } else {
            members.push_back(seed);
            assigned[seed] = true;
            std::vector<const TriangleInfo*> controls{&triangles[seed]};
            while (members.size() < options.hard_max_triangles_per_leaf) {
                std::set<std::size_t> candidates;
                for (const auto current : members) {
                    for (const auto candidate : adjacency[current]) {
                        if (!assigned[candidate]) candidates.insert(candidate);
                    }
                }
                std::optional<std::size_t> selected;
                for (const auto candidate : candidates) {
                    if (members.size() >= options.target_triangles_per_leaf &&
                        triangles[candidate].lock_group == 0U) break;
                    if (compatible(triangles[candidate], controls, options)) {
                        selected = candidate;
                        break;
                    }
                }
                if (!selected.has_value()) break;
                members.push_back(*selected);
                assigned[*selected] = true;
                controls.push_back(&triangles[*selected]);
            }
        }
        for (const auto index : members) assigned[index] = true;
        leaf_triangles.push_back(std::move(members));
    }

    WebGeometryPackage package;
    package.source_revision = mesh.source_revision;
    package.options = options;
    package.hierarchy.source_revision = mesh.source_revision;
    package.hierarchy.digest = {};
    package.hierarchy.triangle_indices.reserve(mesh.indices.size() / 3U);
    package.hierarchy.source_faces.reserve(triangles.size());
    for (const auto& triangle : triangles) package.hierarchy.source_faces.push_back(triangle.face);
    const std::string options_digest = options.digest().hex();
    for (std::size_t leaf_index = 0U; leaf_index < leaf_triangles.size(); ++leaf_index) {
        const auto& members = leaf_triangles[leaf_index];
        ClusterRecord record;
        record.id = static_cast<ClusterId>(leaf_index);
        record.first_triangle = static_cast<std::uint32_t>(
            package.hierarchy.triangle_indices.size());
        record.triangle_count = static_cast<std::uint32_t>(members.size());
        record.bounds = bounds_for_triangles(mesh, members);
        record.material_group = triangles[members.front()].material_group;
        record.semantic_group = triangles[members.front()].semantic_group;
        std::ostringstream content;
        content << "leaf=" << record.id << ';';
        for (const auto triangle : members) {
            package.hierarchy.triangle_indices.push_back(static_cast<std::uint32_t>(triangle));
            content << triangle << ':' << triangles[triangle].face.value << ';';
        }
        record.content_digest = digest_prefix(digest_text(content.str()));
        package.hierarchy.clusters.push_back(record);
        ClusterProvenance trace;
        trace.cluster = record.id;
        trace.source_revision = mesh.source_revision;
        trace.compiler_options_digest = options_digest;
        for (const auto triangle : members) trace.source_faces.push_back(triangles[triangle].face);
        std::sort(trace.source_faces.begin(), trace.source_faces.end());
        trace.source_faces.erase(std::unique(trace.source_faces.begin(), trace.source_faces.end()),
                                 trace.source_faces.end());
        trace.content_digest = digest_text(content.str()).hex();
        package.provenance.push_back(std::move(trace));
    }

    const ClusterId root_id = static_cast<ClusterId>(package.hierarchy.clusters.size());
    ClusterRecord root;
    root.id = root_id;
    root.first_child = 0U;
    root.child_count = static_cast<std::uint32_t>(leaf_triangles.size());
    root.bounds = bounds_for_triangles(mesh, [&]() {
        std::vector<std::size_t> all;
        all.reserve(triangles.size());
        for (std::size_t index = 0U; index < triangles.size(); ++index) all.push_back(index);
        return all;
    }());
    root.geometric_error = 0.0;
    package.hierarchy.child_ids.reserve(leaf_triangles.size());
    for (auto& cluster : package.hierarchy.clusters) {
        cluster.parent = root_id;
        package.hierarchy.child_ids.push_back(cluster.id);
    }
    std::ostringstream root_content;
    root_content << "root=" << root_id << ';';
    for (const auto& cluster : package.hierarchy.clusters) {
        root_content << cluster.id << ':' << cluster.content_digest << ';';
    }
    for (const auto face : provenance.source_faces) root_content << face.value << ',';
    const auto root_content_digest = digest_text(root_content.str());
    root.content_digest = digest_prefix(root_content_digest);
    package.hierarchy.clusters.push_back(root);
    ClusterProvenance root_trace;
    root_trace.cluster = root_id;
    root_trace.source_revision = mesh.source_revision;
    root_trace.compiler_options_digest = options_digest;
    root_trace.source_faces = provenance.source_faces;
    root_trace.content_digest = root_content_digest.hex();
    package.provenance.push_back(std::move(root_trace));

    if (auto result = pack_runtime_pages(mesh, package); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }

    const std::string hierarchy_seed = package.hierarchy.canonical();
    package.hierarchy.digest = digest_text(hierarchy_seed);
    if (auto result = package.hierarchy.validate(); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (auto result = package.refresh_digest(); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    if (auto result = package.validate(); !result) {
        return core::Result<WebGeometryPackage>::failure(result.error());
    }
    return core::Result<WebGeometryPackage>::success(std::move(package));
}

} // namespace carto::web_geometry
