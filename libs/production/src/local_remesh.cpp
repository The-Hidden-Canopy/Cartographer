#include <carto/production/local_remesh.hpp>

#include <carto/attributes/transfer.hpp>
#include <carto/mesh_attributes/provenance.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::production {
namespace {

constexpr double kMaxLocalRemeshCoordinate = 1.0e9;
constexpr double kMaxLocalRemeshRadius = 1.0e6;
constexpr std::array<attributes::AttributeDomain, 4> kMeshDomains{
    attributes::AttributeDomain::vertex,
    attributes::AttributeDomain::edge,
    attributes::AttributeDomain::face,
    attributes::AttributeDomain::corner,
};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

std::string f64_hex(double value) {
    constexpr char digits[] = "0123456789abcdef";
    std::uint64_t bits = std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value);
    std::string encoded(16U, '0');
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        const std::size_t shift = (encoded.size() - index - 1U) * 4U;
        encoded[index] = digits[(bits >> shift) & 0x0fU];
    }
    return encoded;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string result_canonical(const LocalRemeshPatchReceipt& receipt) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTO_LOCAL_REMESH_PATCH_RECEIPT 1\n"
           << "MESH " << receipt.mesh_asset << '\n'
           << "ALGORITHM " << std::quoted(receipt.algorithm) << '\n'
           << "SOURCE_BEFORE " << std::quoted(receipt.source_before.identity) << ' '
           << receipt.source_before.revision.value() << ' '
           << receipt.source_before.content_digest.hex() << '\n'
           << "SOURCE_AFTER " << std::quoted(receipt.source_after.identity) << ' '
           << receipt.source_after.revision.value() << ' '
           << receipt.source_after.content_digest.hex() << '\n'
           << "SETTINGS " << receipt.settings.canonical().size() << ':'
           << receipt.settings.canonical() << '\n'
           << "SPLITS " << receipt.splits.size() << '\n';
    for (const auto& split : receipt.splits) {
        const std::string topology = split.topology.serialize();
        output << "SPLIT " << split.source_edge.value << ' '
               << f64_hex(split.source_edge_length) << ' '
               << topology.size() << ':' << topology << '\n';
    }
    return output.str();
}

struct EdgeCandidate {
    geometry::EdgeId id;
    double length = 0.0;
};

core::Result<std::optional<EdgeCandidate>> select_edge(
    const geometry::EditableMesh& mesh,
    const LocalRemeshSettings& settings) {
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<std::optional<EdgeCandidate>>::failure(topology.error());
    }
    std::optional<EdgeCandidate> selected;
    for (const geometry::EdgeRecord& edge : topology.value().edges) {
        if (settings.preserve_boundary && !edge.second_half_edge.has_value()) continue;
        const geometry::Vertex* first = mesh.find_vertex(edge.first);
        const geometry::Vertex* second = mesh.find_vertex(edge.second);
        if (first == nullptr || second == nullptr) {
            return core::Result<std::optional<EdgeCandidate>>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "local remesh edge references a missing source vertex"));
        }
        const core::Vec3d delta = second->position - first->position;
        if (!delta.finite()) {
            return core::Result<std::optional<EdgeCandidate>>::failure(validation(
                "local remesh edge length cannot be represented as a finite value"));
        }
        const double length = std::hypot(delta.x, delta.y, delta.z);
        if (!std::isfinite(length)) {
            return core::Result<std::optional<EdgeCandidate>>::failure(validation(
                "local remesh edge length is non-finite"));
        }
        if (length <= settings.maximum_edge_length) continue;
        const core::Vec3d midpoint{
            first->position.x * 0.5 + second->position.x * 0.5,
            first->position.y * 0.5 + second->position.y * 0.5,
            first->position.z * 0.5 + second->position.z * 0.5};
        const core::Vec3d from_center = midpoint - settings.center;
        if (!midpoint.finite() || !from_center.finite()) continue;
        const double distance = std::hypot(from_center.x, from_center.y, from_center.z);
        if (!std::isfinite(distance) || distance > settings.region_radius) continue;
        if (!selected.has_value() || length > selected->length ||
            (length == selected->length && edge.id < selected->id)) {
            selected = EdgeCandidate{edge.id, length};
        }
    }
    return core::Result<std::optional<EdgeCandidate>>::success(selected);
}

