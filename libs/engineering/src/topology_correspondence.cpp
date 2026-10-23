#include <carto/engineering/topology_correspondence.hpp>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace carto::engineering {
namespace {

constexpr std::string_view kIdentityDomain = "carto.engineering.topology-id.v1";
constexpr std::size_t kMaxIdentityText = 4096U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_identity_text(std::string_view value, std::size_t maximum = kMaxIdentityText) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\r' && byte != '\n';
        });
}

bool valid_topology_kind(TopologyKind kind) {
    return static_cast<unsigned>(kind) <=
        static_cast<unsigned>(TopologyKind::topology_set);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

struct DisplayTriangleKey {
    std::uint64_t asset = 0U;
    core::Revision revision;
    assets::Sha256Digest digest;
    std::uint32_t triangle = 0U;
    [[nodiscard]] auto operator<=>(const DisplayTriangleKey&) const noexcept = default;
};

std::set<assets::Sha256Digest> identity_set(const TopologyCorrespondenceMap& map) {
    std::set<assets::Sha256Digest> result;
    for (const auto& binding : map.bindings) result.insert(binding.persistent_id);
    return result;
}

} // namespace

core::Result<assets::Sha256Digest> persistent_topology_id(
    const TopologyIdentityKey& key) {
    if (!safe_identity_text(key.source_namespace, 256U) ||
        !safe_identity_text(key.part_external_id) ||
        !safe_identity_text(key.topology_external_id) || !valid_topology_kind(key.kind)) {
        return core::Result<assets::Sha256Digest>::failure(invalid(
            "persistent topology identity requires bounded source, part, kind, and external IDs"));
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kIdentityDomain.size() + key.source_namespace.size() +
        key.part_external_id.size() + key.topology_external_id.size() + 16U);
    append_string(bytes, kIdentityDomain);
    append_string(bytes, key.source_namespace);
    append_string(bytes, key.part_external_id);
    bytes.push_back(static_cast<std::uint8_t>(key.kind));
    append_string(bytes, key.topology_external_id);
    return core::Result<assets::Sha256Digest>::success(assets::sha256(bytes));
}

core::Result<void> TopologyCorrespondenceMap::validate() const {
    if (!safe_identity_text(source_namespace, 256U) ||
        !safe_identity_text(source_revision, 256U) || source_digest.is_zero() ||
        bindings.size() > kMaxBindings) {
        return core::Result<void>::failure(validation(
            "topology correspondence source identity or binding count is invalid"));
    }
    std::map<assets::Sha256Digest, TopologyIdentityKey> identities;
    std::set<std::tuple<assets::Sha256Digest, std::uint64_t, std::uint64_t,
        std::string, std::uint32_t>> unique_bindings;
    for (const auto& binding : bindings) {
        if (binding.topology.source_namespace != source_namespace ||
            binding.display_mesh_asset == 0U || binding.display_mesh_revision.exhausted() ||
            binding.display_mesh_digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "topology display binding has an unresolved source or mesh identity"));
        }
        auto expected_id = persistent_topology_id(binding.topology);
        if (!expected_id || expected_id.value() != binding.persistent_id) {
            return core::Result<void>::failure(validation(
                "topology correspondence identity does not match its stable source key"));
        }
        const auto [identity, inserted] = identities.emplace(
            binding.persistent_id, binding.topology);
        if (!inserted && identity->second != binding.topology) {
            return core::Result<void>::failure(validation(
                "persistent topology identity collision was detected"));
        }
        const auto unique = std::tuple{binding.persistent_id,
            binding.display_mesh_asset, binding.display_mesh_revision.value(),
            binding.display_mesh_digest.hex(), binding.triangle};
        if (!unique_bindings.insert(unique).second) {
            return core::Result<void>::failure(validation(
                "topology correspondence contains a duplicate display binding"));
        }
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> TopologyCorrespondenceMap::resolve_display_triangle(
    std::string_view expected_source_revision,
    std::uint64_t display_mesh_asset,
    core::Revision display_mesh_revision,
    const assets::Sha256Digest& display_mesh_digest,
    std::uint32_t triangle) const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    if (expected_source_revision != source_revision || display_mesh_asset == 0U ||
        display_mesh_revision.exhausted() || display_mesh_digest.is_zero()) {
        return core::Result<assets::Sha256Digest>::failure(stale(
            "display selection does not match the active source or tessellation revision"));
    }
    std::set<assets::Sha256Digest> matches;
    for (const auto& binding : bindings) {
        if (binding.display_mesh_asset == display_mesh_asset &&
            binding.display_mesh_revision == display_mesh_revision &&
            binding.display_mesh_digest == display_mesh_digest &&
            binding.triangle == triangle) {
            matches.insert(binding.persistent_id);
        }
    }
    if (matches.empty()) {
        return core::Result<assets::Sha256Digest>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "display triangle has no source topology correspondence"));
    }
    if (matches.size() != 1U) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "display triangle maps to multiple source topology identities"));
    }
    return core::Result<assets::Sha256Digest>::success(*matches.begin());
}

