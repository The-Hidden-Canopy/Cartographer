#include <carto/production/reimport.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace carto::production {

namespace {

constexpr std::string_view kSnapshotMagic = "CARTOGRAPHER_RUNTIME_SNAPSHOT";
constexpr std::string_view kDeltaMagic = "CARTOGRAPHER_PREPARED_DELTA";
constexpr std::string_view kPlanMagic = "CARTOGRAPHER_REIMPORT_PLAN";
constexpr std::string_view kReceiptMagic = "CARTOGRAPHER_REIMPORT_RECEIPT";
constexpr std::size_t kMaxIdentityBytes = 192U;
constexpr std::size_t kMaxDetailBytes = 2'048U;
constexpr std::size_t kMaxOperatorBytes = 192U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-' || byte == ':' || byte == '/';
        });
}

bool valid_source_kind(PreparedSourceEntityKind kind) {
    return static_cast<unsigned>(kind) <=
        static_cast<unsigned>(PreparedSourceEntityKind::material_region);
}

bool valid_prepared_kind(PreparedEntityKind kind) {
    return static_cast<unsigned>(kind) <=
        static_cast<unsigned>(PreparedEntityKind::material_binding);
}

bool valid_condition(RuntimeConditionKind condition) {
    return static_cast<unsigned>(condition) <=
        static_cast<unsigned>(RuntimeConditionKind::damaged_and_looted);
}

bool valid_conflict_kind(ReimportConflictKind kind) {
    switch (kind) {
    case ReimportConflictKind::destroyed:
    case ReimportConflictKind::moved:
    case ReimportConflictKind::damaged:
    case ReimportConflictKind::looted:
    case ReimportConflictKind::hierarchy_reparent:
        return true;
    }
    return false;
}

bool valid_decision(ReimportOperatorDecision decision) {
    switch (decision) {
    case ReimportOperatorDecision::admit:
    case ReimportOperatorDecision::override_decision:
    case ReimportOperatorDecision::skip:
        return true;
    }
    return false;
}

// The merge key: stable source identity. Strong ordering keeps every derived
// list canonically sorted.
struct SourceKey {
    PreparedSourceEntityKind kind = PreparedSourceEntityKind::scene_node;
    std::uint64_t primary = 0U;
    std::uint64_t secondary = 0U;

    [[nodiscard]] auto operator<=>(const SourceKey&) const noexcept = default;
};

SourceKey key_of(const PreparedSceneNode& node) {
    return {PreparedSourceEntityKind::scene_node, node.id, 0U};
}

SourceKey key_of(const PreparedAssetDefinition& asset) {
    return {PreparedSourceEntityKind::mesh_asset, asset.id, 0U};
}

SourceKey key_of(const PreparedMaterialBinding& binding) {
    return {PreparedSourceEntityKind::material_region, binding.asset, binding.region};
}

SourceKey key_of(const PreparedSourceCorrespondence& correspondence) {
    return {correspondence.source_kind, correspondence.source_primary,
            correspondence.source_secondary};
}

SourceKey key_of(const RuntimeSourceState& state) {
    return {state.source_kind, state.source_primary, state.source_secondary};
}

SourceKey key_of(const PreparedEntityRef& ref) {
    return {ref.source_kind, ref.source_primary, ref.source_secondary};
}

SourceKey key_of(const ReimportConflict& conflict) {
    return key_of(conflict.entity);
}

// Records of one envelope reachable by stable source identity.
struct EntityRecords {
    const PreparedSceneNode* node = nullptr;
    const PreparedAssetDefinition* asset = nullptr;
    const PreparedMaterialBinding* binding = nullptr;
    const PreparedSourceCorrespondence* correspondence = nullptr;
};

using EntityIndex = std::map<SourceKey, EntityRecords>;

EntityIndex index_envelope(const PreparedSceneEnvelope& envelope) {
    EntityIndex index;
    for (const auto& correspondence : envelope.correspondences) {
        index[key_of(correspondence)].correspondence = &correspondence;
    }
    for (const auto& node : envelope.nodes) {
        index[key_of(node)].node = &node;
    }
    for (const auto& asset : envelope.assets) {
        index[key_of(asset)].asset = &asset;
    }
    for (const auto& binding : envelope.material_bindings) {
        index[key_of(binding)].binding = &binding;
    }
    return index;
}

PreparedEntityRef ref_from(const PreparedSourceCorrespondence& correspondence) {
    PreparedEntityRef ref;
    ref.source_kind = correspondence.source_kind;
    ref.source_primary = correspondence.source_primary;
    ref.source_secondary = correspondence.source_secondary;
    ref.prepared_kind = correspondence.prepared_kind;
    ref.prepared_primary = correspondence.prepared_primary;
    ref.prepared_secondary = correspondence.prepared_secondary;
    return ref;
}

std::string node_record_canonical(const PreparedSceneNode& node) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "node-v1{id=" << node.id << ";parent=" << node.parent.value_or(0U)
           << ";asset=" << node.asset.value_or(0U) << ";visible=" << (node.visible ? 1U : 0U)
           << ";name=" << std::quoted(node.name)
           << ";t=" << node.local_transform.translation.x << ','
           << node.local_transform.translation.y << ','
           << node.local_transform.translation.z
           << ";r=" << node.local_transform.rotation.x << ','
           << node.local_transform.rotation.y << ','
           << node.local_transform.rotation.z << ','
           << node.local_transform.rotation.w
           << ";s=" << node.local_transform.scale.x << ','
           << node.local_transform.scale.y << ',' << node.local_transform.scale.z << '}';
    return output.str();
}

std::string asset_record_canonical(const PreparedAssetDefinition& asset) {
    std::ostringstream output;
    output << "asset-v1{id=" << asset.id << ";name=" << std::quoted(asset.name)
           << ";render=" << asset.render_mesh_product
           << ";lookdev=" << asset.lookdev_product.value_or("")
           << ";channels=" << asset.channel_mask << ";regions=";
    for (std::size_t index = 0U; index < asset.material_regions.size(); ++index) {
        if (index > 0U) output << ',';
        output << asset.material_regions[index];
    }
    output << '}';
    return output.str();
}

