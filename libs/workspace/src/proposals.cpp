#include <carto/workspace/proposals.hpp>

#include "codec.hpp"
#include "record_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::workspace {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic not_found(std::string message) {
    return core::Diagnostic(core::ErrorCode::not_found, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

// ---- shared digest helpers ------------------------------------------------

assets::Sha256Digest digest_of(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

// Parses one quoted digest member (the command/proposal spelling quotes every
// digest, so the bare-hex shared codec cannot be reused).
core::Result<assets::Sha256Digest> digest_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = codec::find(members, key);
    if (member == nullptr) {
        return core::Result<assets::Sha256Digest>::failure(codec::missing(key));
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) {
        return core::Result<assets::Sha256Digest>::failure(decoded.error());
    }
    return assets::Sha256Digest::from_hex(decoded.value());
}

core::Result<std::vector<assets::Sha256Digest>> quoted_digest_array(
    std::string_view raw, std::size_t max_entries) {
    const auto elements = codec::split_array(raw);
    if (!elements) {
        return core::Result<std::vector<assets::Sha256Digest>>::failure(elements.error());
    }
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<assets::Sha256Digest>>::failure(codec::malformed(
            "workspace digest array exceeds its bounded capacity"));
    }
    std::vector<assets::Sha256Digest> digests;
    digests.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto decoded = core::json::decode_string(element);
        if (!decoded) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(decoded.error());
        }
        const auto digest = assets::Sha256Digest::from_hex(decoded.value());
        if (!digest) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(digest.error());
        }
        digests.push_back(digest.value());
    }
    return core::Result<std::vector<assets::Sha256Digest>>::success(std::move(digests));
}

// Strictly-increasing ordering check (sorted and unique under operator<).
template <typename T, typename Key>
bool sorted_unique(const std::vector<T>& items, const Key& key) {
    for (std::size_t index = 1U; index < items.size(); ++index) {
        if (!(key(items[index - 1U]) < key(items[index]))) return false;
    }
    return true;
}

// ---- record fragments -----------------------------------------------------

std::string uncertainty_json(const UncertaintyDiagnostic& entry) {
    codec::ObjectWriter writer;
    writer.add("aspect", entry.aspect);
    writer.add("confidence", entry.confidence);
    writer.add("note", entry.note);
    return std::move(writer).finish();
}

core::Result<UncertaintyDiagnostic> uncertainty_from(std::string_view raw) {
    const auto members = core::json::parse_object(raw);
    if (!members) {
        return core::Result<UncertaintyDiagnostic>::failure(members.error());
    }
    UncertaintyDiagnostic entry;
    const auto aspect = codec::string_member(members.value(), "aspect");
    if (!aspect) return core::Result<UncertaintyDiagnostic>::failure(aspect.error());
    entry.aspect = aspect.value();
    const auto confidence = codec::double_member(members.value(), "confidence");
    if (!confidence) {
        return core::Result<UncertaintyDiagnostic>::failure(confidence.error());
    }
    entry.confidence = confidence.value();
    const auto note = codec::string_member(members.value(), "note");
    if (!note) return core::Result<UncertaintyDiagnostic>::failure(note.error());
    entry.note = note.value();
    return core::Result<UncertaintyDiagnostic>::success(std::move(entry));
}

std::string quality_metric_json(const ExpectedQualityMetric& metric) {
    codec::ObjectWriter writer;
    writer.add("metric", metric.metric);
    writer.add("value", metric.value);
    writer.add("tolerance", metric.tolerance);
    return std::move(writer).finish();
}

core::Result<ExpectedQualityMetric> quality_metric_from(std::string_view raw) {
    const auto members = core::json::parse_object(raw);
    if (!members) {
        return core::Result<ExpectedQualityMetric>::failure(members.error());
    }
    ExpectedQualityMetric metric;
    const auto name = codec::string_member(members.value(), "metric");
    if (!name) return core::Result<ExpectedQualityMetric>::failure(name.error());
    metric.metric = name.value();
    const auto value = codec::double_member(members.value(), "value");
    if (!value) return core::Result<ExpectedQualityMetric>::failure(value.error());
    metric.value = value.value();
    const auto tolerance = codec::double_member(members.value(), "tolerance");
    if (!tolerance) {
        return core::Result<ExpectedQualityMetric>::failure(tolerance.error());
    }
    metric.tolerance = tolerance.value();
    return core::Result<ExpectedQualityMetric>::success(std::move(metric));
}

template <typename T>
core::Result<std::vector<T>> object_array_from(
    std::string_view raw,
    std::size_t max_entries,
    core::Result<T> (*parse_one)(std::string_view)) {
    const auto elements = codec::split_array(raw);
    if (!elements) {
        return core::Result<std::vector<T>>::failure(elements.error());
    }
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<T>>::failure(codec::malformed(
            "workspace record array exceeds its bounded capacity"));
    }
    std::vector<T> parsed;
    parsed.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto item = parse_one(element);
        if (!item) return core::Result<std::vector<T>>::failure(item.error());
        parsed.push_back(item.value());
    }
    return core::Result<std::vector<T>>::success(std::move(parsed));
}

core::Result<ProposalEffect> effect_from(std::string_view name) {
    return record::scan_enum(name, ProposalEffect::rig_structure,
                             ProposalEffect::review_gates, proposal_effect_name);
}

std::string effect_array_json(const std::vector<ProposalEffect>& effects) {
    std::string out = "[";
    bool first = true;
    for (const ProposalEffect effect : effects) {
        if (!first) out.push_back(',');
        first = false;
        out += codec::quote(proposal_effect_name(effect));
    }
    out.push_back(']');
    return out;
}

core::Result<std::vector<ProposalEffect>> effect_array_from(
    const std::vector<core::json::Member>& members, std::string_view key,
    std::size_t max_entries) {
    const auto elements = codec::array_member(members, key);
    if (!elements) {
        return core::Result<std::vector<ProposalEffect>>::failure(elements.error());
    }
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<ProposalEffect>>::failure(codec::malformed(
            "workspace effect array exceeds its bounded capacity"));
    }
    std::vector<ProposalEffect> effects;
    effects.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto decoded = core::json::decode_string(element);
        if (!decoded) {
            return core::Result<std::vector<ProposalEffect>>::failure(decoded.error());
        }
        const auto effect = effect_from(decoded.value());
        if (!effect) {
            return core::Result<std::vector<ProposalEffect>>::failure(effect.error());
        }
        effects.push_back(effect.value());
    }
    return core::Result<std::vector<ProposalEffect>>::success(std::move(effects));
}

std::string string_array_json(const std::vector<std::string>& values) {
    std::string out = "[";
    bool first = true;
    for (const std::string& value : values) {
        if (!first) out.push_back(',');
        first = false;
        out += codec::quote(value);
    }
    out.push_back(']');
    return out;
}

// ---- envelope assembly ----------------------------------------------------

UncertaintyDiagnostic make_uncertainty(
    std::string aspect, double confidence, std::string note) {
    UncertaintyDiagnostic entry;
    entry.aspect = std::move(aspect);
    entry.confidence = confidence;
    entry.note = std::move(note);
    return entry;
}