core::Result<TopologyReimportReport> compare_topology_reimport(
    const TopologyCorrespondenceMap& previous,
    const TopologyCorrespondenceMap& incoming) {
    if (auto result = previous.validate(); !result) {
        return core::Result<TopologyReimportReport>::failure(result.error().with_context(
            "previous topology correspondence"));
    }
    if (auto result = incoming.validate(); !result) {
        return core::Result<TopologyReimportReport>::failure(result.error().with_context(
            "incoming topology correspondence"));
    }
    if (previous.source_namespace != incoming.source_namespace) {
        return core::Result<TopologyReimportReport>::failure(invalid(
            "topology reimport cannot compare different source namespaces"));
    }

    const auto before = identity_set(previous);
    const auto after = identity_set(incoming);
    TopologyReimportReport report;
    report.source_changed = previous.source_revision != incoming.source_revision ||
        previous.source_digest != incoming.source_digest;
    std::set_intersection(before.begin(), before.end(), after.begin(), after.end(),
        std::back_inserter(report.preserved_identities));
    std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
        std::back_inserter(report.added_identities));
    std::set_difference(before.begin(), before.end(), after.begin(), after.end(),
        std::back_inserter(report.removed_identities));

    std::map<assets::Sha256Digest, std::set<DisplayTriangleKey>> previous_display;
    std::map<assets::Sha256Digest, std::set<DisplayTriangleKey>> incoming_display;
    for (const auto& binding : previous.bindings) {
        previous_display[binding.persistent_id].insert({binding.display_mesh_asset,
            binding.display_mesh_revision, binding.display_mesh_digest, binding.triangle});
    }
    for (const auto& binding : incoming.bindings) {
        incoming_display[binding.persistent_id].insert({binding.display_mesh_asset,
            binding.display_mesh_revision, binding.display_mesh_digest, binding.triangle});
    }
    for (const auto& id : report.preserved_identities) {
        if (previous_display[id] != incoming_display[id]) {
            report.changed_correspondence_identities.push_back(id);
        }
    }

    std::map<DisplayTriangleKey, std::set<assets::Sha256Digest>> display_targets;
    for (const auto& binding : incoming.bindings) {
        display_targets[{binding.display_mesh_asset, binding.display_mesh_revision,
            binding.display_mesh_digest, binding.triangle}].insert(binding.persistent_id);
    }
    std::set<std::uint64_t> ambiguous_assets;
    for (const auto& [display_key, identities] : display_targets) {
        if (identities.size() > 1U) ambiguous_assets.insert(display_key.asset);
    }
    report.ambiguous_display_mesh_assets.assign(
        ambiguous_assets.begin(), ambiguous_assets.end());
    report.publication_blocked = report.source_changed ||
        !report.removed_identities.empty() ||
        !report.changed_correspondence_identities.empty() ||
        !report.ambiguous_display_mesh_assets.empty();
    return core::Result<TopologyReimportReport>::success(std::move(report));
}

} // namespace carto::engineering