std::string binding_record_canonical(const PreparedMaterialBinding& binding) {
    std::ostringstream output;
    output << "binding-v1{asset=" << binding.asset << ";region=" << binding.region
           << ";material=" << binding.material
           << ";variant=" << binding.variant.value_or(0U) << '}';
    return output.str();
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

// Per-leg record digest for conflict binding; zero when the leg has no record.
assets::Sha256Digest record_digest(const EntityRecords& records) {
    if (records.node != nullptr) return digest_text(node_record_canonical(*records.node));
    if (records.asset != nullptr) return digest_text(asset_record_canonical(*records.asset));
    if (records.binding != nullptr) {
        return digest_text(binding_record_canonical(*records.binding));
    }
    return {};
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_signed(std::istream& input, std::int64_t& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

template <typename Enum>
bool read_enum(std::istream& input, unsigned maximum, Enum& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum) return false;
    value = static_cast<Enum>(encoded);
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

void write_digest(std::ostringstream& output, const assets::Sha256Digest& digest) {
    if (digest.is_zero()) {
        output << "-";
    } else {
        output << digest.hex();
    }
}

// Zero digests encode as "-"; anything else must parse to a non-zero digest.
bool read_digest_token(std::istream& input, assets::Sha256Digest& digest) {
    std::string encoded;
    if (!(input >> encoded)) return false;
    if (encoded == "-") {
        digest = {};
        return true;
    }
    const auto parsed = assets::Sha256Digest::from_hex(encoded);
    if (!parsed || parsed.value().is_zero()) return false;
    digest = parsed.value();
    return true;
}

void write_footprint(std::ostringstream& output, const SpatialTileRange& footprint) {
    output << " \"" << footprint.grid_identity << "\" " << footprint.level << ' '
           << footprint.minimum_x << ' ' << footprint.minimum_y << ' '
           << footprint.minimum_z << ' ' << footprint.maximum_x << ' '
           << footprint.maximum_y << ' ' << footprint.maximum_z;
}

bool read_footprint(std::istream& input, SpatialTileRange& footprint) {
    return static_cast<bool>(input >> std::quoted(footprint.grid_identity)) &&
        read_unsigned(input, footprint.level) &&
        read_signed(input, footprint.minimum_x) &&
        read_signed(input, footprint.minimum_y) &&
        read_signed(input, footprint.minimum_z) &&
        read_signed(input, footprint.maximum_x) &&
        read_signed(input, footprint.maximum_y) &&
        read_signed(input, footprint.maximum_z);
}

void write_entity_ref(std::ostringstream& output, const PreparedEntityRef& ref) {
    output << static_cast<unsigned>(ref.source_kind) << ' ' << ref.source_primary << ' '
           << ref.source_secondary << ' ' << static_cast<unsigned>(ref.prepared_kind)
           << ' ' << ref.prepared_primary << ' ' << ref.prepared_secondary;
}

bool read_entity_ref(std::istream& input, PreparedEntityRef& ref) {
    return read_enum(input,
                static_cast<unsigned>(PreparedSourceEntityKind::material_region),
                ref.source_kind) &&
        read_unsigned(input, ref.source_primary) &&
        read_unsigned(input, ref.source_secondary) &&
        read_enum(input, static_cast<unsigned>(PreparedEntityKind::material_binding),
            ref.prepared_kind) &&
        read_unsigned(input, ref.prepared_primary) &&
        read_unsigned(input, ref.prepared_secondary);
}

template <typename Entry, typename Writer>
void write_entries(std::ostringstream& output, std::string_view count_label,
    std::string_view row_label, const std::vector<Entry>& entries, Writer writer) {
    output << count_label << ' ' << entries.size() << '\n';
    for (const auto& entry : entries) {
        output << row_label << ' ';
        writer(entry);
        output << '\n';
    }
}

template <typename Entry, typename Reader>
bool read_entries(std::istream& input, std::string_view count_label,
    std::string_view row_label, std::size_t maximum, std::vector<Entry>& entries,
    Reader reader) {
    std::size_t count = 0U;
    if (!require_token(input, count_label) || !read_count(input, maximum, count)) {
        return false;
    }
    entries.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        Entry entry;
        if (!require_token(input, row_label) || !reader(entry)) return false;
        entries.push_back(std::move(entry));
    }
    return true;
}

bool strictly_sorted_unique(const std::vector<ReimportPlanEntry>& entries) {
    for (std::size_t index = 1U; index < entries.size(); ++index) {
        if (!(key_of(entries[index - 1U].entity) < key_of(entries[index].entity))) {
            return false;
        }
    }
    return true;
}

bool strictly_sorted_unique(const std::vector<ReimportConflict>& conflicts) {
    for (std::size_t index = 1U; index < conflicts.size(); ++index) {
        if (!(key_of(conflicts[index - 1U]) < key_of(conflicts[index]))) return false;
    }
    return true;
}

bool strictly_sorted_unique(const std::vector<ReimportDecisionRecord>& decisions) {
    for (std::size_t index = 1U; index < decisions.size(); ++index) {
        const SourceKey previous{decisions[index - 1U].source_kind,
            decisions[index - 1U].source_primary, decisions[index - 1U].source_secondary};
        const SourceKey current{decisions[index].source_kind,
            decisions[index].source_primary, decisions[index].source_secondary};
        if (!(previous < current)) return false;
    }
    return true;
}

bool strictly_sorted_unique(const std::vector<RuntimeSourceState>& states) {
    for (std::size_t index = 1U; index < states.size(); ++index) {
        if (!(key_of(states[index - 1U]) < key_of(states[index]))) return false;
    }
    return true;
}

bool strictly_sorted_unique(const std::vector<PreparedEntityChange>& changes) {
    for (std::size_t index = 1U; index < changes.size(); ++index) {
        if (!(key_of(changes[index - 1U].entity) < key_of(changes[index].entity))) {
            return false;
        }
    }
    return true;
}

bool strictly_sorted_unique(const std::vector<PropertyOwnershipRecord>& records) {
    for (std::size_t index = 1U; index < records.size(); ++index) {
        const SourceKey previous{records[index - 1U].source_kind,
            records[index - 1U].source_primary, records[index - 1U].source_secondary};
        const SourceKey current{records[index].source_kind,
            records[index].source_primary, records[index].source_secondary};
        if (!(previous < current)) return false;
    }
    return true;
}

// Runtime drift relative to the admitted baseline. Only placement-relevant
// entities (scene nodes) consult placement; condition drift applies to any
// entity with game-owned state.
enum class RuntimeDrift {
    none,
    destroyed,
    moved,
    reparented,
    damaged,
    looted,
};

RuntimeDrift drift_of(const RuntimeSourceState* state, const EntityRecords& base) {
    if (state == nullptr) return RuntimeDrift::none;
    if (!state->present) return RuntimeDrift::destroyed;
    if (base.node != nullptr &&
        state->placement_digest != state->admitted_placement_digest) {
        // Parent comparison happens in source identity space; the prepared
        // node id of a scene node is its stable source identity.
        std::optional<std::uint64_t> base_parent;
        if (base.node->parent.has_value()) base_parent = base.node->parent.value();
        if (state->parent_source != base_parent) return RuntimeDrift::reparented;
        return RuntimeDrift::moved;
    }
    switch (state->condition) {
    case RuntimeConditionKind::intact:
        return RuntimeDrift::none;
    case RuntimeConditionKind::damaged:
    case RuntimeConditionKind::damaged_and_looted:
        return RuntimeDrift::damaged;
    case RuntimeConditionKind::looted:
        return RuntimeDrift::looted;
    }
    return RuntimeDrift::none;
}

ReimportConflictKind conflict_kind_of(RuntimeDrift drift) {
    switch (drift) {
    case RuntimeDrift::destroyed:
        return ReimportConflictKind::destroyed;
    case RuntimeDrift::moved:
        return ReimportConflictKind::moved;
    case RuntimeDrift::reparented:
        return ReimportConflictKind::hierarchy_reparent;
    case RuntimeDrift::damaged:
        return ReimportConflictKind::damaged;
    case RuntimeDrift::looted:
        return ReimportConflictKind::looted;
    case RuntimeDrift::none:
        break;
    }
    return ReimportConflictKind::destroyed;
}

const char* drift_detail(RuntimeDrift drift) {
    switch (drift) {
    case RuntimeDrift::destroyed:
        return "runtime destruction must never be resurrected by an authored change";
    case RuntimeDrift::moved:
        return "runtime placement drift collides with an authored change";
    case RuntimeDrift::reparented:
        return "runtime hierarchy reparenting collides with an authored change";
    case RuntimeDrift::damaged:
        return "runtime damage state collides with an authored change";
    case RuntimeDrift::looted:
        return "runtime inventory drift collides with an authored change";
    case RuntimeDrift::none:
        break;
    }
    return "runtime state collides with an authored change";
}

std::uint32_t diff_aspects(
    const EntityRecords& base, const EntityRecords& incoming) {
    std::uint32_t aspects = 0U;
    if (base.correspondence != nullptr && incoming.correspondence != nullptr &&
        (base.correspondence->prepared_kind != incoming.correspondence->prepared_kind ||
            base.correspondence->prepared_primary != incoming.correspondence->prepared_primary ||
            base.correspondence->prepared_secondary !=
                incoming.correspondence->prepared_secondary)) {
        aspects |= changed_correspondence_target;
    }
    if (base.node != nullptr && incoming.node != nullptr) {
        const auto& left = *base.node;
        const auto& right = *incoming.node;
        if (!(left.local_transform.translation.x == right.local_transform.translation.x &&
                left.local_transform.translation.y == right.local_transform.translation.y &&
                left.local_transform.translation.z == right.local_transform.translation.z &&
                left.local_transform.rotation.x == right.local_transform.rotation.x &&
                left.local_transform.rotation.y == right.local_transform.rotation.y &&
                left.local_transform.rotation.z == right.local_transform.rotation.z &&
                left.local_transform.rotation.w == right.local_transform.rotation.w &&
                left.local_transform.scale.x == right.local_transform.scale.x &&
                left.local_transform.scale.y == right.local_transform.scale.y &&
                left.local_transform.scale.z == right.local_transform.scale.z)) {
            aspects |= changed_node_transform;
        }
        if (left.parent != right.parent) aspects |= changed_node_hierarchy;
        if (left.visible != right.visible) aspects |= changed_node_visibility;
        if (left.name != right.name) aspects |= changed_node_name;
        if (left.asset != right.asset) aspects |= changed_node_asset_ref;
    }
    if (base.asset != nullptr && incoming.asset != nullptr) {
        const auto& left = *base.asset;
        const auto& right = *incoming.asset;
        if (left.name != right.name || left.render_mesh_product != right.render_mesh_product ||
            left.lookdev_product != right.lookdev_product) {
            aspects |= changed_asset_products;
        }
        if (left.channel_mask != right.channel_mask) aspects |= changed_asset_channels;
        if (left.material_regions != right.material_regions) aspects |= changed_asset_regions;
    }
    if (base.binding != nullptr && incoming.binding != nullptr) {
        const auto& left = *base.binding;
        const auto& right = *incoming.binding;
        if (left.material != right.material) aspects |= changed_binding_material;
        if (left.variant != right.variant) aspects |= changed_binding_variant;
    }
    return aspects;
}

core::Result<void> check_live_against_base(
    const RuntimeWorldSnapshot& live,
    const EntityIndex& base_index,
    const EntityIndex& incoming_index) {
    for (const auto& state : live.states) {
        const SourceKey key = key_of(state);
        const auto base = base_index.find(key);
        if (state.present) {
            if (base == base_index.end()) {
                return core::Result<void>::failure(stale(
                    "live world reports a source identity the admitted base does not contain"));
            }
        } else if (base == base_index.end() && !incoming_index.contains(key)) {
            return core::Result<void>::failure(stale(
                "live tombstone references an identity no prepared revision contains"));
        }
    }
    return core::Result<void>::success();
}

struct MergeInput {
    const PreparedSceneEnvelope* base = nullptr;
    const PreparedSceneEnvelope* incoming = nullptr;
    const RuntimeWorldSnapshot* live = nullptr;
    EntityIndex base_index;
    EntityIndex incoming_index;
    PreparedRevisionDelta delta;
    std::map<SourceKey, const RuntimeSourceState*> live_states;
};

core::Result<MergeInput> make_merge_input(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming,
    const RuntimeWorldSnapshot& live) {
    if (auto result = base.validate(); !result) return core::Result<MergeInput>::failure(result.error());
    if (auto result = incoming.validate(); !result) {
        return core::Result<MergeInput>::failure(result.error());
    }
    if (auto result = live.validate(); !result) {
        return core::Result<MergeInput>::failure(result.error());
    }
    if (base.source_namespace != incoming.source_namespace ||
        base.source_namespace != live.source_namespace) {
        return core::Result<MergeInput>::failure(validation(
            "three-way reconciliation requires one source namespace across all legs"));
    }
    const auto delta = diff_prepared_revisions(base, incoming);
    if (!delta) return core::Result<MergeInput>::failure(delta.error());

    MergeInput input;
    input.base = &base;
    input.incoming = &incoming;
    input.live = &live;
    input.base_index = index_envelope(base);
    input.incoming_index = index_envelope(incoming);
    input.delta = delta.value();
    for (const auto& state : live.states) {
        input.live_states.emplace(key_of(state), &state);
    }
    if (auto result = check_live_against_base(live, input.base_index, input.incoming_index);
        !result) {
        return core::Result<MergeInput>::failure(result.error());
    }
    return core::Result<MergeInput>::success(std::move(input));
}

std::set<SourceKey> merge_key_union(const MergeInput& input) {
    std::set<SourceKey> keys;
    for (const auto& change : input.delta.added) keys.insert(key_of(change.entity));
    for (const auto& change : input.delta.removed) keys.insert(key_of(change.entity));
    for (const auto& change : input.delta.changed) keys.insert(key_of(change.entity));
    for (const auto& [key, state] : input.live_states) {
        (void)state;
        keys.insert(key);
    }
    return keys;
}

// Authored side of one identity: none, added, removed, or changed.
enum class AuthoredChange {
    none,
    added,
    removed,
    changed,
};

AuthoredChange authored_change_of(const MergeInput& input, const SourceKey& key) {
    const auto added = std::find_if(input.delta.added.begin(), input.delta.added.end(),
        [&key](const PreparedEntityChange& change) { return key_of(change.entity) == key; });
    if (added != input.delta.added.end()) return AuthoredChange::added;
    const auto removed = std::find_if(input.delta.removed.begin(), input.delta.removed.end(),
        [&key](const PreparedEntityChange& change) { return key_of(change.entity) == key; });
    if (removed != input.delta.removed.end()) return AuthoredChange::removed;
    const auto changed = std::find_if(input.delta.changed.begin(), input.delta.changed.end(),
        [&key](const PreparedEntityChange& change) { return key_of(change.entity) == key; });
    if (changed != input.delta.changed.end()) return AuthoredChange::changed;
    return AuthoredChange::none;
}

const PreparedEntityChange* find_change(
    const std::vector<PreparedEntityChange>& changes, const SourceKey& key) {
    const auto found = std::find_if(changes.begin(), changes.end(),
        [&key](const PreparedEntityChange& change) { return key_of(change.entity) == key; });
    return found == changes.end() ? nullptr : &*found;
}

PreparedEntityRef ref_for_key(const MergeInput& input, const SourceKey& key) {
    const auto incoming = input.incoming_index.find(key);
    if (incoming != input.incoming_index.end() &&
        incoming->second.correspondence != nullptr) {
        return ref_from(*incoming->second.correspondence);
    }
    const auto base = input.base_index.find(key);
    if (base != input.base_index.end() && base->second.correspondence != nullptr) {
        return ref_from(*base->second.correspondence);
    }
    // Unreachable for validated inputs: every merged identity carries a
    // correspondence on at least one leg.
    PreparedEntityRef ref;
    ref.source_kind = key.kind;
    ref.source_primary = key.primary;
    ref.source_secondary = key.secondary;
    return ref;
}

} // namespace

const char* runtime_condition_kind_name(RuntimeConditionKind condition) noexcept {
    switch (condition) {
    case RuntimeConditionKind::intact: return "intact";
    case RuntimeConditionKind::damaged: return "damaged";
    case RuntimeConditionKind::looted: return "looted";
    case RuntimeConditionKind::damaged_and_looted: return "damaged-and-looted";
    }
    return "unknown";
}

core::Result<void> RuntimeSourceState::validate() const {
    if (!valid_source_kind(source_kind) || source_primary == 0U ||
        (source_kind == PreparedSourceEntityKind::material_region) !=
            (source_secondary != 0U) ||
        !valid_condition(condition)) {
        return core::Result<void>::failure(invalid(
            "runtime source state identity or condition is invalid"));
    }
    if (present) {
        if (state_digest.is_zero() || admitted_state_digest.is_zero() ||
            placement_digest.is_zero() || admitted_placement_digest.is_zero()) {
            return core::Result<void>::failure(invalid(
                "a present runtime state requires non-zero state and placement digests"));
        }
    } else if (!state_digest.is_zero() || !admitted_state_digest.is_zero() ||
        !placement_digest.is_zero() || !admitted_placement_digest.is_zero() ||
        condition != RuntimeConditionKind::intact || parent_source.has_value() ||
        footprint.has_value()) {
        return core::Result<void>::failure(invalid(
            "a destroyed runtime state must be a canonical tombstone"));
    }
    if (source_kind != PreparedSourceEntityKind::scene_node && parent_source.has_value()) {
        return core::Result<void>::failure(invalid(
            "only scene nodes may carry a runtime parent identity"));
    }
    if (parent_source.has_value() && parent_source.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "a runtime parent identity must be non-zero"));
    }
    if (footprint.has_value()) {
        if (auto result = footprint->validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string RuntimeSourceState::canonical() const {
    std::ostringstream output;
    output << "state-v1{kind=" << static_cast<unsigned>(source_kind)
           << ";primary=" << source_primary << ";secondary=" << source_secondary
           << ";present=" << (present ? 1U : 0U)
           << ";condition=" << runtime_condition_kind_name(condition) << ";state=";
    write_digest(output, state_digest);
    output << ";admitted-state=";
    write_digest(output, admitted_state_digest);
    output << ";placement=";
    write_digest(output, placement_digest);
    output << ";admitted-placement=";
    write_digest(output, admitted_placement_digest);
    output << ";parent=" << parent_source.value_or(0U) << ";footprint=";
    if (footprint.has_value()) {
        output << footprint->canonical();
    } else {
        output << "none";
    }
    output << '}';
    return output.str();
}

core::Result<void> RuntimeWorldSnapshot::validate() const {
    if (!safe_identity(source_namespace) || live_revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "runtime snapshot namespace or live revision is invalid"));
    }
    if (states.size() > kMaxStates) {
        return core::Result<void>::failure(validation(
            "runtime snapshot exceeds its bounded state capacity"));
    }
    if (!strictly_sorted_unique(states)) {
        return core::Result<void>::failure(validation(
            "runtime states must be canonically sorted and unique by source identity"));
    }
    for (const auto& state : states) {
        if (auto result = state.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

std::string RuntimeWorldSnapshot::serialize() const {
    std::ostringstream output;
    output << kSnapshotMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << live_revision.value() << '\n';
    output << "STATES " << states.size() << '\n';
    for (const auto& state : states) {
        output << "STATE " << static_cast<unsigned>(state.source_kind) << ' '
               << state.source_primary << ' ' << state.source_secondary << ' '
               << (state.present ? 1U : 0U) << ' '
               << static_cast<unsigned>(state.condition) << ' ';
        write_digest(output, state.state_digest);
        output << ' ';
        write_digest(output, state.admitted_state_digest);
        output << ' ';
        write_digest(output, state.placement_digest);
        output << ' ';
        write_digest(output, state.admitted_placement_digest);
        output << ' ' << state.parent_source.value_or(0U) << ' '
               << (state.footprint.has_value() ? 1U : 0U);
        if (state.footprint.has_value()) write_footprint(output, state.footprint.value());
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<RuntimeWorldSnapshot> RuntimeWorldSnapshot::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<RuntimeWorldSnapshot>::failure(validation(
            "runtime snapshot exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kSnapshotMagic) || !read_unsigned(input, version)) {
        return core::Result<RuntimeWorldSnapshot>::failure(validation(
            "runtime snapshot header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RuntimeWorldSnapshot>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "runtime snapshot schema version is unsupported"));
    }
    RuntimeWorldSnapshot snapshot;
    std::uint64_t revision = 0U;
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(snapshot.source_namespace)) ||
        !read_unsigned(input, revision)) {
        return core::Result<RuntimeWorldSnapshot>::failure(validation(
            "runtime snapshot source record is invalid"));
    }
    snapshot.live_revision = core::Revision{revision};

    std::size_t count = 0U;
    if (!require_token(input, "STATES") || !read_count(input, kMaxStates, count)) {
        return core::Result<RuntimeWorldSnapshot>::failure(validation(
            "runtime snapshot state count is invalid"));
    }
    snapshot.states.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        RuntimeSourceState state;
        bool has_footprint = false;
        std::uint64_t parent = 0U;
        if (!require_token(input, "STATE") ||
            !read_enum(input,
                static_cast<unsigned>(PreparedSourceEntityKind::material_region),
                state.source_kind) ||
            !read_unsigned(input, state.source_primary) ||
            !read_unsigned(input, state.source_secondary) ||
            !read_bool(input, state.present) ||
            !read_enum(input,
                static_cast<unsigned>(RuntimeConditionKind::damaged_and_looted),
                state.condition) ||
            !read_digest_token(input, state.state_digest) ||
            !read_digest_token(input, state.admitted_state_digest) ||
            !read_digest_token(input, state.placement_digest) ||
            !read_digest_token(input, state.admitted_placement_digest) ||
            !read_unsigned(input, parent) ||
            !read_bool(input, has_footprint)) {
            return core::Result<RuntimeWorldSnapshot>::failure(validation(
                "runtime snapshot state record is invalid"));
        }
        if (parent != 0U) state.parent_source = parent;
        if (has_footprint) {
            SpatialTileRange footprint;
            if (!read_footprint(input, footprint)) {
                return core::Result<RuntimeWorldSnapshot>::failure(validation(
                    "runtime snapshot footprint is invalid"));
            }
            state.footprint = std::move(footprint);
        }
        snapshot.states.push_back(std::move(state));
    }
    if (!require_token(input, "END")) {
        return core::Result<RuntimeWorldSnapshot>::failure(validation(
            "runtime snapshot is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<RuntimeWorldSnapshot>::failure(validation(
            "runtime snapshot contains trailing data"));
    }
    if (auto result = snapshot.validate(); !result) {
        return core::Result<RuntimeWorldSnapshot>::failure(result.error());
    }
    return core::Result<RuntimeWorldSnapshot>::success(std::move(snapshot));
}

core::Result<assets::Sha256Digest> RuntimeWorldSnapshot::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

const RuntimeSourceState* RuntimeWorldSnapshot::find(
    PreparedSourceEntityKind source_kind,
    std::uint64_t source_primary,
    std::uint64_t source_secondary) const noexcept {
    const SourceKey key{source_kind, source_primary, source_secondary};
    const auto found = std::lower_bound(states.begin(), states.end(), key,
        [](const RuntimeSourceState& state, const SourceKey& wanted) {
            return key_of(state) < wanted;
        });
    if (found == states.end() || !(key_of(*found) == key)) return nullptr;
    return &*found;
}

core::Result<void> PreparedEntityRef::validate() const {
    const bool region_source = source_kind == PreparedSourceEntityKind::material_region;
    const bool binding_target = prepared_kind == PreparedEntityKind::material_binding;
    const bool pair_ok =
        (source_kind == PreparedSourceEntityKind::scene_node &&
            prepared_kind == PreparedEntityKind::node) ||
        (source_kind == PreparedSourceEntityKind::mesh_asset &&
            prepared_kind == PreparedEntityKind::asset) ||
        (region_source && binding_target);
    if (!valid_source_kind(source_kind) || !valid_prepared_kind(prepared_kind) ||
        source_primary == 0U || prepared_primary == 0U ||
        (region_source != (source_secondary != 0U)) ||
        (binding_target != (prepared_secondary != 0U)) || !pair_ok) {
        return core::Result<void>::failure(invalid(
            "prepared entity reference is invalid"));
    }
    return core::Result<void>::success();
}

std::string PreparedEntityRef::canonical() const {
    std::ostringstream output;
    output << "entity{source=" << static_cast<unsigned>(source_kind) << ':'
           << source_primary << ':' << source_secondary << ";prepared="
           << static_cast<unsigned>(prepared_kind) << ':' << prepared_primary << ':'
           << prepared_secondary << '}';
    return output.str();
}

core::Result<void> PreparedEntityChange::validate() const {
    if (auto result = entity.validate(); !result) return result;
    if ((aspects & ~kKnownPreparedChangeAspects) != 0U) {
        return core::Result<void>::failure(invalid(
            "prepared entity change carries unknown aspects"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedRevisionDelta::validate() const {
    if (!safe_identity(source_namespace) || base_revision.exhausted() ||
        incoming_revision.exhausted() || base_digest.is_zero() ||
        incoming_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "prepared revision delta header is invalid"));
    }
    if (added.size() > kMaxChanges || removed.size() > kMaxChanges ||
        changed.size() > kMaxChanges) {
        return core::Result<void>::failure(validation(
            "prepared revision delta exceeds a bounded collection size"));
    }
    if (!strictly_sorted_unique(added) || !strictly_sorted_unique(removed) ||
        !strictly_sorted_unique(changed)) {
        return core::Result<void>::failure(validation(
            "prepared delta records must be canonically sorted and unique"));
    }
    for (const auto& change : added) {
        if (auto result = change.validate(); !result) return result;
        if (change.aspects != 0U) {
            return core::Result<void>::failure(validation(
                "added delta records carry no aspects"));
        }
    }
    for (const auto& change : removed) {
        if (auto result = change.validate(); !result) return result;
        if (change.aspects != 0U) {
            return core::Result<void>::failure(validation(
                "removed delta records carry no aspects"));
        }
    }
    for (const auto& change : changed) {
        if (auto result = change.validate(); !result) return result;
        if (change.aspects == 0U) {
            return core::Result<void>::failure(validation(
                "changed delta records require at least one aspect"));
        }
    }
    return core::Result<void>::success();
}

std::string PreparedRevisionDelta::serialize() const {
    std::ostringstream output;
    output << kDeltaMagic << ' ' << kSchemaVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << base_revision.value() << ' ' << incoming_revision.value() << ' '
           << base_digest.hex() << ' ' << incoming_digest.hex() << '\n';
    const auto write_change = [&output](const PreparedEntityChange& change) {
        write_entity_ref(output, change.entity);
        if (change.aspects != 0U) output << ' ' << change.aspects;
    };
    write_entries(output, "ADDED", "ADD", added, write_change);
    write_entries(output, "REMOVED", "REMOVE", removed, write_change);
    write_entries(output, "CHANGED", "CHANGE", changed, write_change);
    output << "END\n";
    return output.str();
}

core::Result<PreparedRevisionDelta> PreparedRevisionDelta::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kDeltaMagic) || !read_unsigned(input, version)) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<PreparedRevisionDelta>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "prepared revision delta schema version is unsupported"));
    }
    PreparedRevisionDelta delta;
    std::uint64_t base_revision = 0U;
    std::uint64_t incoming_revision = 0U;
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(delta.source_namespace)) ||
        !read_unsigned(input, base_revision) ||
        !read_unsigned(input, incoming_revision)) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta source record is invalid"));
    }
    auto digest = read_digest_token(input, delta.base_digest);
    if (!digest) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta base digest is invalid"));
    }
    digest = read_digest_token(input, delta.incoming_digest);
    if (!digest) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta incoming digest is invalid"));
    }
    delta.base_revision = core::Revision{base_revision};
    delta.incoming_revision = core::Revision{incoming_revision};

    // Aspect-masked rows appear only in the CHANGED list; ADD/REMOVE rows end
    // after the entity reference. The reader closure therefore cannot share
    // one parse shape, so changed rows are parsed by a dedicated reader.
    auto read_plain = [&input](PreparedEntityChange& change) {
        return read_entity_ref(input, change.entity);
    };
    auto read_aspected = [&input](PreparedEntityChange& change) {
        return read_entity_ref(input, change.entity) &&
            read_unsigned(input, change.aspects);
    };
    if (!read_entries(input, "ADDED", "ADD", kMaxChanges, delta.added, read_plain) ||
        !read_entries(input, "REMOVED", "REMOVE", kMaxChanges, delta.removed,
            read_plain) ||
        !read_entries(input, "CHANGED", "CHANGE", kMaxChanges, delta.changed,
            read_aspected)) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta record is invalid"));
    }
    if (!require_token(input, "END")) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision delta contains trailing data"));
    }
    if (auto result = delta.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    return core::Result<PreparedRevisionDelta>::success(std::move(delta));
}