// Derives the deterministic proposal identity from the pinned source state
// and the recipe: identical inputs always produce the identical proposal id,
// so re-running a generator against unchanged source is a no-op for identity.
journal::Uuid derive_proposal_id(
    ProposalKind kind,
    character::CharacterId target,
    core::Revision source_revision,
    const assets::Sha256Digest& recipe_digest,
    const assets::Sha256Digest& objective_text_digest,
    const std::string& provider_build_id,
    const std::vector<WorkspaceCommand>& operations) {
    std::string seed = "carto.workspace.proposal-id.v1|";
    seed += proposal_kind_name(kind);
    seed.push_back('|');
    seed += std::to_string(target.value);
    seed.push_back('|');
    seed += std::to_string(source_revision.value());
    seed.push_back('|');
    seed += recipe_digest.hex();
    seed.push_back('|');
    seed += objective_text_digest.hex();
    seed.push_back('|');
    seed += provider_build_id;
    seed.push_back('|');
    for (const WorkspaceCommand& operation : operations) {
        seed += workspace_command_kind_name(operation.kind());
        seed.push_back(';');
    }
    const assets::Sha256Digest derived = digest_of(seed);
    journal::Uuid proposal_id;
    std::copy_n(derived.bytes.begin(), proposal_id.bytes.size(), proposal_id.bytes.begin());
    return proposal_id;
}

// Assembles the shared proposal envelope from materialized native commands.
// This is the only materialization path: a proposal never mutates the store
// and never produces anything that is not an ordinary WorkspaceCommand.
core::Result<AuthoringProposal> assemble_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    ProposalKind kind,
    std::vector<WorkspaceCommand> operations,
    std::vector<std::string> explicit_target_ids,
    std::vector<ProposalEffect> effects,
    std::vector<UncertaintyDiagnostic> uncertainty,
    std::vector<std::string> assumptions,
    std::vector<ExpectedQualityMetric> metrics,
    std::uint64_t max_vertex_changes,
    std::uint64_t max_joint_changes,
    const assets::Sha256Digest& recipe_digest,
    const assets::Sha256Digest& objective_text_digest,
    const std::string& provider_build_id,
    std::string_view evidence_seed) {
    const CharacterSourceBundle* bundle = store.find(target);
    if (bundle == nullptr) {
        return core::Result<AuthoringProposal>::failure(not_found(
            "workspace character is not registered"));
    }
    const auto source_digest = store.canonical_digest();
    if (!source_digest) {
        return core::Result<AuthoringProposal>::failure(source_digest.error());
    }
    std::sort(explicit_target_ids.begin(), explicit_target_ids.end());
    explicit_target_ids.erase(
        std::unique(explicit_target_ids.begin(), explicit_target_ids.end()),
        explicit_target_ids.end());

    AuthoringProposal envelope;
    envelope.proposal_id = derive_proposal_id(kind, target, store.revision(),
                                              recipe_digest, objective_text_digest,
                                              provider_build_id, operations);
    envelope.kind = kind;
    envelope.source_namespace = bundle->definition.project_namespace;
    envelope.source_revision = store.revision();
    envelope.source_digest = source_digest.value();
    envelope.objective_text_digest = objective_text_digest;
    envelope.provider_build_id = provider_build_id;
    envelope.explicit_target_ids = std::move(explicit_target_ids);
    envelope.permitted_effect_kinds = std::move(effects);
    envelope.operation_sequence = std::move(operations);
    envelope.declared_assumptions = std::move(assumptions);
    envelope.uncertainty_by_operation = std::move(uncertainty);
    envelope.expected_quality_metrics = std::move(metrics);
    envelope.max_vertex_changes = max_vertex_changes;
    envelope.max_joint_changes = max_joint_changes;
    envelope.max_time_ms = 0U;
    envelope.max_memory_bytes = 0U;
    envelope.internal_evidence_digest = digest_of(evidence_seed);
    envelope.expiry_or_revalidation_policy = "revalidate-if-source-revision-advances";
    if (auto result = envelope.validate(); !result) {
        return core::Result<AuthoringProposal>::failure(result.error());
    }
    return core::Result<AuthoringProposal>::success(std::move(envelope));
}

} // namespace

// ---- name tables ----------------------------------------------------------

std::string_view proposal_kind_name(ProposalKind kind) noexcept {
    switch (kind) {
    case ProposalKind::rig: return "rig";
    case ProposalKind::skin: return "skin";
    case ProposalKind::motion: return "motion";
    }
    return "unknown";
}

std::string_view proposal_effect_name(ProposalEffect effect) noexcept {
    switch (effect) {
    case ProposalEffect::rig_structure: return "rig-structure";
    case ProposalEffect::rig_rest: return "rig-rest";
    case ProposalEffect::rig_attachments: return "rig-attachments";
    case ProposalEffect::skin_weights: return "skin-weights";
    case ProposalEffect::motion_keys: return "motion-keys";
    case ProposalEffect::motion_policy: return "motion-policy";
    case ProposalEffect::sim_bindings: return "sim-bindings";
    case ProposalEffect::face_morphs: return "face-morphs";
    case ProposalEffect::review_gates: return "review-gates";
    }
    return "unknown";
}

WorkspaceDomain proposal_domain(ProposalKind kind) noexcept {
    switch (kind) {
    case ProposalKind::rig: return WorkspaceDomain::rig;
    case ProposalKind::skin: return WorkspaceDomain::skin;
    case ProposalKind::motion: return WorkspaceDomain::animate;
    }
    return WorkspaceDomain::rig;
}

ProposalEffect command_effect(WorkspaceCommandKind kind) noexcept {
    switch (kind) {
    case WorkspaceCommandKind::rig_add_joint:
    case WorkspaceCommandKind::rig_remove_joint:
    case WorkspaceCommandKind::rig_add_control:
    case WorkspaceCommandKind::rig_remove_control:
    case WorkspaceCommandKind::rig_set_control_axes:
        return ProposalEffect::rig_structure;
    case WorkspaceCommandKind::rig_set_rest:
        return ProposalEffect::rig_rest;
    case WorkspaceCommandKind::rig_bind_attachment:
    case WorkspaceCommandKind::rig_unbind_attachment:
        return ProposalEffect::rig_attachments;
    case WorkspaceCommandKind::skin_paint_stamp:
    case WorkspaceCommandKind::skin_set_weight_lock:
    case WorkspaceCommandKind::skin_rebind_layer:
        return ProposalEffect::skin_weights;
    case WorkspaceCommandKind::animate_set_key:
    case WorkspaceCommandKind::animate_remove_key:
        return ProposalEffect::motion_keys;
    case WorkspaceCommandKind::animate_set_root_motion:
        return ProposalEffect::motion_policy;
    case WorkspaceCommandKind::face_add_morph_target:
    case WorkspaceCommandKind::face_remove_morph_target:
        return ProposalEffect::face_morphs;
    case WorkspaceCommandKind::sim_bind_garment:
    case WorkspaceCommandKind::sim_unbind_garment:
    case WorkspaceCommandKind::sim_enqueue_bake:
    case WorkspaceCommandKind::sim_cancel_bake:
    case WorkspaceCommandKind::sim_discard_bake:
    case WorkspaceCommandKind::sim_attach_bake_receipt:
    case WorkspaceCommandKind::sim_clear_bake_receipt:
        return ProposalEffect::sim_bindings;
    case WorkspaceCommandKind::review_record_gate:
        return ProposalEffect::review_gates;
    }
    return ProposalEffect::review_gates;
}

std::string_view proposal_decision_name(ProposalDecision decision) noexcept {
    switch (decision) {
    case ProposalDecision::accepted: return "accepted";
    case ProposalDecision::rejected: return "rejected";
    }
    return "unknown";
}

// ---- UncertaintyDiagnostic ------------------------------------------------