bool interpolable(attributes::AttributeType type) {
    switch (type) {
    case attributes::AttributeType::float32:
    case attributes::AttributeType::float64:
    case attributes::AttributeType::vec2:
    case attributes::AttributeType::vec3:
    case attributes::AttributeType::vec4:
    case attributes::AttributeType::color4:
        return true;
    case attributes::AttributeType::boolean:
    case attributes::AttributeType::int32:
    case attributes::AttributeType::uint32:
    case attributes::AttributeType::string_id:
    case attributes::AttributeType::object_id:
    case attributes::AttributeType::vertex_id:
        return false;
    }
    return false;
}

struct AppliedSplit {
    geometry::EditableMesh mesh;
    geometry::TopologyEditReceipt receipt;
};

core::Result<AppliedSplit> split_edge_with_attributes(
    const geometry::EditableMesh& source,
    geometry::EdgeId edge) {
    if (auto result = source.validate(); !result) {
        return core::Result<AppliedSplit>::failure(result.error());
    }
    geometry::EditableMesh output = source;
    geometry::TopologyEditReceipt receipt;
    const bool has_attributes = source.has_attribute_payload();
    if (!has_attributes) {
        const auto split = output.split_edge(edge, 0.5);
        if (!split) return core::Result<AppliedSplit>::failure(split.error());
        receipt = split.value();
    } else {
        auto source_topology = mesh_attributes::stable_topology_snapshot(source);
        if (!source_topology) return core::Result<AppliedSplit>::failure(source_topology.error());

        attributes::AttributeSet transfer_source = source.attributes();
        for (const auto& [domain, ignored] : transfer_source.domain_counts()) {
            static_cast<void>(ignored);
            if (std::find(kMeshDomains.begin(), kMeshDomains.end(), domain) == kMeshDomains.end()) {
                return core::Result<AppliedSplit>::failure(unsupported(
                    "local remesh cannot transfer non-mesh attribute domains"));
            }
        }
        for (const attributes::AttributeDomain domain : kMeshDomains) {
            const auto ids = source_topology.value().element_ids.find(domain);
            if (ids == source_topology.value().element_ids.end()) {
                return core::Result<AppliedSplit>::failure(validation(
                    "local remesh source topology is missing a mesh attribute domain"));
            }
            const auto count = transfer_source.domain_count(domain);
            if (count.has_value() && *count != ids->second.size()) {
                return core::Result<AppliedSplit>::failure(validation(
                    "local remesh source attribute domain count disagrees with mesh topology"));
            }
            if (!count.has_value()) {
                if (auto result = transfer_source.set_domain_count(domain, ids->second.size());
                    !result) {
                    return core::Result<AppliedSplit>::failure(result.error());
                }
            }
        }
        for (const auto& [layer_id, layer] : transfer_source.layers()) {
            if ((layer.descriptor.domain == attributes::AttributeDomain::vertex ||
                    layer.descriptor.domain == attributes::AttributeDomain::corner) &&
                !interpolable(layer.descriptor.type)) {
                return core::Result<AppliedSplit>::failure(unsupported(
                    "local remesh requires an explicit transfer policy for discrete vertex/corner layer " +
                    layer_id));
            }
            if (std::find(kMeshDomains.begin(), kMeshDomains.end(), layer.descriptor.domain) ==
                kMeshDomains.end()) {
                return core::Result<AppliedSplit>::failure(unsupported(
                    "local remesh cannot transfer non-mesh attribute layer " + layer_id));
            }
        }

        const attributes::UvSetTable source_uv_sets = source.uv_sets();
        if (auto result = output.clear_attribute_payload(); !result) {
            return core::Result<AppliedSplit>::failure(result.error());
        }
        const geometry::EditableMesh topology_source = output;
        const auto split = output.split_edge(edge, 0.5);
        if (!split) return core::Result<AppliedSplit>::failure(split.error());
        const auto provenance = mesh_attributes::provenance_from_receipt(
            topology_source, output, split.value());
        if (!provenance) return core::Result<AppliedSplit>::failure(provenance.error());

        attributes::AttributeTransferPolicy policy;
        policy.default_mode = attributes::TransferMode::invalidate;
        for (const auto& [layer_id, layer] : transfer_source.layers()) {
            attributes::TransferMode mode = attributes::TransferMode::invalidate;
            switch (layer.descriptor.domain) {
            case attributes::AttributeDomain::vertex:
            case attributes::AttributeDomain::corner:
                mode = attributes::TransferMode::interpolate;
                break;
            case attributes::AttributeDomain::edge:
                mode = attributes::TransferMode::duplicate;
                break;
            case attributes::AttributeDomain::face:
                mode = attributes::TransferMode::preserve;
                break;
            default:
                return core::Result<AppliedSplit>::failure(unsupported(
                    "local remesh cannot transfer attribute layer " + layer_id));
            }
            policy.per_layer.emplace(layer_id, mode);
        }
        const auto transferred = attributes::transfer(
            transfer_source, provenance.value(), policy);
        if (!transferred) return core::Result<AppliedSplit>::failure(transferred.error());
        if (auto result = output.set_attribute_payload(transferred.value(), source_uv_sets);
            !result) {
            return core::Result<AppliedSplit>::failure(result.error());
        }
        receipt = split.value();
    }

    receipt.revision_before = source.revision();
    receipt.revision_after = output.revision();
    if (auto result = receipt.validate(); !result) {
        return core::Result<AppliedSplit>::failure(result.error());
    }
    if (auto result = output.validate(); !result) {
        return core::Result<AppliedSplit>::failure(result.error());
    }
    return core::Result<AppliedSplit>::success(AppliedSplit{
        std::move(output), std::move(receipt)});
}