core::Result<assets::Sha256Digest> PreparedRevisionDelta::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<PreparedRevisionDelta> diff_prepared_revisions(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming) {
    if (auto result = base.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (auto result = incoming.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (base.source_namespace != incoming.source_namespace) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revisions must share one source namespace to be diffed"));
    }

    const auto base_index = index_envelope(base);
    const auto incoming_index = index_envelope(incoming);
    std::set<SourceKey> keys;
    for (const auto& [key, records] : base_index) {
        (void)records;
        keys.insert(key);
    }
    for (const auto& [key, records] : incoming_index) {
        (void)records;
        keys.insert(key);
    }

    PreparedRevisionDelta delta;
    delta.source_namespace = base.source_namespace;
    delta.base_revision = base.source_revision;
    delta.incoming_revision = incoming.source_revision;
    const auto base_digest = base.canonical_digest();
    const auto incoming_digest = incoming.canonical_digest();
    if (!base_digest || !incoming_digest) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "prepared revision digests could not be computed"));
    }
    delta.base_digest = base_digest.value();
    delta.incoming_digest = incoming_digest.value();
    for (const auto& key : keys) {
        const auto base_records = base_index.find(key);
        const auto incoming_records = incoming_index.find(key);
        const bool in_base = base_records != base_index.end();
        const bool in_incoming = incoming_records != incoming_index.end();
        if (!in_base && in_incoming) {
            PreparedEntityChange change;
            change.entity = ref_from(*incoming_records->second.correspondence);
            delta.added.push_back(std::move(change));
        } else if (in_base && !in_incoming) {
            PreparedEntityChange change;
            change.entity = ref_from(*base_records->second.correspondence);
            delta.removed.push_back(std::move(change));
        } else {
            const std::uint32_t aspects =
                diff_aspects(base_records->second, incoming_records->second);
            if (aspects != 0U) {
                PreparedEntityChange change;
                change.entity = ref_from(*incoming_records->second.correspondence);
                change.aspects = aspects;
                delta.changed.push_back(std::move(change));
            }
        }
    }
    if (auto result = delta.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    return core::Result<PreparedRevisionDelta>::success(std::move(delta));
}

