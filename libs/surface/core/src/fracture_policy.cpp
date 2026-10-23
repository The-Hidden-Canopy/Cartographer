#include <carto/surface/inheritance.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <span>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace carto::surface {

namespace {

constexpr std::size_t kMaxFractureGeometryTriangles = 1'000'000U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

std::string digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}).hex();
}

bool valid_digest(std::string_view value) {
    return value.size() == 64U && assets::Sha256Digest::from_hex(value).has_value();
}

std::string vec3_canonical(core::Vec3d value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << value.x << ',' << value.y << ',' << value.z;
    return output.str();
}

std::string compiled_mesh_canonical(const geometry::CompiledMesh& mesh) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << "compiled-mesh-v1;revision="
           << mesh.source_revision.value() << ";positions=";
    for (const auto& position : mesh.positions) output << vec3_canonical(position) << ';';
    output << ";normals=";
    for (const auto& normal : mesh.normals) output << vec3_canonical(normal) << ';';
    output << ";indices=";
    for (const std::uint32_t index : mesh.indices) output << index << ',';
    output << ";vertex_ids=";
    for (const geometry::VertexId id : mesh.vertex_ids) output << id.value << ',';
    output << ";triangle_faces=";
    for (const geometry::FaceId face : mesh.triangle_faces) output << face.value << ',';
    output << ";triangle_edges=";
    for (const auto& edges : mesh.triangle_edges) {
        for (const auto& edge : edges) output << (edge.has_value() ? edge->value : 0U) << ',';
        output << ';';
    }
    output << ";bounds=" << vec3_canonical(mesh.bounds.minimum) << ':'
           << vec3_canonical(mesh.bounds.maximum);
    return output.str();
}

std::string fracture_shard_id(
    std::string_view representation_digest,
    std::string_view policy_digest,
    std::size_t shard_index,
    const SurfaceFractureShard& shard) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "surface-fracture-shard-v1\n"
              << "representation_digest=" << representation_digest << '\n'
              << "policy_digest=" << policy_digest << '\n'
              << "index=" << shard_index << '\n'
              << "faces=";
    for (const auto face : shard.source_faces) canonical << face.value << ',';
    return digest_text(canonical.str());
}

std::string fracture_outcome_canonical(const RepresentationFracturePolicyOutcome& outcome) {
    std::ostringstream canonical;
    canonical.imbue(std::locale::classic());
    canonical << "surface-fracture-policy-outcome-v1\n"
              << "representation=" << outcome.representation.value << '\n'
              << "representation_digest=" << outcome.representation_digest << '\n'
              << "policy_digest=" << outcome.effective_policy_digest << '\n'
              << "inheritance_digest=" << outcome.inheritance_digest << '\n';
    for (const auto& shard : outcome.shards) {
        canonical << "shard=" << shard.stable_id << ':';
        for (const auto face : shard.source_faces) canonical << face.value << ',';
        canonical << '\n';
    }
    return canonical.str();
}

bool supported_mode(SurfaceFractureMode mode) noexcept {
    return mode == SurfaceFractureMode::balanced_face_id_order;
}

core::Result<void> validate_binding(const RepresentationFracturePolicyBinding& binding) {
    if (!binding.representation || binding.shard_count < 2U || binding.shard_count > 65'536U) {
        return core::Result<void>::failure(
            invalid("fracture policy identity or shard count is invalid"));
    }
    if (!supported_mode(binding.mode)) {
        return core::Result<void>::failure(
            unsupported("fracture mode is not supported by the public reference policy"));
    }
    return core::Result<void>::success();
}

std::string policy_canonical(const RepresentationFracturePolicyBinding& binding) {
    std::ostringstream output;
    output << "surface-fracture-policy-v1\n"
           << "mode=balanced_face_id_order\n"
           << "shard_count=" << binding.shard_count << '\n';
    return output.str();
}

} // namespace