core::Result<LocalRemeshPatchReceipt> fail_and_rollback(
    project::ProjectTransaction& transaction,
    core::Diagnostic diagnostic) {
    const auto rollback = transaction.rollback();
    if (!rollback) {
        return core::Result<LocalRemeshPatchReceipt>::failure(
            rollback.error().with_context(diagnostic.message));
    }
    return core::Result<LocalRemeshPatchReceipt>::failure(std::move(diagnostic));
}

} // namespace

core::Result<void> LocalRemeshSettings::validate() const {
    if (!center.finite() || std::abs(center.x) > kMaxLocalRemeshCoordinate ||
        std::abs(center.y) > kMaxLocalRemeshCoordinate ||
        std::abs(center.z) > kMaxLocalRemeshCoordinate ||
        !std::isfinite(region_radius) || region_radius <= 0.0 ||
        region_radius > kMaxLocalRemeshRadius ||
        !std::isfinite(maximum_edge_length) || maximum_edge_length <= 0.0 ||
        maximum_edge_length > kMaxLocalRemeshRadius || max_splits == 0U ||
        max_splits > kMaxLocalRemeshSplits) {
        return core::Result<void>::failure(invalid(
            "local remesh settings are invalid or exceed bounded limits"));
    }
    return core::Result<void>::success();
}

std::string LocalRemeshSettings::canonical() const {
    if (!validate()) return {};
    std::ostringstream output;
    output << "CARTO_LOCAL_REMESH_SETTINGS " << kLocalRemeshPatchVersion << '\n'
           << "CENTER " << f64_hex(center.x) << ' ' << f64_hex(center.y) << ' '
           << f64_hex(center.z) << '\n'
           << "REGION_RADIUS " << f64_hex(region_radius) << '\n'
           << "MAX_EDGE_LENGTH " << f64_hex(maximum_edge_length) << '\n'
           << "MAX_SPLITS " << max_splits << '\n'
           << "PRESERVE_BOUNDARY " << (preserve_boundary ? 1 : 0) << '\n';
    return output.str();
}