core::Result<PreparedRevisionDelta> filter_delta_to_region(
    const PreparedRevisionDelta& delta,
    const RuntimeWorldSnapshot& live,
    const SpatialTileRange& dirty_region) {
    if (auto result = delta.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (auto result = live.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (auto result = dirty_region.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (delta.source_namespace != live.source_namespace) {
        return core::Result<PreparedRevisionDelta>::failure(validation(
            "region filtering requires the delta and snapshot to share one namespace"));
    }
    PreparedRevisionDelta filtered = delta;
    // Entities without a runtime footprint are global and stay dirty; an
    // intersecting footprint keeps the change; a disjoint footprint drops it.
    // Grid or level mismatches fail closed through SpatialTileRange.
    auto filter_list = [&live, &dirty_region](std::vector<PreparedEntityChange>& changes)
        -> core::Result<void> {
        std::vector<PreparedEntityChange> kept;
        kept.reserve(changes.size());
        for (const auto& change : changes) {
            const RuntimeSourceState* state = live.find(change.entity.source_kind,
                change.entity.source_primary, change.entity.source_secondary);
            if (state == nullptr || !state->footprint.has_value()) {
                kept.push_back(change);
                continue;
            }
            const auto intersects = state->footprint->intersects(dirty_region);
            if (!intersects) return core::Result<void>::failure(intersects.error());
            if (intersects.value()) kept.push_back(change);
        }
        changes = std::move(kept);
        return core::Result<void>::success();
    };
    if (auto result = filter_list(filtered.added); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (auto result = filter_list(filtered.removed); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (auto result = filter_list(filtered.changed); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    if (auto result = filtered.validate(); !result) {
        return core::Result<PreparedRevisionDelta>::failure(result.error());
    }
    return core::Result<PreparedRevisionDelta>::success(std::move(filtered));
}

const char* property_ownership_name(PropertyOwnership ownership) noexcept {
    switch (ownership) {
    case PropertyOwnership::carto_owned: return "carto-owned";
    case PropertyOwnership::game_owned: return "game-owned";
    case PropertyOwnership::conflict: return "conflict";
    }
    return "unknown";
}

core::Result<void> PropertyOwnershipRecord::validate() const {
    if (!valid_source_kind(source_kind) || source_primary == 0U ||
        (source_kind == PreparedSourceEntityKind::material_region) !=
            (source_secondary != 0U)) {
        return core::Result<void>::failure(invalid(
            "property ownership record identity is invalid"));
    }
    return core::Result<void>::success();
}

std::string PropertyOwnershipRecord::canonical() const {
    std::ostringstream output;
    output << "ownership{source=" << static_cast<unsigned>(source_kind) << ':'
           << source_primary << ':' << source_secondary << "="
           << property_ownership_name(ownership) << '}';
    return output.str();
}

core::Result<std::vector<PropertyOwnershipRecord>> classify_property_ownership(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming,
    const RuntimeWorldSnapshot& live) {
    const auto merge = make_merge_input(base, incoming, live);
    if (!merge) {
        return core::Result<std::vector<PropertyOwnershipRecord>>::failure(merge.error());
    }
    std::vector<PropertyOwnershipRecord> records;
    for (const auto& key : merge_key_union(merge.value())) {
        const auto base_records = merge.value().base_index.find(key);
        const RuntimeSourceState* state = nullptr;
        if (const auto found = merge.value().live_states.find(key);
            found != merge.value().live_states.end()) {
            state = found->second;
        }
        const RuntimeDrift drift =
            drift_of(state, base_records != merge.value().base_index.end()
                    ? base_records->second
                    : EntityRecords{});
        const AuthoredChange authored = authored_change_of(merge.value(), key);
        PropertyOwnership ownership = PropertyOwnership::carto_owned;
        if (drift == RuntimeDrift::destroyed) {
            ownership = (authored == AuthoredChange::changed ||
                    authored == AuthoredChange::added)
                ? PropertyOwnership::conflict
                : PropertyOwnership::game_owned;
        } else if (drift != RuntimeDrift::none) {
            ownership = authored == AuthoredChange::none
                ? PropertyOwnership::game_owned
                : PropertyOwnership::conflict;
        }
        PropertyOwnershipRecord record;
        record.source_kind = key.kind;
        record.source_primary = key.primary;
        record.source_secondary = key.secondary;
        record.ownership = ownership;
        records.push_back(std::move(record));
    }
    if (!strictly_sorted_unique(records)) {
        return core::Result<std::vector<PropertyOwnershipRecord>>::failure(validation(
            "property ownership records must be canonically sorted and unique"));
    }
    return core::Result<std::vector<PropertyOwnershipRecord>>::success(std::move(records));
}

const char* reimport_conflict_kind_name(ReimportConflictKind kind) noexcept {
    switch (kind) {
    case ReimportConflictKind::destroyed: return "destroyed";
    case ReimportConflictKind::moved: return "moved";
    case ReimportConflictKind::damaged: return "damaged";
    case ReimportConflictKind::looted: return "looted";
    case ReimportConflictKind::hierarchy_reparent: return "hierarchy-reparent";
    }
    return "unknown";
}

core::Result<void> ReimportConflict::validate() const {
    if (!valid_conflict_kind(kind)) {
        return core::Result<void>::failure(invalid(
            "reimport conflict kind is invalid"));
    }
    if (auto result = entity.validate(); !result) return result;
    if (!safe_text(detail, kMaxDetailBytes, true)) {
        return core::Result<void>::failure(invalid(
            "reimport conflict detail is unsafe or too long"));
    }
    return core::Result<void>::success();
}

std::string ReimportConflict::canonical() const {
    std::ostringstream output;
    output << "conflict-v1{kind=" << reimport_conflict_kind_name(kind) << ';'
           << entity.canonical() << ";base=";
    write_digest(output, base_record_digest);
    output << ";incoming=";
    write_digest(output, incoming_record_digest);
    output << ";live=";
    write_digest(output, live_state_digest);
    output << ";detail=" << std::quoted(detail) << '}';
    return output.str();
}

core::Result<void> ReimportPlanEntry::validate() const {
    if (auto result = entity.validate(); !result) return result;
    if ((aspects & ~kKnownPreparedChangeAspects) != 0U) {
        return core::Result<void>::failure(invalid(
            "reimport plan entry carries unknown aspects"));
    }
    if (ownership == PropertyOwnership::conflict) {
        return core::Result<void>::failure(validation(
            "staged plan entries are never conflicts; conflicts carry their own list"));
    }
    return core::Result<void>::success();
}

std::string ReimportPlanEntry::canonical() const {
    std::ostringstream output;
    output << "entry-v1{" << entity.canonical() << ";aspects=" << aspects
           << ";ownership=" << property_ownership_name(ownership) << '}';
    return output.str();
}

core::Result<void> ReimportPlan::validate() const {
    if (!safe_identity(source_namespace) || base_revision.exhausted() ||
        incoming_revision.exhausted() || live_revision.exhausted() ||
        base_digest.is_zero() || incoming_digest.is_zero() ||
        live_snapshot_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "reimport plan header is invalid"));
    }
    if (creates.size() > kMaxEntries || updates.size() > kMaxEntries ||
        deletes.size() > kMaxEntries || conflicts.size() > kMaxEntries) {
        return core::Result<void>::failure(validation(
            "reimport plan exceeds a bounded collection size"));
    }
    if (!strictly_sorted_unique(creates) || !strictly_sorted_unique(updates) ||
        !strictly_sorted_unique(deletes) || !strictly_sorted_unique(conflicts)) {
        return core::Result<void>::failure(validation(
            "reimport plan records must be canonically sorted and unique"));
    }
    std::set<SourceKey> staged;
    const auto check_disjoint = [&staged](const auto& entries, bool expect_aspects) {
        for (const auto& entry : entries) {
            if (auto result = entry.validate(); !result) return result;
            if (expect_aspects ? entry.aspects == 0U : entry.aspects != 0U) {
                return core::Result<void>::failure(validation(
                    "reimport plan entry aspects do not match its action"));
            }
            if (!staged.insert(key_of(entry.entity)).second) {
                return core::Result<void>::failure(validation(
                    "one source identity cannot stage two plan actions"));
            }
        }
        return core::Result<void>::success();
    };
    if (auto result = check_disjoint(creates, false); !result) return result;
    if (auto result = check_disjoint(updates, true); !result) return result;
    if (auto result = check_disjoint(deletes, false); !result) return result;
    for (const auto& conflict : conflicts) {
        if (auto result = conflict.validate(); !result) return result;
        if (!staged.insert(key_of(conflict.entity)).second) {
            return core::Result<void>::failure(validation(
                "one source identity cannot stage two plan actions"));
        }
    }
    return core::Result<void>::success();
}

std::string ReimportPlan::serialize() const {
    std::ostringstream output;
    output << kPlanMagic << ' ' << kSchemaVersion << '\n';
    output << "TRIPLE " << std::quoted(source_namespace) << ' '
           << base_revision.value() << ' ' << incoming_revision.value() << ' '
           << live_revision.value() << ' ' << base_digest.hex() << ' '
           << incoming_digest.hex() << ' ' << live_snapshot_digest.hex() << '\n';
    const auto write_entry = [&output](const ReimportPlanEntry& entry) {
        write_entity_ref(output, entry.entity);
        if (entry.aspects != 0U) output << ' ' << entry.aspects;
    };
    write_entries(output, "CREATES", "CREATE", creates, write_entry);
    write_entries(output, "UPDATES", "UPDATE", updates, write_entry);
    write_entries(output, "DELETES", "DELETE", deletes, write_entry);
    const auto write_conflict = [&output](const ReimportConflict& conflict) {
        output << static_cast<unsigned>(conflict.kind) << ' ';
        write_entity_ref(output, conflict.entity);
        output << ' ';
        write_digest(output, conflict.base_record_digest);
        output << ' ';
        write_digest(output, conflict.incoming_record_digest);
        output << ' ';
        write_digest(output, conflict.live_state_digest);
        output << ' ' << std::quoted(conflict.detail);
    };
    write_entries(output, "CONFLICTS", "CONFLICT", conflicts, write_conflict);
    output << "END\n";
    return output.str();
}

core::Result<ReimportPlan> ReimportPlan::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kPlanMagic) || !read_unsigned(input, version)) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<ReimportPlan>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "reimport plan schema version is unsupported"));
    }
    ReimportPlan plan;
    std::uint64_t base_revision = 0U;
    std::uint64_t incoming_revision = 0U;
    std::uint64_t live_revision = 0U;
    if (!require_token(input, "TRIPLE") ||
        !(input >> std::quoted(plan.source_namespace)) ||
        !read_unsigned(input, base_revision) ||
        !read_unsigned(input, incoming_revision) ||
        !read_unsigned(input, live_revision)) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan triple header is invalid"));
    }
    if (!read_digest_token(input, plan.base_digest) ||
        !read_digest_token(input, plan.incoming_digest) ||
        !read_digest_token(input, plan.live_snapshot_digest)) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan triple digest is invalid"));
    }
    plan.base_revision = core::Revision{base_revision};
    plan.incoming_revision = core::Revision{incoming_revision};
    plan.live_revision = core::Revision{live_revision};

    auto read_entry = [&input](ReimportPlanEntry& entry) {
        return read_entity_ref(input, entry.entity);
    };
    auto read_update = [&input](ReimportPlanEntry& entry) {
        return read_entity_ref(input, entry.entity) &&
            read_unsigned(input, entry.aspects);
    };
    auto read_conflict = [&input](ReimportConflict& conflict) {
        unsigned kind = 0U;
        if (!read_unsigned(input, kind) ||
            kind > static_cast<unsigned>(ReimportConflictKind::hierarchy_reparent) ||
            !read_entity_ref(input, conflict.entity) ||
            !read_digest_token(input, conflict.base_record_digest) ||
            !read_digest_token(input, conflict.incoming_record_digest) ||
            !read_digest_token(input, conflict.live_state_digest) ||
            !(input >> std::quoted(conflict.detail))) {
            return false;
        }
        conflict.kind = static_cast<ReimportConflictKind>(kind);
        return true;
    };
    if (!read_entries(input, "CREATES", "CREATE", kMaxEntries, plan.creates, read_entry) ||
        !read_entries(input, "UPDATES", "UPDATE", kMaxEntries, plan.updates, read_update) ||
        !read_entries(input, "DELETES", "DELETE", kMaxEntries, plan.deletes, read_entry) ||
        !read_entries(input, "CONFLICTS", "CONFLICT", kMaxEntries, plan.conflicts,
            read_conflict)) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan record is invalid"));
    }
    if (!require_token(input, "END")) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ReimportPlan>::failure(validation(
            "reimport plan contains trailing data"));
    }
    if (auto result = plan.validate(); !result) {
        return core::Result<ReimportPlan>::failure(result.error());
    }
    return core::Result<ReimportPlan>::success(std::move(plan));
}