core::Result<RepresentationFracturePolicyOutcome> evaluate_inherited_fracture_policy(
    const SurfaceRepresentationTree& tree,
    const std::vector<RepresentationInheritanceRecord>& inheritance,
    const std::vector<RepresentationFracturePolicyBinding>& bindings,
    const std::vector<geometry::FaceId>& source_faces,
    RepresentationId representation) {
    if (source_faces.empty() || source_faces.size() > 1'000'000U ||
        !std::is_sorted(source_faces.begin(), source_faces.end()) ||
        std::adjacent_find(source_faces.begin(), source_faces.end()) != source_faces.end() ||
        std::any_of(source_faces.begin(), source_faces.end(),
            [](geometry::FaceId id) { return !id; })) {
        return core::Result<RepresentationFracturePolicyOutcome>::failure(
            invalid("fracture source face identities are empty, invalid, or non-canonical"));
    }
    const SurfaceRepresentationNode* target = nullptr;
    std::map<RepresentationId, const SurfaceRepresentationNode*> nodes;
    for (const auto& node : tree.nodes()) {
        nodes.emplace(node.id, &node);
        if (node.id == representation) target = &node;
    }
    if (target == nullptr) {
        return core::Result<RepresentationFracturePolicyOutcome>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "fracture policy target is absent from the representation tree"));
    }
    if (auto valid = tree.validate(target->domain); !valid) {
        return core::Result<RepresentationFracturePolicyOutcome>::failure(valid.error());
    }
    std::map<RepresentationId, const RepresentationInheritanceRecord*> records;
    for (const auto& record : inheritance) {
        if (auto valid = record.validate(); !valid) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(valid.error());
        }
        const auto node = nodes.find(record.representation);
        if (node == nodes.end() || !node->second->parent.has_value() ||
            *node->second->parent != record.parent ||
            !records.emplace(record.representation, &record).second) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(
                invalid("fracture inheritance does not match the representation tree"));
        }
    }
    std::map<RepresentationId, const RepresentationFracturePolicyBinding*> policy_bindings;
    for (const auto& binding : bindings) {
        if (auto valid = validate_binding(binding); !valid) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(valid.error());
        }
        if (!nodes.contains(binding.representation) ||
            !policy_bindings.emplace(binding.representation, &binding).second) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(
                invalid("fracture policy binding has an unknown or duplicate representation"));
        }
    }

    std::vector<const SurfaceRepresentationNode*> path;
    const SurfaceRepresentationNode* current = target;
    while (current != nullptr) {
        path.push_back(current);
        if (!current->parent.has_value()) break;
        const auto parent = nodes.find(*current->parent);
        if (parent == nodes.end()) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(
                invalid("fracture policy ancestry is incomplete"));
        }
        current = parent->second;
    }
    std::reverse(path.begin(), path.end());

    const RepresentationFracturePolicyBinding* effective = nullptr;
    std::string inheritance_material = "surface-fracture-inheritance-v1\n";
    inheritance_material.append("root=");
    inheritance_material.append(std::to_string(path.front()->id.value));
    inheritance_material.push_back('\n');
    for (std::size_t index = 0U; index < path.size(); ++index) {
        const auto* node = path[index];
        const auto binding_it = policy_bindings.find(node->id);
        const RepresentationFracturePolicyBinding* binding =
            binding_it == policy_bindings.end() ? nullptr : binding_it->second;
        if (index == 0U) {
            if (binding == nullptr || records.contains(node->id)) {
                return core::Result<RepresentationFracturePolicyOutcome>::failure(
                    unsupported("fracture policy root has no explicit mode"));
            }
            effective = binding;
            inheritance_material.append("root-policy=");
            inheritance_material.append(digest_text(policy_canonical(*effective)));
            inheritance_material.push_back('\n');
            continue;
        }
        const auto record_it = records.find(node->id);
        if (record_it == records.end()) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(
                unsupported("representation has no fracture inheritance declaration"));
        }
        const auto& record = *record_it->second;
        const bool inherits = std::binary_search(record.inherited_fields.begin(),
            record.inherited_fields.end(), RepresentationPolicyField::fracture);
        const bool overrides = std::binary_search(record.override_fields.begin(),
            record.override_fields.end(), RepresentationPolicyField::fracture);
        const auto encoded = record.serialize();
        if (!encoded) {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(encoded.error());
        }
        inheritance_material.append(encoded.value());
        inheritance_material.push_back('\n');
        if (inherits) {
            if (binding != nullptr &&
                (binding->mode != effective->mode ||
                 binding->shard_count != effective->shard_count)) {
                return core::Result<RepresentationFracturePolicyOutcome>::failure(
                    invalid("inherited fracture policy differs from the parent"));
            }
        } else if (overrides) {
            if (binding == nullptr) {
                return core::Result<RepresentationFracturePolicyOutcome>::failure(
                    unsupported("fracture override has no explicit policy value"));
            }
            effective = binding;
        } else {
            return core::Result<RepresentationFracturePolicyOutcome>::failure(
                unsupported("representation does not inherit or override fracture policy"));
        }
    }
    if (effective == nullptr || effective->shard_count > source_faces.size()) {
        return core::Result<RepresentationFracturePolicyOutcome>::failure(
            invalid("fracture policy cannot create the requested non-empty shards"));
    }

    const std::string policy_digest = digest_text(policy_canonical(*effective));
    const std::string inheritance_digest = digest_text(inheritance_material);
    std::vector<SurfaceFractureShard> shards;
    shards.reserve(effective->shard_count);
    const std::size_t base_count = source_faces.size() / effective->shard_count;
    const std::size_t remainder = source_faces.size() % effective->shard_count;
    std::size_t face_offset = 0U;
    std::ostringstream outcome_canonical;
    outcome_canonical << "surface-fracture-policy-outcome-v1\n"
                      << "representation=" << target->id.value << '\n'
                      << "representation_digest=" << target->content_digest << '\n'
                      << "policy_digest=" << policy_digest << '\n'
                      << "inheritance_digest=" << inheritance_digest << '\n';
    for (std::uint32_t shard_index = 0U; shard_index < effective->shard_count; ++shard_index) {
        const std::size_t shard_size = base_count + (shard_index < remainder ? 1U : 0U);
        SurfaceFractureShard shard;
        shard.source_faces.insert(shard.source_faces.end(),
            source_faces.begin() + static_cast<std::ptrdiff_t>(face_offset),
            source_faces.begin() + static_cast<std::ptrdiff_t>(face_offset + shard_size));
        face_offset += shard_size;
        std::ostringstream shard_canonical;
        shard_canonical << "surface-fracture-shard-v1\n"
                        << "representation_digest=" << target->content_digest << '\n'
                        << "policy_digest=" << policy_digest << '\n'
                        << "index=" << shard_index << '\n'
                        << "faces=";
        for (const auto face : shard.source_faces) shard_canonical << face.value << ',';
        shard.stable_id = digest_text(shard_canonical.str());
        outcome_canonical << "shard=" << shard.stable_id << ':';
        for (const auto face : shard.source_faces) outcome_canonical << face.value << ',';
        outcome_canonical << '\n';
        shards.push_back(std::move(shard));
    }
    RepresentationFracturePolicyOutcome outcome{
        target->id, target->content_digest, policy_digest, inheritance_digest,
        digest_text(outcome_canonical.str()), std::move(shards)};
    return core::Result<RepresentationFracturePolicyOutcome>::success(std::move(outcome));
}