core::Result<assets::Sha256Digest> LocalRemeshSettings::digest() const {
    const std::string encoded = canonical();
    if (encoded.empty()) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "cannot digest invalid local remesh settings"));
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(encoded));
}

core::Result<void> LocalRemeshPatchReceipt::refresh_digest() {
    if (auto result = settings.validate(); !result) return result;
    const auto calculated_settings_digest = settings.digest();
    if (!calculated_settings_digest) return core::Result<void>::failure(
        calculated_settings_digest.error());
    settings_digest = calculated_settings_digest.value();
    output_digest = source_after.content_digest;
    result_digest = digest_text(result_canonical(*this));
    return core::Result<void>::success();
}

core::Result<void> LocalRemeshPatchReceipt::validate() const {
    if (mesh_asset == 0U || algorithm != "carto.local-remesh.edge-split.v1" ||
        splits.empty() || splits.size() > settings.max_splits) {
        return core::Result<void>::failure(validation(
            "local remesh patch receipt identity or split count is invalid"));
    }
    if (auto result = source_before.validate(); !result) return result;
    if (auto result = source_after.validate(); !result) return result;
    if (source_before.identity != source_after.identity ||
        source_before.revision.exhausted() || source_after.revision <= source_before.revision ||
        output_digest != source_after.content_digest) {
        return core::Result<void>::failure(stale(
            "local remesh patch source/output bindings do not form one revision interval"));
    }
    if (auto result = settings.validate(); !result) return result;
    const auto calculated_settings_digest = settings.digest();
    if (!calculated_settings_digest || calculated_settings_digest.value() != settings_digest) {
        return core::Result<void>::failure(validation(
            "local remesh settings digest does not match its recipe"));
    }
    core::Revision expected_revision = source_before.revision;
    for (const auto& split : splits) {
        if (!split.source_edge || !std::isfinite(split.source_edge_length) ||
            split.source_edge_length <= settings.maximum_edge_length ||
            split.topology.revision_before != expected_revision ||
            split.topology.removed_edges.size() != 1U ||
            split.topology.removed_edges.front() != split.source_edge) {
            return core::Result<void>::failure(validation(
                "local remesh split record does not bind its source edge and revision"));
        }
        if (auto result = split.topology.validate(); !result) return result;
        expected_revision = split.topology.revision_after;
    }
    if (expected_revision != source_after.revision) {
        return core::Result<void>::failure(stale(
            "local remesh split chain does not reach its output revision"));
    }
    if (result_digest.is_zero() || digest_text(result_canonical(*this)) != result_digest) {
        return core::Result<void>::failure(validation(
            "local remesh patch receipt digest does not bind its settings, source, and split chain"));
    }
    return core::Result<void>::success();
}

std::string LocalRemeshPatchReceipt::transaction_operation() const {
    if (!validate()) return {};
    return "local-remesh.v1." + std::to_string(mesh_asset) + "." + result_digest.hex();
}