core::Result<assets::Sha256Digest> ReimportPlan::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<ReimportPlan> plan_reimport(
    const PreparedSceneEnvelope& base,
    const PreparedSceneEnvelope& incoming,
    const RuntimeWorldSnapshot& live) {
    const auto merge = make_merge_input(base, incoming, live);
    if (!merge) return core::Result<ReimportPlan>::failure(merge.error());

    ReimportPlan plan;
    plan.source_namespace = merge.value().base->source_namespace;
    plan.base_revision = merge.value().base->source_revision;
    plan.incoming_revision = merge.value().incoming->source_revision;
    plan.live_revision = merge.value().live->live_revision;
    plan.base_digest = merge.value().delta.base_digest;
    plan.incoming_digest = merge.value().delta.incoming_digest;
    const auto live_digest = merge.value().live->canonical_digest();
    if (!live_digest) return core::Result<ReimportPlan>::failure(live_digest.error());
    plan.live_snapshot_digest = live_digest.value();

    for (const auto& key : merge_key_union(merge.value())) {
        const auto base_records = merge.value().base_index.find(key);
        const EntityRecords base_entity_records =
            base_records != merge.value().base_index.end() ? base_records->second : EntityRecords{};
        const RuntimeSourceState* state = nullptr;
        if (const auto found = merge.value().live_states.find(key);
            found != merge.value().live_states.end()) {
            state = found->second;
        }
        const RuntimeDrift drift = drift_of(state, base_entity_records);
        const AuthoredChange authored = authored_change_of(merge.value(), key);
        const PreparedEntityRef ref = ref_for_key(merge.value(), key);

        if (drift == RuntimeDrift::destroyed) {
            if (authored == AuthoredChange::changed ||
                authored == AuthoredChange::added) {
                ReimportConflict conflict;
                conflict.kind = ReimportConflictKind::destroyed;
                conflict.entity = ref;
                conflict.base_record_digest = record_digest(base_entity_records);
                const auto incoming_records = merge.value().incoming_index.find(key);
                conflict.incoming_record_digest =
                    incoming_records != merge.value().incoming_index.end()
                    ? record_digest(incoming_records->second)
                    : assets::Sha256Digest{};
                conflict.live_state_digest = {};
                conflict.detail = drift_detail(RuntimeDrift::destroyed);
                plan.conflicts.push_back(std::move(conflict));
            } else if (authored == AuthoredChange::none) {
                // The entity stays dead: stage the deletion of any admitted
                // remnant so admission can never restage it.
                ReimportPlanEntry entry;
                entry.entity = ref;
                entry.ownership = PropertyOwnership::game_owned;
                plan.deletes.push_back(std::move(entry));
            }
            // Authored removal and runtime destruction agree: nothing staged.
            continue;
        }
        if (drift != RuntimeDrift::none) {
            if (authored == AuthoredChange::changed ||
                authored == AuthoredChange::removed) {
                ReimportConflict conflict;
                conflict.kind = conflict_kind_of(drift);
                conflict.entity = ref;
                conflict.base_record_digest = record_digest(base_entity_records);
                const auto incoming_records = merge.value().incoming_index.find(key);
                conflict.incoming_record_digest =
                    incoming_records != merge.value().incoming_index.end()
                    ? record_digest(incoming_records->second)
                    : assets::Sha256Digest{};
                conflict.live_state_digest = state != nullptr ? state->state_digest
                    : assets::Sha256Digest{};
                conflict.detail = drift_detail(drift);
                plan.conflicts.push_back(std::move(conflict));
            }
            // Drift without an authored change preserves game-owned state and
            // stages nothing.
            continue;
        }
        switch (authored) {
        case AuthoredChange::added: {
            ReimportPlanEntry entry;
            entry.entity = ref;
            entry.ownership = PropertyOwnership::carto_owned;
            plan.creates.push_back(std::move(entry));
            break;
        }
        case AuthoredChange::changed: {
            const auto* change = find_change(merge.value().delta.changed, key);
            ReimportPlanEntry entry;
            entry.entity = ref;
            entry.aspects = change != nullptr ? change->aspects : 0U;
            entry.ownership = PropertyOwnership::carto_owned;
            plan.updates.push_back(std::move(entry));
            break;
        }
        case AuthoredChange::removed: {
            ReimportPlanEntry entry;
            entry.entity = ref;
            entry.ownership = PropertyOwnership::carto_owned;
            plan.deletes.push_back(std::move(entry));
            break;
        }
        case AuthoredChange::none:
            break;
        }
    }
    if (auto result = plan.validate(); !result) {
        return core::Result<ReimportPlan>::failure(result.error());
    }
    return core::Result<ReimportPlan>::success(std::move(plan));
}