core::Result<SurfaceFractureGeometry> materialize_fracture_geometry(
    const geometry::CompiledMesh& source,
    const RepresentationFracturePolicyOutcome& outcome) {
    if (!source.valid() || source.triangle_faces.empty() ||
        source.triangle_faces.size() > kMaxFractureGeometryTriangles) {
        return core::Result<SurfaceFractureGeometry>::failure(
            invalid("fracture geometry source is invalid, empty, or over the materialization limit"));
    }
    if (!outcome.representation || !valid_digest(outcome.representation_digest) ||
        !valid_digest(outcome.effective_policy_digest) || !valid_digest(outcome.inheritance_digest) ||
        !valid_digest(outcome.digest) || outcome.shards.size() < 2U ||
        outcome.shards.size() > 65'536U ||
        digest_text(fracture_outcome_canonical(outcome)) != outcome.digest) {
        return core::Result<SurfaceFractureGeometry>::failure(
            invalid("fracture policy outcome is invalid or its digest is stale"));
    }

    std::map<geometry::FaceId, std::size_t> face_to_shard;
    std::set<std::string> shard_ids;
    for (std::size_t shard_index = 0U; shard_index < outcome.shards.size(); ++shard_index) {
        const auto& shard = outcome.shards[shard_index];
        if (!valid_digest(shard.stable_id) || shard.source_faces.empty() ||
            !std::is_sorted(shard.source_faces.begin(), shard.source_faces.end()) ||
            std::adjacent_find(shard.source_faces.begin(), shard.source_faces.end()) !=
                shard.source_faces.end() || !shard_ids.insert(shard.stable_id).second ||
            fracture_shard_id(outcome.representation_digest, outcome.effective_policy_digest,
                shard_index, shard) != shard.stable_id) {
            return core::Result<SurfaceFractureGeometry>::failure(
                invalid("fracture shard identity or face list is invalid"));
        }
        for (const geometry::FaceId face : shard.source_faces) {
            if (!face || !face_to_shard.emplace(face, shard_index).second) {
                return core::Result<SurfaceFractureGeometry>::failure(
                    invalid("fracture plan contains an invalid or multiply assigned face"));
            }
        }
    }

    std::set<geometry::FaceId> source_face_set(
        source.triangle_faces.begin(), source.triangle_faces.end());
    if (source_face_set.size() != face_to_shard.size() ||
        !std::equal(source_face_set.begin(), source_face_set.end(), face_to_shard.begin(),
            [](geometry::FaceId face, const auto& entry) { return face == entry.first; })) {
        return core::Result<SurfaceFractureGeometry>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "fracture plan does not cover exactly the source mesh faces"));
    }

    std::vector<SurfaceFractureGeometryShard> emitted;
    emitted.reserve(outcome.shards.size());
    for (std::size_t shard_index = 0U; shard_index < outcome.shards.size(); ++shard_index) {
        geometry::CompiledMesh shard_mesh;
        shard_mesh.source_revision = source.source_revision;
        std::map<std::uint32_t, std::uint32_t> remapped_vertices;
        for (std::size_t triangle = 0U; triangle < source.triangle_faces.size(); ++triangle) {
            const auto assignment = face_to_shard.find(source.triangle_faces[triangle]);
            if (assignment == face_to_shard.end() || assignment->second != shard_index) continue;
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const std::uint32_t source_index = source.indices[triangle * 3U + corner];
                auto vertex = remapped_vertices.find(source_index);
                if (vertex == remapped_vertices.end()) {
                    if (shard_mesh.positions.size() >=
                        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
                        return core::Result<SurfaceFractureGeometry>::failure(
                            invalid("fracture shard exceeds the compiled mesh index range"));
                    }
                    const auto output_index = static_cast<std::uint32_t>(shard_mesh.positions.size());
                    shard_mesh.positions.push_back(source.positions[source_index]);
                    shard_mesh.normals.push_back(source.normals[source_index]);
                    shard_mesh.vertex_ids.push_back(source.vertex_ids[source_index]);
                    shard_mesh.bounds.include(source.positions[source_index]);
                    vertex = remapped_vertices.emplace(source_index, output_index).first;
                }
                shard_mesh.indices.push_back(vertex->second);
            }
            shard_mesh.triangle_faces.push_back(source.triangle_faces[triangle]);
            shard_mesh.triangle_edges.push_back(source.triangle_edges[triangle]);
        }
        if (!shard_mesh.valid()) {
            return core::Result<SurfaceFractureGeometry>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "fracture materialization produced an invalid mesh shard"));
        }
        emitted.push_back({outcome.shards[shard_index].stable_id, std::move(shard_mesh)});
    }

    const std::string source_digest = digest_text(compiled_mesh_canonical(source));
    std::ostringstream geometry_canonical;
    geometry_canonical.imbue(std::locale::classic());
    geometry_canonical << "surface-fracture-geometry-v1\n"
                       << "representation=" << outcome.representation.value << '\n'
                       << "source_mesh_digest=" << source_digest << '\n'
                       << "policy_outcome_digest=" << outcome.digest << '\n';
    for (const auto& shard : emitted) {
        geometry_canonical << "shard=" << shard.stable_id << ':'
                           << digest_text(compiled_mesh_canonical(shard.mesh)) << '\n';
    }
    SurfaceFractureGeometry result{
        outcome.representation,
        source_digest,
        outcome.digest,
        digest_text(geometry_canonical.str()),
        std::move(emitted)};
    return core::Result<SurfaceFractureGeometry>::success(std::move(result));
}

core::Result<void> SurfaceFractureGeometry::validate(
    const geometry::CompiledMesh& source,
    const RepresentationFracturePolicyOutcome& outcome) const {
    const auto expected = materialize_fracture_geometry(source, outcome);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (representation != expected.value().representation ||
        source_mesh_digest != expected.value().source_mesh_digest ||
        policy_outcome_digest != expected.value().policy_outcome_digest ||
        digest != expected.value().digest || shards.size() != expected.value().shards.size()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::validation_failed,
                "fracture geometry receipt is stale or does not match the current inputs"));
    }
    for (std::size_t index = 0U; index < shards.size(); ++index) {
        if (shards[index].stable_id != expected.value().shards[index].stable_id ||
            compiled_mesh_canonical(shards[index].mesh) !=
                compiled_mesh_canonical(expected.value().shards[index].mesh)) {
            return core::Result<void>::failure(
                core::Diagnostic(core::ErrorCode::validation_failed,
                    "fracture geometry shard differs from its current source and plan"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::surface