core::Result<void> UncertaintyDiagnostic::validate() const {
    if (!record::safe_text(aspect, kMaxAspectBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal uncertainty aspect is out of bounds"));
    }
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        return core::Result<void>::failure(invalid(
            "workspace proposal confidence is not a finite value in [0, 1]"));
    }
    if (!record::safe_text(note, kMaxNoteBytes, true)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal uncertainty note is out of bounds"));
    }
    return core::Result<void>::success();
}

// ---- ExpectedQualityMetric ------------------------------------------------

core::Result<void> ExpectedQualityMetric::validate() const {
    if (!record::safe_text(metric, kMaxMetricBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal quality metric name is out of bounds"));
    }
    if (!std::isfinite(value)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal quality metric value is not finite"));
    }
    if (!std::isfinite(tolerance) || tolerance < 0.0) {
        return core::Result<void>::failure(invalid(
            "workspace proposal quality metric tolerance is not finite and non-negative"));
    }
    return core::Result<void>::success();
}

// ---- AuthoringProposal ----------------------------------------------------

core::Result<void> AuthoringProposal::validate() const {
    if (serialize().size() > kMaxSerializedBytes) {
        return core::Result<void>::failure(validation(
            "workspace proposal exceeds its bounded serialized size"));
    }
    if (static_cast<std::uint8_t>(kind) > static_cast<std::uint8_t>(ProposalKind::motion)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal kind is unknown"));
    }
    if (proposal_id.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal carries the invalid zero proposal identity"));
    }
    if (!record::safe_text(source_namespace, kMaxNamespaceBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal source namespace is out of bounds"));
    }
    if (source_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal carries no pinned source digest"));
    }
    if (objective_text_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal carries no objective text digest"));
    }
    if (!record::safe_text(provider_build_id, kMaxProviderBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal provider build id is out of bounds"));
    }
    if (referenced_assets.size() > kMaxAssets ||
        !sorted_unique(referenced_assets,
                       [](const assets::Sha256Digest& digest) { return digest; })) {
        return core::Result<void>::failure(validation(
            "workspace proposal referenced assets are not sorted and unique within bounds"));
    }
    for (const assets::Sha256Digest& digest : referenced_assets) {
        if (digest.is_zero()) {
            return core::Result<void>::failure(invalid(
                "workspace proposal references a zero asset digest"));
        }
    }
    if (explicit_target_ids.size() > kMaxTargets ||
        !sorted_unique(explicit_target_ids,
                       [](const std::string& id) { return id; })) {
        return core::Result<void>::failure(validation(
            "workspace proposal explicit target ids are not sorted and unique within bounds"));
    }
    for (const std::string& id : explicit_target_ids) {
        if (!record::safe_text(id, kMaxTargetIdBytes)) {
            return core::Result<void>::failure(invalid(
                "workspace proposal explicit target id is out of bounds"));
        }
    }
    if (permitted_effect_kinds.empty() ||
        permitted_effect_kinds.size() > kMaxEffects ||
        !sorted_unique(permitted_effect_kinds,
                       [](ProposalEffect effect) {
                           return static_cast<std::uint8_t>(effect);
                       })) {
        return core::Result<void>::failure(validation(
            "workspace proposal permitted effects are not sorted and unique within bounds"));
    }
    for (const ProposalEffect effect : permitted_effect_kinds) {
        if (static_cast<std::uint8_t>(effect) >
            static_cast<std::uint8_t>(ProposalEffect::review_gates)) {
            return core::Result<void>::failure(invalid(
                "workspace proposal carries an unknown permitted effect"));
        }
    }
    if (operation_sequence.empty() || operation_sequence.size() > kMaxOperations) {
        return core::Result<void>::failure(validation(
            "workspace proposal operation sequence is empty or exceeds its bounded capacity"));
    }
    for (const WorkspaceCommand& operation : operation_sequence) {
        if (auto result = operation.validate(); !result) return result;
        if (operation.domain != proposal_domain(kind)) {
            return core::Result<void>::failure(validation(
                "workspace proposal operation does not belong to the proposal domain"));
        }
        const ProposalEffect required = command_effect(operation.kind());
        const auto admitted = std::binary_search(
            permitted_effect_kinds.begin(), permitted_effect_kinds.end(), required,
            [](ProposalEffect left, ProposalEffect right) {
                return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right);
            });
        if (!admitted) {
            return core::Result<void>::failure(validation(
                "workspace proposal operation exercises a non-permitted effect"));
        }
    }
    if (declared_assumptions.size() > kMaxAssumptions) {
        return core::Result<void>::failure(validation(
            "workspace proposal assumptions exceed their bounded capacity"));
    }
    for (const std::string& assumption : declared_assumptions) {
        if (!record::safe_text(assumption, kMaxAssumptionBytes)) {
            return core::Result<void>::failure(invalid(
                "workspace proposal assumption is out of bounds"));
        }
    }
    if (uncertainty_by_operation.size() != operation_sequence.size()) {
        return core::Result<void>::failure(validation(
            "workspace proposal must declare exactly one uncertainty per operation"));
    }
    for (const UncertaintyDiagnostic& entry : uncertainty_by_operation) {
        if (auto result = entry.validate(); !result) return result;
    }
    if (expected_quality_metrics.size() > kMaxMetrics) {
        return core::Result<void>::failure(validation(
            "workspace proposal quality metrics exceed their bounded capacity"));
    }
    for (const ExpectedQualityMetric& metric : expected_quality_metrics) {
        if (auto result = metric.validate(); !result) return result;
    }
    if (internal_evidence_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal carries no internal evidence digest"));
    }
    if (!record::safe_text(expiry_or_revalidation_policy, kMaxPolicyBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal expiry or revalidation policy is out of bounds"));
    }
    return core::Result<void>::success();
}

std::string AuthoringProposal::serialize() const {
    codec::ObjectWriter writer;
    writer.add("schema", static_cast<std::uint64_t>(kSchemaVersion));
    writer.add("proposal_id", proposal_id.hex());
    writer.add("kind", proposal_kind_name(kind));
    writer.add("source_namespace", source_namespace);
    writer.add("source_revision", source_revision.value());
    writer.add_raw("source_digest", codec::quote(source_digest.hex()));
    writer.add_raw("objective_text_digest", codec::quote(objective_text_digest.hex()));
    writer.add("provider_build_id", provider_build_id);
    {
        std::string out = "[";
        bool first = true;
        for (const assets::Sha256Digest& digest : referenced_assets) {
            if (!first) out.push_back(',');
            first = false;
            out += codec::quote(digest.hex());
        }
        out.push_back(']');
        writer.add_raw("referenced_assets", out);
    }
    writer.add_raw("explicit_target_ids", string_array_json(explicit_target_ids));
    writer.add_raw("permitted_effect_kinds", effect_array_json(permitted_effect_kinds));
    {
        std::string out = "[";
        bool first = true;
        for (const WorkspaceCommand& operation : operation_sequence) {
            if (!first) out.push_back(',');
            first = false;
            out += operation.serialize();
        }
        out.push_back(']');
        writer.add_raw("operation_sequence", out);
    }
    writer.add_raw("declared_assumptions", string_array_json(declared_assumptions));
    {
        std::string out = "[";
        bool first = true;
        for (const UncertaintyDiagnostic& entry : uncertainty_by_operation) {
            if (!first) out.push_back(',');
            first = false;
            out += uncertainty_json(entry);
        }
        out.push_back(']');
        writer.add_raw("uncertainty_by_operation", out);
    }
    {
        std::string out = "[";
        bool first = true;
        for (const ExpectedQualityMetric& metric : expected_quality_metrics) {
            if (!first) out.push_back(',');
            first = false;
            out += quality_metric_json(metric);
        }
        out.push_back(']');
        writer.add_raw("expected_quality_metrics", out);
    }
    writer.add("max_vertex_changes", max_vertex_changes);
    writer.add("max_joint_changes", max_joint_changes);
    writer.add("max_time_ms", max_time_ms);
    writer.add("max_memory_bytes", max_memory_bytes);
    writer.add_raw("internal_evidence_digest", codec::quote(internal_evidence_digest.hex()));
    writer.add("expiry_or_revalidation_policy", expiry_or_revalidation_policy);
    return std::move(writer).finish();
}

core::Result<assets::Sha256Digest> AuthoringProposal::canonical_digest() const {
    const std::string serialized = serialize();
    return core::Result<assets::Sha256Digest>::success(digest_of(serialized));
}

core::Result<AuthoringProposal> AuthoringProposal::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<AuthoringProposal>::failure(codec::malformed(
            "workspace proposal exceeds its bounded serialized size"));
    }
    const auto members = core::json::parse_object(text);
    if (!members) {
        return core::Result<AuthoringProposal>::failure(members.error());
    }
    const auto schema = codec::uint_member(members.value(), "schema");
    if (!schema) return core::Result<AuthoringProposal>::failure(schema.error());
    if (schema.value() != kSchemaVersion) {
        return core::Result<AuthoringProposal>::failure(codec::malformed(
            "workspace proposal schema version is not readable"));
    }
    AuthoringProposal envelope;
    const auto proposal_id = codec::string_member(members.value(), "proposal_id");
    if (!proposal_id) {
        return core::Result<AuthoringProposal>::failure(proposal_id.error());
    }
    const auto parsed_id = journal::Uuid::from_hex(proposal_id.value());
    if (!parsed_id) return core::Result<AuthoringProposal>::failure(parsed_id.error());
    envelope.proposal_id = parsed_id.value();
    const auto kind_text = codec::string_member(members.value(), "kind");
    if (!kind_text) return core::Result<AuthoringProposal>::failure(kind_text.error());
    const auto parsed_kind = record::scan_enum(
        kind_text.value(), ProposalKind::rig, ProposalKind::motion, proposal_kind_name);
    if (!parsed_kind) return core::Result<AuthoringProposal>::failure(parsed_kind.error());
    envelope.kind = parsed_kind.value();
    const auto source_namespace =
        codec::string_member(members.value(), "source_namespace");
    if (!source_namespace) {
        return core::Result<AuthoringProposal>::failure(source_namespace.error());
    }
    envelope.source_namespace = source_namespace.value();
    const auto source_revision = codec::uint_member(members.value(), "source_revision");
    if (!source_revision) {
        return core::Result<AuthoringProposal>::failure(source_revision.error());
    }
    envelope.source_revision = core::Revision{source_revision.value()};
    const auto source_digest = digest_member(members.value(), "source_digest");
    if (!source_digest) {
        return core::Result<AuthoringProposal>::failure(source_digest.error());
    }
    envelope.source_digest = source_digest.value();
    const auto objective = digest_member(members.value(), "objective_text_digest");
    if (!objective) return core::Result<AuthoringProposal>::failure(objective.error());
    envelope.objective_text_digest = objective.value();
    const auto provider = codec::string_member(members.value(), "provider_build_id");
    if (!provider) return core::Result<AuthoringProposal>::failure(provider.error());
    envelope.provider_build_id = provider.value();
    const core::json::Member* assets_member = codec::find(members.value(), "referenced_assets");
    if (assets_member == nullptr) {
        return core::Result<AuthoringProposal>::failure(codec::missing("referenced_assets"));
    }
    const auto referenced = quoted_digest_array(
        assets_member->raw_value, kMaxAssets);
    if (!referenced) return core::Result<AuthoringProposal>::failure(referenced.error());
    envelope.referenced_assets = referenced.value();
    const auto target_ids = codec::string_array_member(
        members.value(), "explicit_target_ids", kMaxTargets);
    if (!target_ids) {
        return core::Result<AuthoringProposal>::failure(target_ids.error());
    }
    envelope.explicit_target_ids = target_ids.value();
    const auto effects = effect_array_from(
        members.value(), "permitted_effect_kinds", kMaxEffects);
    if (!effects) return core::Result<AuthoringProposal>::failure(effects.error());
    envelope.permitted_effect_kinds = effects.value();
    const core::json::Member* operations_member =
        codec::find(members.value(), "operation_sequence");
    if (operations_member == nullptr) {
        return core::Result<AuthoringProposal>::failure(
            codec::missing("operation_sequence"));
    }
    const auto operation_texts = codec::split_array(operations_member->raw_value);
    if (!operation_texts) {
        return core::Result<AuthoringProposal>::failure(operation_texts.error());
    }
    if (operation_texts.value().size() > kMaxOperations) {
        return core::Result<AuthoringProposal>::failure(codec::malformed(
            "workspace proposal operation sequence exceeds its bounded capacity"));
    }
    envelope.operation_sequence.reserve(operation_texts.value().size());
    for (const std::string_view operation_text : operation_texts.value()) {
        const auto operation = WorkspaceCommand::deserialize(operation_text);
        if (!operation) {
            return core::Result<AuthoringProposal>::failure(operation.error());
        }
        envelope.operation_sequence.push_back(operation.value());
    }
    const auto assumptions = codec::string_array_member(
        members.value(), "declared_assumptions", kMaxAssumptions);
    if (!assumptions) {
        return core::Result<AuthoringProposal>::failure(assumptions.error());
    }
    envelope.declared_assumptions = assumptions.value();
    const core::json::Member* uncertainty_member =
        codec::find(members.value(), "uncertainty_by_operation");
    if (uncertainty_member == nullptr) {
        return core::Result<AuthoringProposal>::failure(
            codec::missing("uncertainty_by_operation"));
    }
    const auto uncertainty = object_array_from<UncertaintyDiagnostic>(
        uncertainty_member->raw_value, kMaxOperations, uncertainty_from);
    if (!uncertainty) {
        return core::Result<AuthoringProposal>::failure(uncertainty.error());
    }
    envelope.uncertainty_by_operation = uncertainty.value();
    const core::json::Member* metrics_member =
        codec::find(members.value(), "expected_quality_metrics");
    if (metrics_member == nullptr) {
        return core::Result<AuthoringProposal>::failure(
            codec::missing("expected_quality_metrics"));
    }
    const auto metrics = object_array_from<ExpectedQualityMetric>(
        metrics_member->raw_value, kMaxMetrics, quality_metric_from);
    if (!metrics) return core::Result<AuthoringProposal>::failure(metrics.error());
    envelope.expected_quality_metrics = metrics.value();
    const auto max_vertices = codec::uint_member(members.value(), "max_vertex_changes");
    if (!max_vertices) {
        return core::Result<AuthoringProposal>::failure(max_vertices.error());
    }
    envelope.max_vertex_changes = max_vertices.value();
    const auto max_joints = codec::uint_member(members.value(), "max_joint_changes");
    if (!max_joints) {
        return core::Result<AuthoringProposal>::failure(max_joints.error());
    }
    envelope.max_joint_changes = max_joints.value();
    const auto max_time = codec::uint_member(members.value(), "max_time_ms");
    if (!max_time) return core::Result<AuthoringProposal>::failure(max_time.error());
    envelope.max_time_ms = max_time.value();
    const auto max_memory = codec::uint_member(members.value(), "max_memory_bytes");
    if (!max_memory) {
        return core::Result<AuthoringProposal>::failure(max_memory.error());
    }
    envelope.max_memory_bytes = max_memory.value();
    const auto evidence = digest_member(members.value(), "internal_evidence_digest");
    if (!evidence) return core::Result<AuthoringProposal>::failure(evidence.error());
    envelope.internal_evidence_digest = evidence.value();
    const auto policy =
        codec::string_member(members.value(), "expiry_or_revalidation_policy");
    if (!policy) return core::Result<AuthoringProposal>::failure(policy.error());
    envelope.expiry_or_revalidation_policy = policy.value();
    if (auto result = envelope.validate(); !result) {
        return core::Result<AuthoringProposal>::failure(result.error());
    }
    return core::Result<AuthoringProposal>::success(std::move(envelope));
}

// ---- ProposalReview -------------------------------------------------------

core::Result<void> ProposalReview::validate() const {
    if (proposal_id.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review carries the invalid zero proposal identity"));
    }
    if (proposal_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review carries no proposal digest"));
    }
    if (static_cast<std::uint8_t>(decision) >
        static_cast<std::uint8_t>(ProposalDecision::rejected)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review decision is unknown"));
    }
    if (!record::safe_text(reviewer, kMaxReviewerBytes)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review reviewer is out of bounds"));
    }
    if (!record::safe_text(reason, kMaxReasonBytes, true)) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review reason is out of bounds"));
    }
    if (store_digest_before.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review carries no pre-review store digest"));
    }
    if (store_digest_after.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace proposal review carries no post-review store digest"));
    }
    if (decision == ProposalDecision::rejected) {
        if (store_digest_after != store_digest_before) {
            return core::Result<void>::failure(validation(
                "workspace proposal review records a source change on rejection"));
        }
        if (!applications.empty()) {
            return core::Result<void>::failure(validation(
                "workspace proposal review records applications on rejection"));
        }
        return core::Result<void>::success();
    }
    if (applications.empty() || applications.size() > kMaxApplications) {
        return core::Result<void>::failure(validation(
            "workspace proposal review applications are empty or exceed their bounded capacity"));
    }
    std::uint32_t previous_index = 0U;
    for (std::size_t index = 0U; index < applications.size(); ++index) {
        const ProposalApplication& application = applications[index];
        if (application.operation_index >= AuthoringProposal::kMaxOperations) {
            return core::Result<void>::failure(validation(
                "workspace proposal review application index is out of bounds"));
        }
        if (index > 0U && application.operation_index <= previous_index) {
            return core::Result<void>::failure(validation(
                "workspace proposal review applications are not in operation order"));
        }
        previous_index = application.operation_index;
        if (static_cast<std::uint8_t>(application.kind) >
            static_cast<std::uint8_t>(WorkspaceCommandKind::review_record_gate)) {
            return core::Result<void>::failure(invalid(
                "workspace proposal review application kind is unknown"));
        }
        if (application.revision_after.value() <= application.revision_before.value()) {
            return core::Result<void>::failure(validation(
                "workspace proposal review application does not advance the revision"));
        }
        if (application.command_digest.is_zero()) {
            return core::Result<void>::failure(invalid(
                "workspace proposal review application carries no command digest"));
        }
    }
    if (store_digest_after == store_digest_before) {
        return core::Result<void>::failure(validation(
            "workspace proposal review accepts without changing source"));
    }
    return core::Result<void>::success();
}