const char* reimport_operator_decision_name(
    ReimportOperatorDecision decision) noexcept {
    switch (decision) {
    case ReimportOperatorDecision::admit: return "admit";
    case ReimportOperatorDecision::override_decision: return "override";
    case ReimportOperatorDecision::skip: return "skip";
    }
    return "unknown";
}

core::Result<void> ReimportOperatorChoice::validate() const {
    if (!valid_source_kind(source_kind) || source_primary == 0U ||
        (source_kind == PreparedSourceEntityKind::material_region) !=
            (source_secondary != 0U) ||
        !valid_decision(decision) ||
        !safe_text(operator_identity, kMaxOperatorBytes, true)) {
        return core::Result<void>::failure(invalid(
            "reimport operator choice is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ReimportDecisionRecord::validate() const {
    if (!valid_conflict_kind(conflict_kind) || !valid_source_kind(source_kind) ||
        source_primary == 0U ||
        (source_kind == PreparedSourceEntityKind::material_region) !=
            (source_secondary != 0U) ||
        !valid_decision(decision) ||
        !safe_text(operator_identity, kMaxOperatorBytes, true)) {
        return core::Result<void>::failure(invalid(
            "reimport decision record is invalid"));
    }
    return core::Result<void>::success();
}

std::string ReimportDecisionRecord::canonical() const {
    std::ostringstream output;
    output << "decision-v1{kind=" << reimport_conflict_kind_name(conflict_kind)
           << ";source=" << static_cast<unsigned>(source_kind) << ':' << source_primary
           << ':' << source_secondary
           << ";decision=" << reimport_operator_decision_name(decision) << ";base=";
    write_digest(output, base_record_digest);
    output << ";incoming=";
    write_digest(output, incoming_record_digest);
    output << ";live=";
    write_digest(output, live_state_digest);
    output << ";operator=" << std::quoted(operator_identity) << '}';
    return output.str();
}

core::Result<void> ReimportReceipt::validate() const {
    if (!safe_identity(source_namespace) || base_revision.exhausted() ||
        incoming_revision.exhausted() || live_revision.exhausted() ||
        base_digest.is_zero() || incoming_digest.is_zero() ||
        live_snapshot_digest.is_zero() || plan_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "reimport receipt header is invalid"));
    }
    if (decisions.size() > kMaxDecisions) {
        return core::Result<void>::failure(validation(
            "reimport receipt exceeds its bounded decision capacity"));
    }
    if (!strictly_sorted_unique(decisions)) {
        return core::Result<void>::failure(validation(
            "reimport decisions must be canonically sorted and unique"));
    }
    for (const auto& decision : decisions) {
        if (auto result = decision.validate(); !result) return result;
    }
    if (receipt_digest.is_zero() ||
        receipt_digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "reimport receipt digest does not match its canonical evidence"));
    }
    return core::Result<void>::success();
}