core::Result<LocalRemeshPatchReceipt> stage_local_remesh_patch(
    project::ProjectTransaction& transaction,
    std::uint64_t mesh_asset,
    const LocalRemeshSettings& settings) {
    if (!transaction.active()) {
        return core::Result<LocalRemeshPatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "local remesh requires an active project transaction"));
    }
    if (transaction.staged_revision() != transaction.revision_before()) {
        return core::Result<LocalRemeshPatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "local remesh patch requires an empty project transaction"));
    }
    if (auto result = settings.validate(); !result) {
        return core::Result<LocalRemeshPatchReceipt>::failure(result.error());
    }
    const auto& initial_document = transaction.staged_document();
    if (initial_document.sculpt_stacks().contains(mesh_asset)) {
        return core::Result<LocalRemeshPatchReceipt>::failure(unsupported(
            "local remesh cannot invalidate an attached SculptStack without a rebase receipt"));
    }
    const auto initial_mesh = initial_document.meshes().find(mesh_asset);
    if (mesh_asset == 0U || initial_mesh == initial_document.meshes().end()) {
        return core::Result<LocalRemeshPatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "local remesh target mesh asset does not exist"));
    }
    const std::string identity = initial_document.source_namespace() + "/mesh/" +
        std::to_string(mesh_asset);
    const auto source_before = bind_bake_source(identity, initial_mesh->second);
    if (!source_before) return core::Result<LocalRemeshPatchReceipt>::failure(
        source_before.error());

    LocalRemeshPatchReceipt receipt;
    receipt.mesh_asset = mesh_asset;
    receipt.source_before = source_before.value();
    receipt.settings = settings;
    const auto settings_digest = settings.digest();
    if (!settings_digest) return core::Result<LocalRemeshPatchReceipt>::failure(
        settings_digest.error());
    receipt.settings_digest = settings_digest.value();

    for (std::uint32_t split_index = 0U; split_index < settings.max_splits; ++split_index) {
        const auto current = transaction.staged_document().meshes().find(mesh_asset);
        if (current == transaction.staged_document().meshes().end()) {
            return fail_and_rollback(transaction, core::Diagnostic(
                core::ErrorCode::stale_data,
                "local remesh target disappeared while staging the patch"));
        }
        const auto candidate = select_edge(current->second, settings);
        if (!candidate) return fail_and_rollback(transaction, candidate.error());
        if (!candidate.value().has_value()) {
            if (receipt.splits.empty()) {
                return core::Result<LocalRemeshPatchReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "local remesh region contains no eligible overlong edge"));
            }
            break;
        }
        const auto applied = split_edge_with_attributes(current->second, candidate.value()->id);
        if (!applied) return fail_and_rollback(transaction, applied.error());
        const auto after = bind_bake_source(identity, applied.value().mesh);
        if (!after) return fail_and_rollback(transaction, after.error());
        geometry::TopologyEditReceipt topology_receipt = applied.value().receipt;
        const auto replace = transaction.replace_mesh(
            mesh_asset, applied.value().mesh, topology_receipt);
        if (!replace) return fail_and_rollback(transaction, replace.error());
        receipt.splits.push_back(LocalRemeshSplitRecord{
            candidate.value()->id, candidate.value()->length, std::move(topology_receipt)});
        if (split_index + 1U == settings.max_splits) {
            const auto remaining = select_edge(
                transaction.staged_document().meshes().at(mesh_asset), settings);
            if (!remaining) return fail_and_rollback(transaction, remaining.error());
            if (remaining.value().has_value()) {
                return fail_and_rollback(transaction, unsupported(
                    "local remesh split budget cannot complete the requested patch"));
            }
        }
    }

    const auto final_mesh = transaction.staged_document().meshes().find(mesh_asset);
    if (final_mesh == transaction.staged_document().meshes().end()) {
        return fail_and_rollback(transaction, core::Diagnostic(
            core::ErrorCode::stale_data,
            "local remesh target disappeared before receipt finalization"));
    }
    const auto source_after = bind_bake_source(identity, final_mesh->second);
    if (!source_after) return fail_and_rollback(transaction, source_after.error());
    receipt.source_after = source_after.value();
    if (auto result = receipt.refresh_digest(); !result) {
        return fail_and_rollback(transaction, result.error());
    }
    if (auto result = receipt.validate(); !result) {
        return fail_and_rollback(transaction, result.error());
    }
    return core::Result<LocalRemeshPatchReceipt>::success(std::move(receipt));
}

} // namespace carto::production