std::string ProposalReview::serialize() const {
    codec::ObjectWriter writer;
    writer.add("schema", static_cast<std::uint64_t>(1U));
    writer.add("proposal_id", proposal_id.hex());
    writer.add_raw("proposal_digest", codec::quote(proposal_digest.hex()));
    writer.add("decision", proposal_decision_name(decision));
    writer.add("reviewer", reviewer);
    writer.add("reason", reason);
    writer.add("review_revision", review_revision.value());
    writer.add_raw("store_digest_before", codec::quote(store_digest_before.hex()));
    writer.add_raw("store_digest_after", codec::quote(store_digest_after.hex()));
    std::string out = "[";
    bool first = true;
    for (const ProposalApplication& application : applications) {
        if (!first) out.push_back(',');
        first = false;
        codec::ObjectWriter entry;
        entry.add("operation_index",
                  static_cast<std::uint64_t>(application.operation_index));
        entry.add("kind", workspace_command_kind_name(application.kind));
        entry.add("revision_before", application.revision_before.value());
        entry.add("revision_after", application.revision_after.value());
        entry.add_raw("command_digest", codec::quote(application.command_digest.hex()));
        out += std::move(entry).finish();
    }
    out.push_back(']');
    writer.add_raw("applications", out);
    return std::move(writer).finish();
}