std::string ReimportReceipt::canonical_without_digest() const {
    std::ostringstream output;
    output << kReceiptMagic << ' ' << kSchemaVersion << '\n';
    output << "TRIPLE " << std::quoted(source_namespace) << ' '
           << base_revision.value() << ' ' << incoming_revision.value() << ' '
           << live_revision.value() << ' ' << base_digest.hex() << ' '
           << incoming_digest.hex() << ' ' << live_snapshot_digest.hex() << '\n';
    output << "PLAN " << plan_digest.hex() << '\n';
    output << "DECISIONS " << decisions.size() << '\n';
    for (const auto& decision : decisions) {
        output << "DECISION " << static_cast<unsigned>(decision.conflict_kind) << ' '
               << static_cast<unsigned>(decision.source_kind) << ' '
               << decision.source_primary << ' ' << decision.source_secondary << ' '
               << static_cast<unsigned>(decision.decision) << ' ';
        write_digest(output, decision.base_record_digest);
        output << ' ';
        write_digest(output, decision.incoming_record_digest);
        output << ' ';
        write_digest(output, decision.live_state_digest);
        output << ' ' << std::quoted(decision.operator_identity) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<void> ReimportReceipt::refresh_digest() {
    if (!safe_identity(source_namespace) || base_revision.exhausted() ||
        incoming_revision.exhausted() || live_revision.exhausted() ||
        base_digest.is_zero() || incoming_digest.is_zero() ||
        live_snapshot_digest.is_zero() || plan_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "reimport receipt header is invalid"));
    }
    for (const auto& decision : decisions) {
        if (auto result = decision.validate(); !result) return result;
    }
    receipt_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string ReimportReceipt::serialize() const {
    std::ostringstream output;
    output << canonical_without_digest();
    output << "DIGEST ";
    if (receipt_digest.is_zero()) {
        output << "-";
    } else {
        output << receipt_digest.hex();
    }
    output << '\n';
    return output.str();
}

core::Result<ReimportReceipt> ReimportReceipt::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kReceiptMagic) || !read_unsigned(input, version)) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<ReimportReceipt>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "reimport receipt schema version is unsupported"));
    }
    ReimportReceipt receipt;
    std::uint64_t base_revision = 0U;
    std::uint64_t incoming_revision = 0U;
    std::uint64_t live_revision = 0U;
    if (!require_token(input, "TRIPLE") ||
        !(input >> std::quoted(receipt.source_namespace)) ||
        !read_unsigned(input, base_revision) ||
        !read_unsigned(input, incoming_revision) ||
        !read_unsigned(input, live_revision)) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt triple header is invalid"));
    }
    if (!read_digest_token(input, receipt.base_digest) ||
        !read_digest_token(input, receipt.incoming_digest) ||
        !read_digest_token(input, receipt.live_snapshot_digest)) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt triple digest is invalid"));
    }
    receipt.base_revision = core::Revision{base_revision};
    receipt.incoming_revision = core::Revision{incoming_revision};
    receipt.live_revision = core::Revision{live_revision};

    if (!require_token(input, "PLAN")) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt plan binding is invalid"));
    }
    const auto plan_digest = read_digest_token(input, receipt.plan_digest);
    if (!plan_digest) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt plan digest is invalid"));
    }

    std::size_t count = 0U;
    if (!require_token(input, "DECISIONS") ||
        !read_count(input, kMaxDecisions, count)) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt decision count is invalid"));
    }
    receipt.decisions.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        ReimportDecisionRecord decision;
        unsigned kind = 0U;
        unsigned source_kind = 0U;
        unsigned decision_value = 0U;
        if (!require_token(input, "DECISION") ||
            !read_unsigned(input, kind) ||
            kind > static_cast<unsigned>(ReimportConflictKind::hierarchy_reparent) ||
            !read_unsigned(input, source_kind) ||
            source_kind >
                static_cast<unsigned>(PreparedSourceEntityKind::material_region) ||
            !read_unsigned(input, decision.source_primary) ||
            !read_unsigned(input, decision.source_secondary) ||
            !read_unsigned(input, decision_value) ||
            decision_value >
                static_cast<unsigned>(ReimportOperatorDecision::skip) ||
            !read_digest_token(input, decision.base_record_digest) ||
            !read_digest_token(input, decision.incoming_record_digest) ||
            !read_digest_token(input, decision.live_state_digest) ||
            !(input >> std::quoted(decision.operator_identity))) {
            return core::Result<ReimportReceipt>::failure(validation(
                "reimport receipt decision record is invalid"));
        }
        decision.conflict_kind = static_cast<ReimportConflictKind>(kind);
        decision.source_kind = static_cast<PreparedSourceEntityKind>(source_kind);
        decision.decision = static_cast<ReimportOperatorDecision>(decision_value);
        receipt.decisions.push_back(std::move(decision));
    }
    if (!require_token(input, "END") || !require_token(input, "DIGEST")) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt digest record is invalid"));
    }
    const auto receipt_digest = read_digest_token(input, receipt.receipt_digest);
    if (!receipt_digest) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt digest is invalid"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ReimportReceipt>::failure(validation(
            "reimport receipt contains trailing data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<ReimportReceipt>::failure(result.error());
    }
    return core::Result<ReimportReceipt>::success(std::move(receipt));
}