core::Result<ProposalReview> ProposalReview::deserialize(std::string_view text) {
    if (text.size() > 64U * 1024U * 1024U) {
        return core::Result<ProposalReview>::failure(codec::malformed(
            "workspace proposal review exceeds its bounded serialized size"));
    }
    const auto members = core::json::parse_object(text);
    if (!members) {
        return core::Result<ProposalReview>::failure(members.error());
    }
    const auto schema = codec::uint_member(members.value(), "schema");
    if (!schema) return core::Result<ProposalReview>::failure(schema.error());
    if (schema.value() != 1U) {
        return core::Result<ProposalReview>::failure(codec::malformed(
            "workspace proposal review schema version is not readable"));
    }
    ProposalReview review;
    const auto proposal_id = codec::string_member(members.value(), "proposal_id");
    if (!proposal_id) {
        return core::Result<ProposalReview>::failure(proposal_id.error());
    }
    const auto parsed_id = journal::Uuid::from_hex(proposal_id.value());
    if (!parsed_id) return core::Result<ProposalReview>::failure(parsed_id.error());
    review.proposal_id = parsed_id.value();
    const auto digest = digest_member(members.value(), "proposal_digest");
    if (!digest) return core::Result<ProposalReview>::failure(digest.error());
    review.proposal_digest = digest.value();
    const auto decision_text = codec::string_member(members.value(), "decision");
    if (!decision_text) {
        return core::Result<ProposalReview>::failure(decision_text.error());
    }
    const auto parsed_decision = record::scan_enum(
        decision_text.value(), ProposalDecision::accepted, ProposalDecision::rejected,
        proposal_decision_name);
    if (!parsed_decision) {
        return core::Result<ProposalReview>::failure(parsed_decision.error());
    }
    review.decision = parsed_decision.value();
    const auto reviewer_text = codec::string_member(members.value(), "reviewer");
    if (!reviewer_text) {
        return core::Result<ProposalReview>::failure(reviewer_text.error());
    }
    review.reviewer = reviewer_text.value();
    const auto reason = codec::string_member(members.value(), "reason");
    if (!reason) return core::Result<ProposalReview>::failure(reason.error());
    review.reason = reason.value();
    const auto review_revision = codec::uint_member(members.value(), "review_revision");
    if (!review_revision) {
        return core::Result<ProposalReview>::failure(review_revision.error());
    }
    review.review_revision = core::Revision{review_revision.value()};
    const auto before = digest_member(members.value(), "store_digest_before");
    if (!before) return core::Result<ProposalReview>::failure(before.error());
    review.store_digest_before = before.value();
    const auto after = digest_member(members.value(), "store_digest_after");
    if (!after) return core::Result<ProposalReview>::failure(after.error());
    review.store_digest_after = after.value();
    const core::json::Member* applications_member =
        codec::find(members.value(), "applications");
    if (applications_member == nullptr) {
        return core::Result<ProposalReview>::failure(codec::missing("applications"));
    }
    const auto application_texts = codec::split_array(applications_member->raw_value);
    if (!application_texts) {
        return core::Result<ProposalReview>::failure(application_texts.error());
    }
    if (application_texts.value().size() > kMaxApplications) {
        return core::Result<ProposalReview>::failure(codec::malformed(
            "workspace proposal review applications exceed their bounded capacity"));
    }
    review.applications.reserve(application_texts.value().size());
    for (const std::string_view application_text : application_texts.value()) {
        const auto application_members = core::json::parse_object(application_text);
        if (!application_members) {
            return core::Result<ProposalReview>::failure(application_members.error());
        }
        ProposalApplication application;
        const auto index = codec::u32_member(application_members.value(), "operation_index");
        if (!index) return core::Result<ProposalReview>::failure(index.error());
        application.operation_index = index.value();
        const auto kind_text = codec::string_member(application_members.value(), "kind");
        if (!kind_text) {
            return core::Result<ProposalReview>::failure(kind_text.error());
        }
        const auto parsed_kind = record::scan_enum(
            kind_text.value(), WorkspaceCommandKind::rig_add_joint,
            WorkspaceCommandKind::review_record_gate, workspace_command_kind_name);
        if (!parsed_kind) {
            return core::Result<ProposalReview>::failure(parsed_kind.error());
        }
        application.kind = parsed_kind.value();
        const auto revision_before =
            codec::uint_member(application_members.value(), "revision_before");
        if (!revision_before) {
            return core::Result<ProposalReview>::failure(revision_before.error());
        }
        application.revision_before = core::Revision{revision_before.value()};
        const auto revision_after =
            codec::uint_member(application_members.value(), "revision_after");
        if (!revision_after) {
            return core::Result<ProposalReview>::failure(revision_after.error());
        }
        application.revision_after = core::Revision{revision_after.value()};
        const auto command = digest_member(application_members.value(), "command_digest");
        if (!command) return core::Result<ProposalReview>::failure(command.error());
        application.command_digest = command.value();
        review.applications.push_back(application);
    }
    if (auto result = review.validate(); !result) {
        return core::Result<ProposalReview>::failure(result.error());
    }
    return core::Result<ProposalReview>::success(std::move(review));
}