core::Result<ReimportReceipt> record_reimport_receipt(
    const ReimportPlan& plan,
    std::span<const ReimportOperatorChoice> choices) {
    if (auto result = plan.validate(); !result) {
        return core::Result<ReimportReceipt>::failure(result.error());
    }
    std::map<SourceKey, const ReimportOperatorChoice*> by_identity;
    for (const auto& choice : choices) {
        if (auto result = choice.validate(); !result) {
            return core::Result<ReimportReceipt>::failure(result.error());
        }
        const SourceKey key{choice.source_kind, choice.source_primary,
            choice.source_secondary};
        if (!by_identity.emplace(key, &choice).second) {
            return core::Result<ReimportReceipt>::failure(invalid(
                "duplicate operator choice for one conflict"));
        }
    }

    ReimportReceipt receipt;
    receipt.source_namespace = plan.source_namespace;
    receipt.base_revision = plan.base_revision;
    receipt.incoming_revision = plan.incoming_revision;
    receipt.live_revision = plan.live_revision;
    receipt.base_digest = plan.base_digest;
    receipt.incoming_digest = plan.incoming_digest;
    receipt.live_snapshot_digest = plan.live_snapshot_digest;
    const auto plan_digest = plan.canonical_digest();
    if (!plan_digest) return core::Result<ReimportReceipt>::failure(plan_digest.error());
    receipt.plan_digest = plan_digest.value();

    for (const auto& conflict : plan.conflicts) {
        const auto choice = by_identity.find(key_of(conflict.entity));
        if (choice == by_identity.end()) {
            return core::Result<ReimportReceipt>::failure(validation(
                "every reimport conflict requires an explicit operator decision"));
        }
        ReimportDecisionRecord record;
        record.conflict_kind = conflict.kind;
        record.source_kind = conflict.entity.source_kind;
        record.source_primary = conflict.entity.source_primary;
        record.source_secondary = conflict.entity.source_secondary;
        record.decision = choice->second->decision;
        record.base_record_digest = conflict.base_record_digest;
        record.incoming_record_digest = conflict.incoming_record_digest;
        record.live_state_digest = conflict.live_state_digest;
        record.operator_identity = choice->second->operator_identity;
        receipt.decisions.push_back(std::move(record));
    }
    if (by_identity.size() != plan.conflicts.size()) {
        return core::Result<ReimportReceipt>::failure(invalid(
            "operator choices must match the plan conflicts exactly"));
    }
    if (auto result = receipt.refresh_digest(); !result) {
        return core::Result<ReimportReceipt>::failure(result.error());
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<ReimportReceipt>::failure(result.error());
    }
    return core::Result<ReimportReceipt>::success(std::move(receipt));
}

} // namespace carto::production