// ---- typed proposal validation ---------------------------------------------

core::Result<void> RigProposal::validate() const {
    if (candidate_joints.empty() || candidate_joints.size() > kMaxCandidates) {
        return core::Result<void>::failure(validation(
            "workspace rig proposal candidates are empty or exceed their bounded capacity"));
    }
    if (recipe_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace rig proposal carries no recipe digest"));
    }
    if (uncertainty.size() != candidate_joints.size()) {
        return core::Result<void>::failure(validation(
            "workspace rig proposal must declare exactly one uncertainty per candidate"));
    }
    std::vector<kinematic::JointId> identities;
    identities.reserve(candidate_joints.size());
    for (const RigCandidateJoint& candidate : candidate_joints) {
        if (!candidate.joint) {
            return core::Result<void>::failure(invalid(
                "workspace rig proposal carries the invalid zero joint identity"));
        }
        if (candidate.parent.has_value() &&
            (!candidate.parent.value() || candidate.parent.value() == candidate.joint)) {
            return core::Result<void>::failure(validation(
                "workspace rig proposal parent is invalid or self-referential"));
        }
        if (!candidate.rest.finite()) {
            return core::Result<void>::failure(invalid(
                "workspace rig proposal rest transform is not finite"));
        }
        if (static_cast<std::uint8_t>(candidate.kind) >
            static_cast<std::uint8_t>(kinematic::JointKind::ball)) {
            return core::Result<void>::failure(invalid(
                "workspace rig proposal joint kind is unknown"));
        }
        if (!record::safe_text(candidate.semantic_type,
                               kinematic::KinematicGraph::kMaxSemanticBytes, true)) {
            return core::Result<void>::failure(invalid(
                "workspace rig proposal semantic type is out of bounds"));
        }
        identities.push_back(candidate.joint);
    }
    if (!sorted_unique(identities,
                       [](kinematic::JointId joint) { return joint; })) {
        return core::Result<void>::failure(validation(
            "workspace rig proposal candidate joints are not unique"));
    }
    for (const UncertaintyDiagnostic& entry : uncertainty) {
        if (auto result = entry.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> SkinProposal::validate() const {
    if (stamps.empty() || stamps.size() > kMaxStamps) {
        return core::Result<void>::failure(validation(
            "workspace skin proposal stamps are empty or exceed their bounded capacity"));
    }
    if (recipe_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace skin proposal carries no recipe digest"));
    }
    if (uncertainty.size() != stamps.size()) {
        return core::Result<void>::failure(validation(
            "workspace skin proposal must declare exactly one uncertainty per stamp"));
    }
    for (std::size_t index = 0U; index < stamps.size(); ++index) {
        const SkinCandidateStamp& stamp = stamps[index];
        if (stamp.vertex > kinematic::SkinBinding::kMaxVertexIndex) {
            return core::Result<void>::failure(invalid(
                "workspace skin proposal vertex index is out of range"));
        }
        if (index > 0U && stamp.vertex <= stamps[index - 1U].vertex) {
            return core::Result<void>::failure(validation(
                "workspace skin proposal stamps are not sorted and unique by vertex"));
        }
        if (auto result = character::validate_influences(stamp.influences); !result) {
            return result;
        }
    }
    for (const UncertaintyDiagnostic& entry : uncertainty) {
        if (auto result = entry.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> MotionProposal::validate() const {
    if (keys.empty() || keys.size() > kMaxKeys) {
        return core::Result<void>::failure(validation(
            "workspace motion proposal keys are empty or exceed their bounded capacity"));
    }
    if (recipe_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "workspace motion proposal carries no recipe digest"));
    }
    if (uncertainty.size() != keys.size()) {
        return core::Result<void>::failure(validation(
            "workspace motion proposal must declare exactly one uncertainty per key"));
    }
    for (std::size_t index = 0U; index < keys.size(); ++index) {
        const MotionCandidateKey& key = keys[index];
        if (!key.channel) {
            return core::Result<void>::failure(invalid(
                "workspace motion proposal carries the invalid zero channel identity"));
        }
        if (auto result = key.time.validate(); !result) return result;
        if (!std::isfinite(key.value)) {
            return core::Result<void>::failure(invalid(
                "workspace motion proposal key value is not finite"));
        }
        if (index > 0U) {
            const MotionCandidateKey& previous = keys[index - 1U];
            const bool ordered =
                previous.channel.value < key.channel.value ||
                (previous.channel.value == key.channel.value &&
                 (previous.time.tick < key.time.tick ||
                  (previous.time.tick == key.time.tick &&
                   previous.time.ticks_per_second < key.time.ticks_per_second)));
            if (!ordered) {
                return core::Result<void>::failure(validation(
                    "workspace motion proposal keys are not sorted and unique"));
            }
        }
    }
    for (const UncertaintyDiagnostic& entry : uncertainty) {
        if (auto result = entry.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

// ---- envelope builders -----------------------------------------------------

core::Result<AuthoringProposal> build_rig_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const RigProposal& proposal,
    const assets::Sha256Digest& objective_text_digest,
    std::string provider_build_id) {
    if (auto result = proposal.validate(); !result) {
        return core::Result<AuthoringProposal>::failure(result.error());
    }
    std::vector<WorkspaceCommand> operations;
    operations.reserve(proposal.candidate_joints.size());
    std::vector<std::string> target_ids;
    target_ids.reserve(proposal.candidate_joints.size());
    std::string evidence_seed = "carto.workspace.rig-proposal.v1|";
    for (const RigCandidateJoint& candidate : proposal.candidate_joints) {
        RigAddJoint payload;
        payload.expected_source_revision = store.revision();
        payload.joint = candidate.joint;
        payload.parent = candidate.parent;
        payload.rest = candidate.rest;
        payload.kind = candidate.kind;
        payload.semantic_type = candidate.semantic_type;
        WorkspaceCommand command;
        command.domain = WorkspaceDomain::rig;
        command.target_character = target;
        command.payload = WorkspaceCommandPayload{std::move(payload)};
        target_ids.push_back("joint:" + std::to_string(candidate.joint.value));
        evidence_seed += std::to_string(candidate.joint.value);
        evidence_seed.push_back(';');
        operations.push_back(std::move(command));
    }
    std::vector<ProposalEffect> effects;
    effects.push_back(ProposalEffect::rig_structure);
    std::vector<ExpectedQualityMetric> metrics;
    ExpectedQualityMetric metric;
    metric.metric = "added_joint_count";
    metric.value = static_cast<double>(proposal.candidate_joints.size());
    metric.tolerance = 0.0;
    metrics.push_back(std::move(metric));
    std::vector<std::string> assumptions;
    assumptions.push_back(
        "proposed joints continue the existing joint identity space and are "
        "expected to be absent from the target graph at admission");
    return assemble_proposal(store, target, ProposalKind::rig, std::move(operations),
                             std::move(target_ids), std::move(effects),
                             proposal.uncertainty, std::move(assumptions),
                             std::move(metrics), 0U,
                             static_cast<std::uint64_t>(proposal.candidate_joints.size()),
                             proposal.recipe_digest, objective_text_digest,
                             provider_build_id, evidence_seed);
}

core::Result<AuthoringProposal> build_skin_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const SkinProposal& proposal,
    const assets::Sha256Digest& objective_text_digest,
    std::string provider_build_id) {
    if (auto result = proposal.validate(); !result) {
        return core::Result<AuthoringProposal>::failure(result.error());
    }
    std::vector<WorkspaceCommand> operations;
    operations.reserve(proposal.stamps.size());
    std::vector<std::string> target_ids;
    target_ids.reserve(proposal.stamps.size());
    std::string evidence_seed = "carto.workspace.skin-proposal.v1|";
    for (const SkinCandidateStamp& stamp : proposal.stamps) {
        SkinPaintStamp payload;
        payload.expected_source_revision = store.revision();
        payload.vertex = stamp.vertex;
        payload.influences = stamp.influences;
        WorkspaceCommand command;
        command.domain = WorkspaceDomain::skin;
        command.target_character = target;
        command.payload = WorkspaceCommandPayload{std::move(payload)};
        target_ids.push_back("vertex:" + std::to_string(stamp.vertex));
        evidence_seed += std::to_string(stamp.vertex);
        evidence_seed.push_back(';');
        operations.push_back(std::move(command));
    }
    std::vector<ProposalEffect> effects;
    effects.push_back(ProposalEffect::skin_weights);
    std::vector<ExpectedQualityMetric> metrics;
    ExpectedQualityMetric metric;
    metric.metric = "stamped_vertex_count";
    metric.value = static_cast<double>(proposal.stamps.size());
    metric.tolerance = 0.0;
    metrics.push_back(std::move(metric));
    std::vector<std::string> assumptions;
    assumptions.push_back(
        "uniform single-influence prior is a starting point, not a result");
    assumptions.push_back("artist-locked vertices are excluded from this proposal");
    return assemble_proposal(store, target, ProposalKind::skin, std::move(operations),
                             std::move(target_ids), std::move(effects),
                             proposal.uncertainty, std::move(assumptions),
                             std::move(metrics),
                             static_cast<std::uint64_t>(proposal.stamps.size()), 0U,
                             proposal.recipe_digest, objective_text_digest,
                             provider_build_id, evidence_seed);
}

core::Result<AuthoringProposal> build_motion_proposal(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const MotionProposal& proposal,
    const assets::Sha256Digest& objective_text_digest,
    std::string provider_build_id) {
    if (auto result = proposal.validate(); !result) {
        return core::Result<AuthoringProposal>::failure(result.error());
    }
    std::vector<WorkspaceCommand> operations;
    operations.reserve(proposal.keys.size());
    std::vector<std::string> target_ids;
    target_ids.reserve(proposal.keys.size());
    std::string evidence_seed = "carto.workspace.motion-proposal.v1|";
    for (const MotionCandidateKey& key : proposal.keys) {
        AnimateSetKey payload;
        payload.expected_source_revision = store.revision();
        payload.channel = key.channel;
        payload.time = key.time;
        payload.value = key.value;
        WorkspaceCommand command;
        command.domain = WorkspaceDomain::animate;
        command.target_character = target;
        command.payload = WorkspaceCommandPayload{std::move(payload)};
        target_ids.push_back("channel:" + std::to_string(key.channel.value) + ":" +
                             std::to_string(key.time.tick) + ":" +
                             std::to_string(key.time.ticks_per_second));
        evidence_seed += std::to_string(key.channel.value);
        evidence_seed.push_back(';');
        operations.push_back(std::move(command));
    }
    std::vector<ProposalEffect> effects;
    effects.push_back(ProposalEffect::motion_keys);
    std::vector<ExpectedQualityMetric> metrics;
    ExpectedQualityMetric metric;
    metric.metric = "proposed_key_count";
    metric.value = static_cast<double>(proposal.keys.size());
    metric.tolerance = 0.0;
    metrics.push_back(std::move(metric));
    std::vector<std::string> assumptions;
    assumptions.push_back(
        "proposed keys restate authored keyframe times with one constant value");
    assumptions.push_back(
        "keys outside the channel duration are refused at admission");
    return assemble_proposal(store, target, ProposalKind::motion, std::move(operations),
                             std::move(target_ids), std::move(effects),
                             proposal.uncertainty, std::move(assumptions),
                             std::move(metrics), 0U, 0U, proposal.recipe_digest,
                             objective_text_digest, provider_build_id, evidence_seed);
}

// ---- deterministic local generators ---------------------------------------

namespace local {

core::Result<RigProposal> propose_rig_chain(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const RigChainOptions& options) {
    if (options.joint_count == 0U ||
        options.joint_count > RigProposal::kMaxCandidates) {
        return core::Result<RigProposal>::failure(invalid(
            "workspace rig chain joint count is out of bounds"));
    }
    if (!options.spacing.finite()) {
        return core::Result<RigProposal>::failure(invalid(
            "workspace rig chain spacing is not finite"));
    }
    const CharacterSourceBundle* bundle = store.find(target);
    if (bundle == nullptr) {
        return core::Result<RigProposal>::failure(not_found(
            "workspace character is not registered"));
    }
    if (auto result = bundle->graph.validate(); !result) {
        return core::Result<RigProposal>::failure(result.error());
    }
    std::uint64_t next_identity = 1U;
    for (const auto& [joint, node] : bundle->graph.nodes()) {
        (void)node;
        next_identity = std::max(next_identity, joint.value + 1U);
    }
    std::optional<kinematic::JointId> root;
    for (const auto& [joint, node] : bundle->graph.nodes()) {
        if (!node.parent.has_value()) {
            root = joint;
            break;
        }
    }
    RigProposal proposal;
    proposal.candidate_joints.reserve(options.joint_count);
    proposal.uncertainty.reserve(options.joint_count);
    std::string seed = "carto.workspace.local.rig-chain.v1|";
    seed += std::to_string(target.value);
    seed.push_back('|');
    seed += std::to_string(store.revision().value());
    seed.push_back('|');
    seed += std::to_string(options.joint_count);
    seed.push_back('|');
    seed += std::to_string(options.spacing.x);
    seed.push_back(',');
    seed += std::to_string(options.spacing.y);
    seed.push_back(',');
    seed += std::to_string(options.spacing.z);
    seed.push_back('|');
    for (std::uint32_t index = 0U; index < options.joint_count; ++index) {
        RigCandidateJoint candidate;
        candidate.joint = kinematic::JointId{next_identity};
        ++next_identity;
        if (index == 0U) {
            candidate.parent = bundle->graph.nodes().empty()
                                   ? std::optional<kinematic::JointId>{}
                                   : root;
        } else {
            candidate.parent = kinematic::JointId{next_identity - 2U};
        }
        core::Transform rest = core::Transform::identity();
        const double step = static_cast<double>(index);
        rest.translation.x = options.spacing.x * step;
        rest.translation.y = options.spacing.y * step;
        rest.translation.z = options.spacing.z * step;
        candidate.rest = rest;
        candidate.kind = kinematic::JointKind::ball;
        candidate.semantic_type = "chain.link";
        proposal.candidate_joints.push_back(candidate);
        // Honest uncertainty: confidence decays along the chain, so longer
        // spines are proposed with less certainty.
        const double confidence =
            std::max(0.05, 1.0 - 0.05 * static_cast<double>(index));
        proposal.uncertainty.push_back(make_uncertainty(
            "chain.link", confidence,
            "deterministic chain extension; confidence decays with depth"));
        seed += std::to_string(candidate.joint.value);
        seed.push_back(';');
    }
    proposal.recipe_digest = digest_of(seed);
    if (auto result = proposal.validate(); !result) {
        return core::Result<RigProposal>::failure(result.error());
    }
    return core::Result<RigProposal>::success(std::move(proposal));
}

core::Result<SkinProposal> propose_uniform_skin(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const UniformSkinOptions& options) {
    if (!options.default_joint) {
        return core::Result<SkinProposal>::failure(invalid(
            "workspace uniform skin proposal carries the invalid zero joint identity"));
    }
    const CharacterSourceBundle* bundle = store.find(target);
    if (bundle == nullptr) {
        return core::Result<SkinProposal>::failure(not_found(
            "workspace character is not registered"));
    }
    if (!bundle->skin.has_value()) {
        return core::Result<SkinProposal>::failure(not_found(
            "workspace character carries no skin binding to propose against"));
    }
    if (bundle->graph.find(options.default_joint) == nullptr) {
        return core::Result<SkinProposal>::failure(not_found(
            "workspace uniform skin default joint is not present in the graph"));
    }
    SkinProposal proposal;
    std::string seed = "carto.workspace.local.uniform-skin.v1|";
    seed += std::to_string(target.value);
    seed.push_back('|');
    seed += std::to_string(store.revision().value());
    seed.push_back('|');
    seed += std::to_string(options.default_joint.value);
    seed.push_back('|');
    for (const auto& [vertex, influences] : bundle->skin->influences) {
        (void)influences;
        // Artist-locked vertices are excluded from automatic weight proposals.
        if (std::binary_search(bundle->weight_locks.begin(),
                               bundle->weight_locks.end(), vertex)) {
            continue;
        }
        SkinCandidateStamp stamp;
        stamp.vertex = vertex;
        stamp.influences.push_back(
            kinematic::JointInfluence{options.default_joint, 1.0});
        proposal.stamps.push_back(std::move(stamp));
        proposal.uncertainty.push_back(make_uncertainty(
            "uniform.prior", 0.5,
            "uniform single-influence prior is a starting point, not a result"));
        seed += std::to_string(vertex);
        seed.push_back(';');
    }
    if (proposal.stamps.empty()) {
        return core::Result<SkinProposal>::failure(validation(
            "workspace uniform skin proposal has no unlocked influenced vertices"));
    }
    proposal.recipe_digest = digest_of(seed);
    if (auto result = proposal.validate(); !result) {
        return core::Result<SkinProposal>::failure(result.error());
    }
    return core::Result<SkinProposal>::success(std::move(proposal));
}

core::Result<MotionProposal> propose_static_keys(
    const CharacterSourceStore& store,
    character::CharacterId target,
    const StaticKeysOptions& options) {
    if (auto result = options.time.validate(); !result) {
        return core::Result<MotionProposal>::failure(result.error());
    }
    if (!std::isfinite(options.value)) {
        return core::Result<MotionProposal>::failure(invalid(
            "workspace static keys value is not finite"));
    }
    const CharacterSourceBundle* bundle = store.find(target);
    if (bundle == nullptr) {
        return core::Result<MotionProposal>::failure(not_found(
            "workspace character is not registered"));
    }
    if (!bundle->channel.has_value()) {
        return core::Result<MotionProposal>::failure(not_found(
            "workspace character carries no temporal channel to propose against"));
    }
    if (bundle->channel->value_kind != temporal::ChannelValueKind::scalar ||
        bundle->channel->scalar_keyframes.empty()) {
        return core::Result<MotionProposal>::failure(validation(
            "workspace channel does not carry scalar keyframes to restate"));
    }
    MotionProposal proposal;
    std::string seed = "carto.workspace.local.static-keys.v1|";
    seed += std::to_string(target.value);
    seed.push_back('|');
    seed += std::to_string(store.revision().value());
    seed.push_back('|');
    seed += std::to_string(options.time.tick);
    seed.push_back(':');
    seed += std::to_string(options.time.ticks_per_second);
    seed.push_back('|');
    for (const temporal::ScalarKeyframe& keyframe :
         bundle->channel->scalar_keyframes) {
        MotionCandidateKey key;
        key.channel = bundle->channel->id;
        key.value = options.value;
        proposal.keys.push_back(key);
        proposal.uncertainty.push_back(make_uncertainty(
            "static.pose", 0.5,
            "static constant restates an authored keyframe time; not a solve"));
        seed += std::to_string(keyframe.time);
        seed.push_back(';');
    }
    // Exact tick-grid times: every authored keyframe time is converted with
    // the channel timebase, never through a lossy intermediate.
    for (std::size_t index = 0U; index < proposal.keys.size(); ++index) {
        const auto converted = character::from_seconds(
            bundle->channel->scalar_keyframes[index].time,
            options.time.ticks_per_second);
        if (!converted) {
            return core::Result<MotionProposal>::failure(converted.error());
        }
        proposal.keys[index].time = converted.value().time;
    }
    proposal.recipe_digest = digest_of(seed);
    if (auto result = proposal.validate(); !result) {
        return core::Result<MotionProposal>::failure(result.error());
    }
    return core::Result<MotionProposal>::success(std::move(proposal));
}

} // namespace local

} // namespace carto::workspace
