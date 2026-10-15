#include <carto/agent/workbench.hpp>

#include <carto/project/project.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

namespace carto::agent {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_AGENT_TASK_CHECKPOINT_V1\n";
constexpr std::size_t kMaxCheckpointBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaxProjectBytes = 240U * 1024U * 1024U;
constexpr std::size_t kMaxTextBytes = 1U * 1024U * 1024U;
constexpr std::string_view kDemonstrationMagicV1 =
    "CARTOGRAPHER_AGENT_DEMONSTRATION_V1\n";
constexpr std::string_view kDemonstrationMagicV2 =
    "CARTOGRAPHER_AGENT_DEMONSTRATION_V2\n";
constexpr std::string_view kDemonstrationMagicV3 =
    "CARTOGRAPHER_AGENT_DEMONSTRATION_V3\n";
constexpr std::string_view kDemonstrationTraceMagic =
    "CARTOGRAPHER_AGENT_DEMONSTRATION_TRACE_V1\n";
constexpr std::string_view kDemonstrationRecipeMagic =
    "CARTOGRAPHER_AGENT_DEMONSTRATION_RECIPE_V1\n";
constexpr std::size_t kMaxDemonstrationBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxDemonstrationSteps = 4096U;
constexpr std::size_t kMaxDemonstrationGates = 256U;
constexpr std::size_t kMaxDemonstrationTraceBytes = 1U * 1024U * 1024U;
constexpr std::size_t kMaxDemonstrationTraceSteps = 256U;
constexpr std::size_t kMaxDemonstrationRecipeBytes = 2U * 1024U * 1024U;

core::Diagnostic malformed(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_demonstration_text(std::string_view value, std::size_t maximum) noexcept {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20U && character != 0x7fU;
        });
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

bool same_transform(const core::Transform& left, const core::Transform& right) noexcept {
    return left.translation.x == right.translation.x &&
        left.translation.y == right.translation.y &&
        left.translation.z == right.translation.z &&
        left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y &&
        left.rotation.z == right.rotation.z && left.rotation.w == right.rotation.w &&
        left.scale.x == right.scale.x && left.scale.y == right.scale.y &&
        left.scale.z == right.scale.z;
}

bool same_position(core::Vec3d left, core::Vec3d right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

assets::Sha256Digest ghost_correction_chain_digest(
    const std::vector<GhostCorrectionReceipt>& corrections,
    std::size_t count) {
    std::string canonical = "cartographer.agent.ghost-correction-chain.v1|";
    count = std::min(count, corrections.size());
    const std::string count_text = std::to_string(count);
    canonical += std::to_string(count_text.size());
    canonical.push_back(':');
    canonical += count_text;
    canonical.push_back('|');
    for (std::size_t index = 0U; index < count; ++index) {
        const std::string digest = corrections[index].evidence_digest.hex();
        canonical += std::to_string(digest.size());
        canonical.push_back(':');
        canonical += digest;
        canonical.push_back('|');
    }
    return digest_text(canonical);
}

class Writer final {
public:
    void field(std::string_view value) {
        bytes_ += std::to_string(value.size());
        bytes_.push_back(':');
        bytes_.append(value);
        bytes_.push_back('|');
    }

    void number(std::uint64_t value) { field(std::to_string(value)); }
    void boolean(bool value) { number(value ? 1U : 0U); }
    void real(double value) { number(std::bit_cast<std::uint64_t>(value)); }
    void digest(const assets::Sha256Digest& value) { field(value.hex()); }

    [[nodiscard]] std::string take() && { return std::move(bytes_); }

private:
    std::string bytes_;
};

class Reader final {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    [[nodiscard]] std::string field(std::size_t maximum = kMaxTextBytes) {
        if (!good_) return {};
        const std::size_t digits_begin = cursor_;
        while (cursor_ < bytes_.size() && bytes_[cursor_] >= '0' && bytes_[cursor_] <= '9') {
            ++cursor_;
        }
        if (cursor_ == digits_begin || cursor_ >= bytes_.size() || bytes_[cursor_] != ':') {
            fail("checkpoint field length is malformed");
            return {};
        }
        std::uint64_t length = 0U;
        const auto parsed = std::from_chars(
            bytes_.data() + digits_begin, bytes_.data() + cursor_, length);
        if (parsed.ec != std::errc{} || parsed.ptr != bytes_.data() + cursor_ ||
            length > maximum || cursor_ + 1U > bytes_.size() ||
            length > bytes_.size() - cursor_ - 1U) {
            fail("checkpoint field exceeds its bound");
            return {};
        }
        ++cursor_;
        const auto value = bytes_.substr(cursor_, static_cast<std::size_t>(length));
        cursor_ += static_cast<std::size_t>(length);
        if (cursor_ >= bytes_.size() || bytes_[cursor_] != '|') {
            fail("checkpoint field terminator is missing");
            return {};
        }
        ++cursor_;
        return std::string(value);
    }

    [[nodiscard]] std::uint64_t number() {
        const std::string value = field(32U);
        if (!good_) return 0U;
        std::uint64_t result = 0U;
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), result);
        if (value.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size()) {
            fail("checkpoint integer is malformed");
            return 0U;
        }
        return result;
    }

    [[nodiscard]] std::size_t count(std::size_t maximum) {
        const auto value = number();
        if (!good_) return 0U;
        if (value > maximum || value > std::numeric_limits<std::size_t>::max()) {
            fail("checkpoint collection exceeds its bound");
            return 0U;
        }
        return static_cast<std::size_t>(value);
    }

    [[nodiscard]] bool boolean() {
        const auto value = number();
        if (value > 1U) fail("checkpoint boolean is malformed");
        return value == 1U;
    }

    [[nodiscard]] double real() {
        const double value = std::bit_cast<double>(number());
        if (!std::isfinite(value)) fail("checkpoint floating-point value is not finite");
        return value;
    }

    [[nodiscard]] assets::Sha256Digest digest() {
        const auto parsed = assets::Sha256Digest::from_hex(field(64U));
        if (!parsed) {
            fail("checkpoint SHA-256 digest is malformed");
            return {};
        }
        return parsed.value();
    }

    void fail(std::string message) {
        if (good_) {
            good_ = false;
            diagnostic_ = malformed(std::move(message));
        }
    }

    [[nodiscard]] bool good() const noexcept { return good_; }
    [[nodiscard]] bool finished() const noexcept { return good_ && cursor_ == bytes_.size(); }
    [[nodiscard]] const core::Diagnostic& error() const noexcept { return diagnostic_; }

private:
    std::string_view bytes_;
    std::size_t cursor_ = 0U;
    bool good_ = true;
    core::Diagnostic diagnostic_{core::ErrorCode::validation_failed, "checkpoint parse failed"};
};

void write_object_ids(Writer& writer, const std::vector<scene::ObjectId>& values) {
    writer.number(values.size());
    for (const auto value : values) writer.number(value.value);
}

std::vector<scene::ObjectId> read_object_ids(Reader& reader) {
    std::vector<scene::ObjectId> values;
    const auto count = reader.count(512U);
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        values.push_back(scene::ObjectId{reader.number()});
    }
    return values;
}

void write_strings(Writer& writer, const std::vector<std::string>& values) {
    writer.number(values.size());
    for (const auto& value : values) writer.field(value);
}

std::vector<std::string> read_strings(Reader& reader, std::size_t maximum_count = 512U) {
    std::vector<std::string> values;
    const auto count = reader.count(maximum_count);
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) values.push_back(reader.field(1024U));
    return values;
}

void write_diagnostic(Writer& writer, const std::optional<core::Diagnostic>& diagnostic) {
    writer.boolean(diagnostic.has_value());
    if (!diagnostic.has_value()) return;
    writer.number(static_cast<std::uint64_t>(diagnostic->code));
    writer.field(diagnostic->message);
    writer.number(diagnostic->context.size());
    for (const auto& context : diagnostic->context) writer.field(context);
}

std::optional<core::Diagnostic> read_diagnostic(Reader& reader) {
    if (!reader.boolean()) return std::nullopt;
    const auto code = reader.number();
    const auto maximum_code = static_cast<std::uint64_t>(core::ErrorCode::version_mismatch);
    if (code > maximum_code) reader.fail("checkpoint diagnostic code is unknown");
    core::Diagnostic diagnostic(
        static_cast<core::ErrorCode>(code), reader.field(4096U));
    const auto count = reader.count(32U);
    diagnostic.context.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        diagnostic.context.push_back(reader.field(4096U));
    }
    return diagnostic;
}

void write_transform(Writer& writer, const core::Transform& transform) {
    writer.real(transform.translation.x);
    writer.real(transform.translation.y);
    writer.real(transform.translation.z);
    writer.real(transform.rotation.x);
    writer.real(transform.rotation.y);
    writer.real(transform.rotation.z);
    writer.real(transform.rotation.w);
    writer.real(transform.scale.x);
    writer.real(transform.scale.y);
    writer.real(transform.scale.z);
}

core::Transform read_transform(Reader& reader) {
    core::Transform transform;
    transform.translation = {reader.real(), reader.real(), reader.real()};
    transform.rotation = {
        reader.real(), reader.real(), reader.real(), reader.real()};
    transform.scale = {reader.real(), reader.real(), reader.real()};
    return transform;
}

void write_plan(Writer& writer, const AgentPlan& plan) {
    writer.field(plan.id);
    writer.field(plan.objective.id);
    writer.field(plan.objective.goal);
    writer.field(plan.objective.recipe_id);
    writer.number(plan.objective.base_revision.value());
    write_object_ids(writer, plan.objective.scope_objects);
    write_object_ids(writer, plan.objective.protected_objects);
    writer.number(plan.objective.hard_constraints.size());
    for (const auto& constraint : plan.objective.hard_constraints) {
        writer.field(constraint.id);
        writer.number(static_cast<std::uint64_t>(constraint.kind));
        writer.boolean(constraint.object.has_value());
        if (constraint.object.has_value()) writer.number(constraint.object->value);
        writer.real(constraint.maximum_displacement);
        writer.field(constraint.rationale);
    }
    writer.number(plan.objective.clarifications.size());
    for (const auto& question : plan.objective.clarifications) {
        writer.field(question.id);
        writer.field(question.prompt);
        writer.field(question.rationale);
        writer.number(question.impacts.size());
        for (const auto impact : question.impacts) {
            writer.number(static_cast<std::uint64_t>(impact));
        }
        write_object_ids(writer, question.affected_objects);
        write_strings(writer, question.affected_constraint_ids);
        writer.number(question.downstream_plan_branches);
        writer.number(question.candidates.size());
        for (const auto& candidate : question.candidates) {
            writer.field(candidate.id);
            writer.field(candidate.label);
            write_object_ids(writer, candidate.affected_objects);
        }
        writer.boolean(question.requires_explicit_answer);
        // Answers and their private receipts are never persisted.
    }
    writer.number(plan.steps.size());
    for (const auto& step : plan.steps) {
        writer.field(step.id);
        writer.number(step.target.object.value);
        writer.number(step.target.component.index());
        if (const auto* face = std::get_if<geometry::FaceId>(&step.target.component)) {
            writer.number(face->value);
        } else if (const auto* vertex =
                       std::get_if<geometry::VertexId>(&step.target.component)) {
            writer.number(vertex->value);
        }
        writer.field(step.intent);
        write_strings(writer, step.dependencies);
    }
}

AgentPlan read_plan(Reader& reader, std::uint64_t version) {
    AgentPlan plan;
    plan.id = reader.field(64U);
    plan.objective.id = reader.field(64U);
    plan.objective.goal = reader.field(1024U);
    if (version >= 5U) plan.objective.recipe_id = reader.field(64U);
    plan.objective.base_revision = core::Revision(reader.number());
    plan.objective.scope_objects = read_object_ids(reader);
    plan.objective.protected_objects = read_object_ids(reader);
    const auto constraint_count = reader.count(256U);
    plan.objective.hard_constraints.reserve(constraint_count);
    for (std::size_t index = 0U; index < constraint_count; ++index) {
        HardConstraint constraint;
        constraint.id = reader.field(64U);
        const auto kind = reader.number();
        if (kind > static_cast<std::uint64_t>(
                ObjectiveConstraintKind::maximum_vertex_displacement)) {
            reader.fail("checkpoint constraint kind is unknown");
        }
        constraint.kind = static_cast<ObjectiveConstraintKind>(kind);
        if (reader.boolean()) constraint.object = scene::ObjectId{reader.number()};
        constraint.maximum_displacement = reader.real();
        constraint.rationale = reader.field(512U);
        plan.objective.hard_constraints.push_back(std::move(constraint));
    }
    const auto question_count = reader.count(256U);
    plan.objective.clarifications.reserve(question_count);
    for (std::size_t index = 0U; index < question_count; ++index) {
        ClarificationQuestion question;
        question.id = reader.field(64U);
        question.prompt = reader.field(1024U);
        question.rationale = reader.field(512U);
        const auto impact_count = reader.count(16U);
        question.impacts.reserve(impact_count);
        for (std::size_t impact = 0U; impact < impact_count; ++impact) {
            const auto value = reader.number();
            if (value > static_cast<std::uint64_t>(ClarificationImpact::publication_readiness)) {
                reader.fail("checkpoint clarification impact is unknown");
            }
            question.impacts.push_back(static_cast<ClarificationImpact>(value));
        }
        question.affected_objects = read_object_ids(reader);
        question.affected_constraint_ids = read_strings(reader, 256U);
        question.downstream_plan_branches = reader.count(256U);
        const auto candidate_count = reader.count(128U);
        question.candidates.reserve(candidate_count);
        for (std::size_t candidate_index = 0U; candidate_index < candidate_count;
             ++candidate_index) {
            ClarificationCandidate candidate;
            candidate.id = reader.field(64U);
            candidate.label = reader.field(512U);
            candidate.affected_objects = read_object_ids(reader);
            question.candidates.push_back(std::move(candidate));
        }
        question.requires_explicit_answer = reader.boolean();
        question.selected_candidate_id.reset();
        question.freeform_answer.reset();
        question.human_receipt.reset();
        plan.objective.clarifications.push_back(std::move(question));
    }
    const auto step_count = reader.count(256U);
    plan.steps.reserve(step_count);
    for (std::size_t index = 0U; index < step_count; ++index) {
        AgentPlanStep step;
        step.id = reader.field(64U);
        step.target.object = scene::ObjectId{reader.number()};
        if (version >= 4U) {
            const auto target_kind = reader.number();
            if (target_kind == 0U) {
                step.target.component = geometry::FaceId{reader.number()};
            } else if (target_kind == 1U) {
                step.target.component = geometry::VertexId{reader.number()};
            } else if (target_kind == 2U) {
                step.target.component = std::monostate{};
            } else {
                reader.fail("checkpoint plan target kind is unknown");
            }
        } else {
            const bool is_face = reader.boolean();
            if (is_face) step.target.component = geometry::FaceId{reader.number()};
            else step.target.component = geometry::VertexId{reader.number()};
        }
        step.intent = reader.field(1024U);
        step.dependencies = read_strings(reader, 256U);
        plan.steps.push_back(std::move(step));
    }
    return plan;
}

void write_envelope(Writer& writer, const AgentWorkEnvelope& envelope) {
    write_object_ids(writer, envelope.scope_objects);
    writer.number(envelope.permitted_effects);
    writer.number(envelope.max_commands);
    writer.number(envelope.max_objects_touched);
    writer.number(envelope.max_topology_operations);
    writer.number(envelope.max_scene_queries);
    writer.number(envelope.max_objects_examined);
    writer.number(envelope.max_topology_elements_examined);
    writer.number(envelope.max_semantic_records_examined);
    writer.number(envelope.max_query_result_bytes);
    writer.number(envelope.max_repair_attempts);
    writer.number(envelope.max_additional_operations);
    writer.real(envelope.max_transform_distance_meters);
}

AgentWorkEnvelope read_envelope(Reader& reader, std::uint64_t version) {
    AgentWorkEnvelope envelope;
    envelope.scope_objects = read_object_ids(reader);
    const auto effects = reader.number();
    if (effects > std::numeric_limits<std::uint32_t>::max()) {
        reader.fail("checkpoint effect mask exceeds its integer bound");
    }
    envelope.permitted_effects = static_cast<std::uint32_t>(effects);
    envelope.max_commands = reader.count(256U);
    envelope.max_objects_touched = reader.count(512U);
    envelope.max_topology_operations = reader.count(1'000'000U);
    envelope.max_scene_queries = reader.count(1'000'000U);
    envelope.max_objects_examined = reader.count(1'000'000U);
    envelope.max_topology_elements_examined = reader.count(1'000'000U);
    envelope.max_semantic_records_examined = reader.count(1'000'000U);
    envelope.max_query_result_bytes = reader.count(64U * 1024U * 1024U);
    envelope.max_repair_attempts = reader.count(4U);
    envelope.max_additional_operations = reader.count(256U);
    if (version >= 4U) envelope.max_transform_distance_meters = reader.real();
    return envelope;
}

void write_step_receipt(Writer& writer, const StepReceipt& receipt) {
    writer.field(receipt.step_id);
    writer.field(receipt.intent);
    writer.field(receipt.tool_id);
    writer.number(receipt.target_object.value);
    writer.number(receipt.revision_before.value());
    writer.number(receipt.revision_after.value());
    writer.number(static_cast<std::uint64_t>(receipt.status));
    write_diagnostic(writer, receipt.diagnostic);
}

StepReceipt read_step_receipt(Reader& reader) {
    StepReceipt receipt;
    receipt.step_id = reader.field(64U);
    receipt.intent = reader.field(1024U);
    receipt.tool_id = reader.field(128U);
    receipt.target_object = scene::ObjectId{reader.number()};
    receipt.revision_before = core::Revision(reader.number());
    receipt.revision_after = core::Revision(reader.number());
    receipt.status = static_cast<StepStatus>(reader.number());
    receipt.diagnostic = read_diagnostic(reader);
    return receipt;
}

void write_gate(Writer& writer, const AcceptanceGateResult& gate) {
    writer.field(gate.id);
    writer.boolean(gate.required);
    writer.boolean(gate.passed);
    writer.field(gate.evidence);
    writer.field(gate.validator_version);
    writer.number(gate.source_revision.value());
    writer.number(gate.evaluated_revision.value());
    write_object_ids(writer, gate.affected_objects);
    writer.number(gate.affected_asset_ids.size());
    for (const auto asset : gate.affected_asset_ids) writer.number(asset);
    writer.digest(gate.result_digest);
    writer.digest(gate.evidence_digest);
    write_diagnostic(writer, gate.diagnostic);
}

AcceptanceGateResult read_gate(Reader& reader) {
    AcceptanceGateResult gate;
    gate.id = reader.field(128U);
    gate.required = reader.boolean();
    gate.passed = reader.boolean();
    gate.evidence = reader.field(4096U);
    gate.validator_version = reader.field(128U);
    gate.source_revision = core::Revision(reader.number());
    gate.evaluated_revision = core::Revision(reader.number());
    gate.affected_objects = read_object_ids(reader);
    const auto asset_count = reader.count(512U);
    gate.affected_asset_ids.reserve(asset_count);
    for (std::size_t index = 0U; index < asset_count; ++index) {
        gate.affected_asset_ids.push_back(reader.number());
    }
    gate.result_digest = reader.digest();
    gate.evidence_digest = reader.digest();
    gate.diagnostic = read_diagnostic(reader);
    return gate;
}

void write_repair(Writer& writer, const AgentRepairReceipt& repair) {
    writer.number(repair.attempt);
    writer.field(repair.previous_task_id);
    writer.field(repair.repaired_task_id);
    writer.field(repair.justification);
    writer.number(repair.previous_plan_steps);
    writer.number(repair.repaired_plan_steps);
    writer.number(repair.source_revision.value());
    writer.digest(repair.previous_plan_digest);
    writer.digest(repair.repaired_plan_digest);
    writer.digest(repair.previous_objective_digest);
    writer.digest(repair.repaired_objective_digest);
    writer.digest(repair.failure_evidence_digest);
    writer.digest(repair.repair_evidence_digest);
    writer.number(repair.previous_ghost_correction_count);
    writer.digest(repair.previous_ghost_correction_digest);
    writer.number(repair.previous_step_receipts.size());
    for (const auto& receipt : repair.previous_step_receipts) write_step_receipt(writer, receipt);
    writer.number(repair.previous_gate_results.size());
    for (const auto& gate : repair.previous_gate_results) write_gate(writer, gate);
}

AgentRepairReceipt read_repair(Reader& reader, std::uint64_t version) {
    AgentRepairReceipt repair;
    repair.attempt = reader.count(4U);
    repair.previous_task_id = reader.field(64U);
    repair.repaired_task_id = reader.field(64U);
    repair.justification = reader.field(1024U);
    repair.previous_plan_steps = reader.count(256U);
    repair.repaired_plan_steps = reader.count(256U);
    repair.source_revision = core::Revision(reader.number());
    repair.previous_plan_digest = reader.digest();
    repair.repaired_plan_digest = reader.digest();
    repair.previous_objective_digest = reader.digest();
    repair.repaired_objective_digest = reader.digest();
    repair.failure_evidence_digest = reader.digest();
    repair.repair_evidence_digest = reader.digest();
    if (version >= 3U) {
        repair.previous_ghost_correction_count = reader.count(256U);
        repair.previous_ghost_correction_digest = reader.digest();
    }
    const auto receipt_count = reader.count(4096U);
    repair.previous_step_receipts.reserve(receipt_count);
    for (std::size_t index = 0U; index < receipt_count; ++index) {
        repair.previous_step_receipts.push_back(read_step_receipt(reader));
    }
    const auto gate_count = reader.count(256U);
    repair.previous_gate_results.reserve(gate_count);
    for (std::size_t index = 0U; index < gate_count; ++index) {
        repair.previous_gate_results.push_back(read_gate(reader));
    }
    return repair;
}

void write_ghost_correction(Writer& writer, const GhostCorrectionReceipt& correction) {
    writer.field(correction.task_id);
    writer.number(correction.object.value);
    write_transform(writer, correction.before_transform);
    write_transform(writer, correction.after_transform);
    writer.number(correction.source_revision.value());
    writer.number(correction.branch_revision_before.value());
    writer.number(correction.branch_revision_after.value());
    writer.digest(correction.plan_context_digest);
    writer.digest(correction.parent_evidence_digest);
    writer.field(correction.justification);
    writer.digest(correction.evidence_digest);
    writer.number(static_cast<std::uint64_t>(correction.kind));
    writer.number(correction.vertex.has_value() ? correction.vertex->value : 0U);
    writer.real(correction.before_position.x);
    writer.real(correction.before_position.y);
    writer.real(correction.before_position.z);
    writer.real(correction.after_position.x);
    writer.real(correction.after_position.y);
    writer.real(correction.after_position.z);
    writer.number(correction.face.has_value() ? correction.face->value : 0U);
    writer.real(correction.distance);
    writer.digest(correction.before_mesh_digest);
    writer.digest(correction.after_mesh_digest);
}

GhostCorrectionReceipt read_ghost_correction(Reader& reader, std::uint64_t version) {
    GhostCorrectionReceipt correction;
    correction.task_id = reader.field(64U);
    correction.object = scene::ObjectId{reader.number()};
    correction.before_transform = read_transform(reader);
    correction.after_transform = read_transform(reader);
    correction.source_revision = core::Revision(reader.number());
    correction.branch_revision_before = core::Revision(reader.number());
    correction.branch_revision_after = core::Revision(reader.number());
    correction.plan_context_digest = reader.digest();
    correction.parent_evidence_digest = reader.digest();
    correction.justification = reader.field(1024U);
    correction.evidence_digest = reader.digest();
    if (version >= 6U) {
        const auto kind = reader.number();
        const auto maximum_kind = version >= 7U
            ? GhostCorrectionKind::face_extrusion
            : GhostCorrectionKind::vertex_position;
        if (kind > static_cast<std::uint64_t>(maximum_kind)) {
            reader.fail("checkpoint ghost correction kind is unknown");
        }
        correction.kind = static_cast<GhostCorrectionKind>(kind);
        const auto vertex = reader.number();
        if (vertex != 0U) correction.vertex = geometry::VertexId{vertex};
        correction.before_position = {reader.real(), reader.real(), reader.real()};
        correction.after_position = {reader.real(), reader.real(), reader.real()};
    }
    if (version >= 7U) {
        const auto face = reader.number();
        if (face != 0U) correction.face = geometry::FaceId{face};
        correction.distance = reader.real();
        correction.before_mesh_digest = reader.digest();
        correction.after_mesh_digest = reader.digest();
    }
    return correction;
}

void write_rebase(Writer& writer, const AgentRebaseReceipt& rebase) {
    writer.field(rebase.checkpoint_task_id);
    writer.digest(rebase.checkpoint_plan_context_digest);
    writer.number(rebase.previous_source_revision.value());
    writer.number(rebase.rebased_source_revision.value());
    writer.digest(rebase.checkpoint_source_digest);
    writer.digest(rebase.rebased_source_digest);
    writer.digest(rebase.read_set_digest);
    writer.digest(rebase.write_set_digest);
    write_object_ids(writer, rebase.read_objects);
    write_object_ids(writer, rebase.write_objects);
    writer.number(rebase.read_mesh_assets.size());
    for (const auto asset : rebase.read_mesh_assets) writer.number(asset);
    writer.number(rebase.write_mesh_assets.size());
    for (const auto asset : rebase.write_mesh_assets) writer.number(asset);
}

AgentRebaseReceipt read_rebase(Reader& reader, std::uint64_t version) {
    AgentRebaseReceipt rebase;
    rebase.checkpoint_task_id = reader.field(64U);
    if (version >= 3U) rebase.checkpoint_plan_context_digest = reader.digest();
    rebase.previous_source_revision = core::Revision(reader.number());
    rebase.rebased_source_revision = core::Revision(reader.number());
    rebase.checkpoint_source_digest = reader.digest();
    rebase.rebased_source_digest = reader.digest();
    rebase.read_set_digest = reader.digest();
    rebase.write_set_digest = reader.digest();
    rebase.read_objects = read_object_ids(reader);
    rebase.write_objects = read_object_ids(reader);
    const auto read_asset_count = reader.count(512U);
    rebase.read_mesh_assets.reserve(read_asset_count);
    for (std::size_t index = 0U; index < read_asset_count; ++index) {
        rebase.read_mesh_assets.push_back(reader.number());
    }
    const auto write_asset_count = reader.count(512U);
    rebase.write_mesh_assets.reserve(write_asset_count);
    for (std::size_t index = 0U; index < write_asset_count; ++index) {
        rebase.write_mesh_assets.push_back(reader.number());
    }
    return rebase;
}

void write_ghost_recompute(
    Writer& writer,
    const AgentGhostRecomputeReceipt& recompute) {
    writer.field(recompute.task_id);
    writer.field(recompute.anchor_step_id);
    writer.digest(recompute.correction_evidence_digest);
    writer.number(recompute.dependent_step_ids.size());
    for (const auto& step_id : recompute.dependent_step_ids) writer.field(step_id);
    writer.number(recompute.previous_step_receipts.size());
    for (const auto& receipt : recompute.previous_step_receipts) {
        write_step_receipt(writer, receipt);
    }
    writer.number(recompute.previous_gate_results.size());
    for (const auto& gate : recompute.previous_gate_results) write_gate(writer, gate);
    writer.digest(recompute.previous_diff_digest);
    writer.digest(recompute.result_diff_digest);
    writer.number(recompute.previous_speculative_revision.value());
    writer.number(recompute.result_speculative_revision.value());
    writer.boolean(recompute.replay_completed);
    write_diagnostic(writer, recompute.diagnostic);
    writer.digest(recompute.evidence_digest);
}

AgentGhostRecomputeReceipt read_ghost_recompute(Reader& reader) {
    AgentGhostRecomputeReceipt recompute;
    recompute.task_id = reader.field(64U);
    recompute.anchor_step_id = reader.field(64U);
    recompute.correction_evidence_digest = reader.digest();
    const auto dependent_count = reader.count(256U);
    recompute.dependent_step_ids.reserve(dependent_count);
    for (std::size_t index = 0U; index < dependent_count; ++index) {
        recompute.dependent_step_ids.push_back(reader.field(64U));
    }
    const auto receipt_count = reader.count(4096U);
    recompute.previous_step_receipts.reserve(receipt_count);
    for (std::size_t index = 0U; index < receipt_count; ++index) {
        recompute.previous_step_receipts.push_back(read_step_receipt(reader));
    }
    const auto gate_count = reader.count(256U);
    recompute.previous_gate_results.reserve(gate_count);
    for (std::size_t index = 0U; index < gate_count; ++index) {
        recompute.previous_gate_results.push_back(read_gate(reader));
    }
    recompute.previous_diff_digest = reader.digest();
    recompute.result_diff_digest = reader.digest();
    recompute.previous_speculative_revision = core::Revision(reader.number());
    recompute.result_speculative_revision = core::Revision(reader.number());
    recompute.replay_completed = reader.boolean();
    recompute.diagnostic = read_diagnostic(reader);
    recompute.evidence_digest = reader.digest();
    return recompute;
}

void write_progress(Writer& writer, const PlanStepProgress& progress) {
    writer.field(progress.step_id);
    write_strings(writer, progress.dependencies);
    writer.number(static_cast<std::uint64_t>(progress.status));
    write_diagnostic(writer, progress.diagnostic);
}

PlanStepProgress read_progress(Reader& reader) {
    PlanStepProgress progress;
    progress.step_id = reader.field(64U);
    progress.dependencies = read_strings(reader, 256U);
    progress.status = static_cast<StepStatus>(reader.number());
    progress.diagnostic = read_diagnostic(reader);
    return progress;
}

} // namespace

core::Result<void> AgentTaskCheckpoint::validate() const {
    if (static_cast<std::uint64_t>(status) >
            static_cast<std::uint64_t>(TaskStatus::rejected) ||
        status == TaskStatus::running) {
        return core::Result<void>::failure(malformed(
            "running tasks cannot be checkpointed"));
    }
    if (provider_identity.empty() || provider_identity.size() > 128U ||
        std::any_of(provider_identity.begin(), provider_identity.end(), [](unsigned char byte) {
            return byte < 0x20U || byte == 0x7fU;
        }) ||
        source_project.empty() || source_project.size() > kMaxProjectBytes ||
        speculative_project.empty() || speculative_project.size() > kMaxProjectBytes ||
        last_validation_report.size() > kMaxTextBytes || source_digest.is_zero() ||
        speculative_digest.is_zero() || diff_digest.is_zero()) {
        return core::Result<void>::failure(malformed(
            "checkpoint identity, artifact, or report exceeds its bound"));
    }
    if (auto result = plan.validate(); !result) return result;
    if (auto result = envelope.validate(); !result) return result;
    if (const auto* recipe = find_agent_recipe(plan.objective.recipe_id);
        recipe != nullptr &&
        (envelope.max_topology_operations > recipe->max_topology_operations ||
         (envelope.permitted_effects & ~recipe->permitted_effects) != 0U)) {
        return core::Result<void>::failure(malformed(
            "checkpoint work envelope exceeds its selected recipe policy"));
    }
    if (envelope.scope_objects.size() != plan.objective.scope_objects.size() ||
        !std::all_of(plan.objective.scope_objects.begin(), plan.objective.scope_objects.end(),
            [this](scene::ObjectId object) {
                return std::find(envelope.scope_objects.begin(), envelope.scope_objects.end(),
                    object) != envelope.scope_objects.end();
            })) {
        return core::Result<void>::failure(malformed(
            "checkpoint work envelope does not match the objective scope"));
    }
    if (plan.objective.base_revision != source_revision) {
        return core::Result<void>::failure(malformed(
            "checkpoint objective and source revisions differ"));
    }
    const auto context = plan.context_digest();
    if (!context || context.value() != plan_context_digest) {
        return core::Result<void>::failure(malformed(
            "checkpoint plan digest does not bind the stored plan"));
    }
    if (digest_text(source_project) != source_digest ||
        digest_text(speculative_project) != speculative_digest) {
        return core::Result<void>::failure(malformed(
            "checkpoint project digest does not bind its artifact"));
    }
    const auto source = project::ProjectDocument::deserialize(source_project);
    if (!source || source.value().revision() != source_revision) {
        return core::Result<void>::failure(malformed(
            "checkpoint source project is invalid or has a different revision"));
    }
    const auto speculative = project::ProjectDocument::deserialize(speculative_project);
    if (!speculative) {
        return core::Result<void>::failure(speculative.error().with_context(
            "checkpoint speculative project"));
    }
    if (speculative.value().revision() < source_revision) {
        return core::Result<void>::failure(malformed(
            "checkpoint speculative revision predates its source"));
    }
    if (status == TaskStatus::prepared && source_project != speculative_project) {
        return core::Result<void>::failure(malformed(
            "prepared checkpoint contains a mutated speculative artifact"));
    }
    if (plan_progress.size() != plan.steps.size() || plan_progress.size() > 256U ||
        step_receipts.size() > 4096U || repair_lineage.size() > 4U ||
        rebase_lineage.size() > 16U || ghost_corrections.size() > 256U ||
        ghost_recomputes.size() > envelope.max_ghost_recomputations ||
        repair_lineage.size() > envelope.max_repair_attempts) {
        return core::Result<void>::failure(malformed(
            "checkpoint progress or lineage count is invalid"));
    }
    std::set<std::string> progress_ids;
    for (const auto& progress : plan_progress) {
        if (!progress_ids.insert(progress.step_id).second ||
            std::none_of(plan.steps.begin(), plan.steps.end(), [&progress](const auto& step) {
                return step.id == progress.step_id && step.dependencies == progress.dependencies;
            }) || static_cast<std::uint64_t>(progress.status) >
                static_cast<std::uint64_t>(StepStatus::invalidated)) {
            return core::Result<void>::failure(malformed(
                "checkpoint progress does not match its plan"));
        }
        if (progress.status == StepStatus::running || progress.status == StepStatus::ready ||
            (status == TaskStatus::prepared && progress.status != StepStatus::pending) ||
            (status == TaskStatus::review && progress.status != StepStatus::passed)) {
            return core::Result<void>::failure(malformed(
                "checkpoint progress state is inconsistent with task status"));
        }
    }
    for (const auto& receipt : step_receipts) {
        const auto step = std::find_if(plan.steps.begin(), plan.steps.end(), [&receipt](const auto& item) {
            return item.id == receipt.step_id && item.target.object == receipt.target_object;
        });
        if (step == plan.steps.end() ||
            (receipt.status != StepStatus::passed && receipt.status != StepStatus::failed) ||
            receipt.revision_before < source_revision ||
            receipt.revision_after < receipt.revision_before ||
            receipt.revision_after > speculative.value().revision()) {
            return core::Result<void>::failure(malformed(
                "checkpoint step receipt is invalid"));
        }
    }
    for (std::size_t index = 0U; index < repair_lineage.size(); ++index) {
        const auto& repair = repair_lineage[index];
        if (repair.attempt == 0U || repair.attempt > 4U ||
            repair.attempt != index + 1U ||
            repair.previous_task_id.empty() || repair.repaired_task_id.empty() ||
            repair.justification.empty() || repair.justification.size() > 1024U ||
            std::any_of(repair.justification.begin(), repair.justification.end(),
                [](unsigned char byte) { return byte < 0x20U || byte == 0x7fU; }) ||
            repair.source_revision != source_revision) {
            return core::Result<void>::failure(malformed(
                "checkpoint repair lineage is invalid"));
        }
        if (!repair.previous_ghost_correction_digest.is_zero() &&
            (repair.previous_ghost_correction_count > ghost_corrections.size() ||
             ghost_correction_chain_digest(
                 ghost_corrections, repair.previous_ghost_correction_count) !=
                 repair.previous_ghost_correction_digest)) {
            return core::Result<void>::failure(malformed(
                "checkpoint repair does not bind its inherited ghost corrections"));
        }
        if (repair.previous_ghost_correction_digest.is_zero() &&
            repair.previous_ghost_correction_count != 0U) {
            return core::Result<void>::failure(malformed(
                "checkpoint repair ghost-correction count has no evidence digest"));
        }
    }
    for (std::size_t index = 0U; index < rebase_lineage.size(); ++index) {
        const auto& rebase = rebase_lineage[index];
        if (auto result = rebase.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "checkpoint rebase lineage"));
        }
        if ((index > 0U && rebase_lineage[index - 1U].rebased_source_revision !=
                rebase.previous_source_revision) ||
            (index == 0U && rebase.previous_source_revision >= source_revision) ||
            (index + 1U == rebase_lineage.size() &&
                rebase.rebased_source_revision != source_revision)) {
            return core::Result<void>::failure(malformed(
                "checkpoint rebase lineage does not bind its source revision chain"));
        }
    }

    std::set<std::string> known_task_ids{plan.id};
    for (const auto& repair : repair_lineage) {
        known_task_ids.insert(repair.previous_task_id);
        known_task_ids.insert(repair.repaired_task_id);
    }
    for (const auto& rebase : rebase_lineage) {
        known_task_ids.insert(rebase.checkpoint_task_id);
    }
    for (const auto& recompute : ghost_recomputes) {
        if (auto result = recompute.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "checkpoint ghost recompute lineage"));
        }
        const bool correction_is_bound = std::any_of(
            ghost_corrections.begin(), ghost_corrections.end(), [&recompute](const auto& correction) {
                return correction.evidence_digest == recompute.correction_evidence_digest;
            });
        if (!known_task_ids.contains(recompute.task_id) || !correction_is_bound) {
            return core::Result<void>::failure(malformed(
                "checkpoint ghost recompute is outside correction or task lineage"));
        }
    }
    const auto plan_context_digest_value = plan.context_digest();
    assets::Sha256Digest expected_correction_parent;
    std::map<scene::ObjectId, core::Transform> correction_final_transforms;
    std::map<std::pair<std::uint64_t, geometry::VertexId>, core::Vec3d>
        correction_final_vertices;
    std::map<std::uint64_t, assets::Sha256Digest> correction_final_mesh_digests;
    for (const auto& correction : ghost_corrections) {
        if (auto result = correction.validate(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "checkpoint ghost correction"));
        }
        const bool correction_context_matches =
            (correction.task_id == plan.id && plan_context_digest_value &&
                correction.plan_context_digest == plan_context_digest_value.value()) ||
            std::any_of(repair_lineage.begin(), repair_lineage.end(),
                [&correction](const auto& repair) {
                    return (correction.task_id == repair.previous_task_id &&
                            correction.plan_context_digest == repair.previous_plan_digest) ||
                        (correction.task_id == repair.repaired_task_id &&
                            correction.plan_context_digest == repair.repaired_plan_digest);
                }) ||
            std::any_of(rebase_lineage.begin(), rebase_lineage.end(),
                [&correction](const auto& rebase) {
                    return correction.task_id == rebase.checkpoint_task_id &&
                        correction.plan_context_digest ==
                            rebase.checkpoint_plan_context_digest;
                });
        const bool correction_source_matches = correction.source_revision == source_revision ||
            std::any_of(rebase_lineage.begin(), rebase_lineage.end(),
                [&correction](const auto& rebase) {
                    return correction.source_revision == rebase.previous_source_revision ||
                        correction.source_revision == rebase.rebased_source_revision;
                });
        if (!known_task_ids.contains(correction.task_id) ||
            !correction_source_matches ||
            !plan_context_digest_value ||
            !correction_context_matches ||
            correction.parent_evidence_digest != expected_correction_parent ||
            std::find(plan.objective.scope_objects.begin(),
                plan.objective.scope_objects.end(), correction.object) ==
                plan.objective.scope_objects.end() ||
            std::find(plan.objective.protected_objects.begin(),
                plan.objective.protected_objects.end(), correction.object) !=
                plan.objective.protected_objects.end()) {
            return core::Result<void>::failure(malformed(
                "checkpoint ghost correction is outside its plan, scope, or lineage"));
        }
        if (correction.task_id == plan.id &&
            correction.plan_context_digest == plan_context_digest_value.value() &&
            correction.recompute_after_step_id.has_value() &&
            !correction.recompute_after_step_id->empty() &&
            std::none_of(plan.steps.begin(), plan.steps.end(), [&correction](const auto& step) {
                return step.id == *correction.recompute_after_step_id;
            })) {
            return core::Result<void>::failure(malformed(
                "checkpoint ghost correction anchor is absent from its bound plan"));
        }
        if (correction.kind == GhostCorrectionKind::object_transform) {
            const auto previous = correction_final_transforms.find(correction.object);
            if (previous != correction_final_transforms.end() &&
                !same_transform(previous->second, correction.before_transform)) {
                return core::Result<void>::failure(malformed(
                    "checkpoint ghost correction transform chain is discontinuous"));
            }
            correction_final_transforms[correction.object] = correction.after_transform;
        } else if (correction.kind == GhostCorrectionKind::vertex_position &&
            correction.vertex.has_value()) {
            const auto* target = speculative.value().scene().find(correction.object);
            if (target == nullptr || target->locked || !target->mesh_asset.has_value()) {
                return core::Result<void>::failure(malformed(
                    "checkpoint ghost vertex correction target has no unlocked mesh"));
            }
            const auto mesh = speculative.value().meshes().find(*target->mesh_asset);
            if (mesh == speculative.value().meshes().end() ||
                mesh->second.find_vertex(*correction.vertex) == nullptr) {
                return core::Result<void>::failure(malformed(
                    "checkpoint ghost vertex correction target is absent from its mesh"));
            }
            for (const auto& instance : speculative.value().scene().objects_sorted()) {
                if (instance.mesh_asset == target->mesh_asset &&
                    (std::find(plan.objective.scope_objects.begin(),
                         plan.objective.scope_objects.end(), instance.id) ==
                         plan.objective.scope_objects.end() || instance.locked ||
                     std::find(plan.objective.protected_objects.begin(),
                         plan.objective.protected_objects.end(), instance.id) !=
                         plan.objective.protected_objects.end())) {
                    return core::Result<void>::failure(malformed(
                        "checkpoint ghost vertex correction escapes shared-mesh scope"));
                }
            }
            const auto key = std::make_pair(*target->mesh_asset, *correction.vertex);
            const auto previous = correction_final_vertices.find(key);
            if (previous != correction_final_vertices.end() &&
                !same_position(previous->second, correction.before_position)) {
                return core::Result<void>::failure(malformed(
                    "checkpoint ghost vertex correction chain is discontinuous"));
            }
            correction_final_vertices[key] = correction.after_position;
            if (!correction.before_mesh_digest.is_zero()) {
                const auto previous_mesh = correction_final_mesh_digests.find(
                    *target->mesh_asset);
                if (previous_mesh != correction_final_mesh_digests.end() &&
                    previous_mesh->second != correction.before_mesh_digest) {
                    return core::Result<void>::failure(malformed(
                        "checkpoint ghost mesh correction chain is discontinuous"));
                }
                correction_final_mesh_digests[*target->mesh_asset] =
                    correction.after_mesh_digest;
            }
        } else if (correction.kind == GhostCorrectionKind::face_inset ||
            correction.kind == GhostCorrectionKind::face_extrusion) {
            const auto* target = speculative.value().scene().find(correction.object);
            if (target == nullptr || target->locked ||
                !target->mesh_asset.has_value() || !correction.face.has_value() ||
                correction.before_mesh_digest.is_zero() ||
                correction.after_mesh_digest.is_zero()) {
                return core::Result<void>::failure(malformed(
                    "checkpoint face correction lacks current scoped mesh evidence"));
            }
            const auto mesh = speculative.value().meshes().find(*target->mesh_asset);
            if (mesh == speculative.value().meshes().end()) {
                return core::Result<void>::failure(malformed(
                    "checkpoint face correction target has no current mesh"));
            }
            for (const auto& instance : speculative.value().scene().objects_sorted()) {
                if (instance.mesh_asset == target->mesh_asset &&
                    (std::find(plan.objective.scope_objects.begin(),
                         plan.objective.scope_objects.end(), instance.id) ==
                         plan.objective.scope_objects.end() || instance.locked ||
                     std::find(plan.objective.protected_objects.begin(),
                         plan.objective.protected_objects.end(), instance.id) !=
                         plan.objective.protected_objects.end())) {
                    return core::Result<void>::failure(malformed(
                        "checkpoint face correction escapes shared-mesh scope"));
                }
            }
            const auto previous_mesh = correction_final_mesh_digests.find(
                *target->mesh_asset);
            if (previous_mesh != correction_final_mesh_digests.end() &&
                previous_mesh->second != correction.before_mesh_digest) {
                return core::Result<void>::failure(malformed(
                    "checkpoint ghost mesh correction chain is discontinuous"));
            }
            correction_final_mesh_digests[*target->mesh_asset] =
                correction.after_mesh_digest;
        }
        expected_correction_parent = correction.evidence_digest;
    }
    for (const auto& recompute : ghost_recomputes) {
        if (recompute.task_id != plan.id) continue;
        const auto anchor = std::find_if(plan.steps.begin(), plan.steps.end(),
            [&recompute](const auto& step) { return step.id == recompute.anchor_step_id; });
        if (anchor == plan.steps.end()) {
            return core::Result<void>::failure(malformed(
                "checkpoint ghost recompute anchor is absent from its bound plan"));
        }
        std::set<std::string> descendants{recompute.anchor_step_id};
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& step : plan.steps) {
                if (descendants.contains(step.id)) continue;
                if (std::any_of(step.dependencies.begin(), step.dependencies.end(),
                        [&descendants](const std::string& dependency) {
                            return descendants.contains(dependency);
                        })) {
                    descendants.insert(step.id);
                    changed = true;
                }
            }
        }
        const auto scheduled = plan.schedule_order();
        if (!scheduled) return core::Result<void>::failure(scheduled.error());
        std::vector<std::string> expected_dependents;
        for (const auto& step_id : scheduled.value()) {
            if (step_id != recompute.anchor_step_id && descendants.contains(step_id)) {
                expected_dependents.push_back(step_id);
            }
        }
        if (expected_dependents != recompute.dependent_step_ids) {
            return core::Result<void>::failure(malformed(
                "checkpoint ghost recompute does not match the plan dependency graph"));
        }
    }
    if (status == TaskStatus::review) {
        for (const auto& [object, expected_transform] : correction_final_transforms) {
            const auto* actual = speculative.value().scene().find(object);
            if (actual == nullptr || !same_transform(actual->local_transform, expected_transform)) {
                return core::Result<void>::failure(malformed(
                    "review checkpoint does not contain its locked ghost correction"));
            }
        }
        for (const auto& [target, expected_position] : correction_final_vertices) {
            const auto mesh = speculative.value().meshes().find(target.first);
            const auto* vertex = mesh != speculative.value().meshes().end()
                ? mesh->second.find_vertex(target.second) : nullptr;
            if (vertex == nullptr || !same_position(vertex->position, expected_position)) {
                return core::Result<void>::failure(malformed(
                    "review checkpoint does not contain its locked ghost vertex correction"));
            }
        }
        for (const auto& [asset_id, expected_digest] : correction_final_mesh_digests) {
            const auto mesh = speculative.value().meshes().find(asset_id);
            if (mesh == speculative.value().meshes().end() ||
                application::canonical_mesh_geometry_digest(mesh->second) != expected_digest) {
                return core::Result<void>::failure(malformed(
                    "review checkpoint does not contain its locked ghost mesh correction"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<std::string> AgentTaskCheckpoint::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    Writer payload;
    payload.number(kFormatVersion);
    payload.number(static_cast<std::uint64_t>(status));
    payload.field(provider_identity);
    payload.number(source_revision.value());
    payload.digest(plan_context_digest);
    payload.digest(source_digest);
    payload.digest(speculative_digest);
    payload.digest(diff_digest);
    payload.field(source_project);
    payload.field(speculative_project);
    write_plan(payload, plan);
    write_envelope(payload, envelope);
    payload.number(plan_progress.size());
    for (const auto& progress : plan_progress) write_progress(payload, progress);
    payload.number(step_receipts.size());
    for (const auto& receipt : step_receipts) write_step_receipt(payload, receipt);
    payload.number(repair_lineage.size());
    for (const auto& repair : repair_lineage) write_repair(payload, repair);
    payload.number(rebase_lineage.size());
    for (const auto& rebase : rebase_lineage) write_rebase(payload, rebase);
    payload.number(ghost_corrections.size());
    for (const auto& correction : ghost_corrections) {
        write_ghost_correction(payload, correction);
    }
    payload.field(last_validation_report);
    Writer extension;
    extension.number(1U);
    extension.number(envelope.max_ghost_recomputations);
    extension.number(ghost_corrections.size());
    for (const auto& correction : ghost_corrections) {
        extension.boolean(correction.recompute_after_step_id.has_value());
        if (correction.recompute_after_step_id.has_value()) {
            extension.field(*correction.recompute_after_step_id);
        }
    }
    extension.number(ghost_recomputes.size());
    for (const auto& recompute : ghost_recomputes) {
        write_ghost_recompute(extension, recompute);
    }
    payload.field(std::move(extension).take());
    auto body = std::move(payload).take();
    Writer outer;
    outer.field(body);
    outer.digest(digest_text(body));
    std::string result(kMagic);
    result += std::move(outer).take();
    if (result.size() > kMaxCheckpointBytes) {
        return core::Result<std::string>::failure(malformed(
            "serialized checkpoint exceeds its file size bound"));
    }
    return core::Result<std::string>::success(std::move(result));
}

core::Result<AgentTaskCheckpoint> AgentTaskCheckpoint::deserialize(std::string_view bytes) {
    if (bytes.size() > kMaxCheckpointBytes || !bytes.starts_with(kMagic)) {
        return core::Result<AgentTaskCheckpoint>::failure(malformed(
            "checkpoint header is unknown or file exceeds its size bound"));
    }
    Reader outer(bytes.substr(kMagic.size()));
    const std::string body = outer.field(kMaxCheckpointBytes);
    const auto supplied_digest = outer.digest();
    if (!outer.finished()) {
        return core::Result<AgentTaskCheckpoint>::failure(outer.error());
    }
    if (digest_text(body) != supplied_digest) {
        return core::Result<AgentTaskCheckpoint>::failure(malformed(
            "checkpoint checksum does not match its contents"));
    }

    Reader reader(body);
    const auto version = reader.number();
    if (version < 1U || version > kFormatVersion) {
        return core::Result<AgentTaskCheckpoint>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "agent task checkpoint version is unsupported"));
    }
    AgentTaskCheckpoint checkpoint;
    checkpoint.status = static_cast<TaskStatus>(reader.number());
    checkpoint.provider_identity = reader.field(128U);
    checkpoint.source_revision = core::Revision(reader.number());
    checkpoint.plan_context_digest = reader.digest();
    checkpoint.source_digest = reader.digest();
    checkpoint.speculative_digest = reader.digest();
    checkpoint.diff_digest = reader.digest();
    checkpoint.source_project = reader.field(kMaxProjectBytes);
    checkpoint.speculative_project = reader.field(kMaxProjectBytes);
    checkpoint.plan = read_plan(reader, version);
    checkpoint.envelope = read_envelope(reader, version);
    if (!reader.good()) {
        return core::Result<AgentTaskCheckpoint>::failure(malformed(
            "checkpoint plan or work envelope: " + reader.error().message));
    }
    const auto progress_count = reader.count(256U);
    checkpoint.plan_progress.reserve(progress_count);
    for (std::size_t index = 0U; index < progress_count; ++index) {
        checkpoint.plan_progress.push_back(read_progress(reader));
    }
    const auto receipt_count = reader.count(4096U);
    checkpoint.step_receipts.reserve(receipt_count);
    for (std::size_t index = 0U; index < receipt_count; ++index) {
        checkpoint.step_receipts.push_back(read_step_receipt(reader));
    }
    const auto repair_count = reader.count(4U);
    checkpoint.repair_lineage.reserve(repair_count);
    for (std::size_t index = 0U; index < repair_count; ++index) {
        checkpoint.repair_lineage.push_back(read_repair(reader, version));
    }
    if (version >= 2U) {
        const auto rebase_count = reader.count(16U);
        checkpoint.rebase_lineage.reserve(rebase_count);
        for (std::size_t index = 0U; index < rebase_count; ++index) {
            checkpoint.rebase_lineage.push_back(read_rebase(reader, version));
        }
    }
    if (version >= 3U) {
        const auto correction_count = reader.count(256U);
        checkpoint.ghost_corrections.reserve(correction_count);
        for (std::size_t index = 0U; index < correction_count; ++index) {
            checkpoint.ghost_corrections.push_back(read_ghost_correction(reader, version));
        }
    }
    if (!reader.good()) {
        return core::Result<AgentTaskCheckpoint>::failure(malformed(
            "checkpoint task evidence: " + reader.error().message));
    }
    checkpoint.last_validation_report = reader.field(kMaxTextBytes);
    if (version >= 8U) {
        const std::string extension_bytes = reader.field(kMaxCheckpointBytes);
        Reader extension(extension_bytes);
        if (extension.number() != 1U) {
            extension.fail("checkpoint workbench extension version is unsupported");
        }
        checkpoint.envelope.max_ghost_recomputations = extension.count(16U);
        const auto anchor_count = extension.count(256U);
        if (anchor_count != checkpoint.ghost_corrections.size()) {
            extension.fail("checkpoint ghost-anchor count does not match its receipts");
        }
        for (std::size_t index = 0U;
             index < anchor_count && index < checkpoint.ghost_corrections.size(); ++index) {
            if (extension.boolean()) {
                checkpoint.ghost_corrections[index].recompute_after_step_id =
                    extension.field(64U);
            }
        }
        const auto recompute_count = extension.count(16U);
        checkpoint.ghost_recomputes.reserve(recompute_count);
        for (std::size_t index = 0U; index < recompute_count; ++index) {
            checkpoint.ghost_recomputes.push_back(read_ghost_recompute(extension));
        }
        if (!extension.good()) {
            return core::Result<AgentTaskCheckpoint>::failure(malformed(
                "checkpoint workbench extension: " + extension.error().message));
        }
        if (!extension.finished()) {
            return core::Result<AgentTaskCheckpoint>::failure(extension.error());
        }
    }
    if (!reader.finished()) {
        return core::Result<AgentTaskCheckpoint>::failure(reader.error());
    }
    if (static_cast<std::uint64_t>(checkpoint.status) >
        static_cast<std::uint64_t>(TaskStatus::rejected)) {
        return core::Result<AgentTaskCheckpoint>::failure(malformed(
            "checkpoint task status is unknown"));
    }
    if (auto result = checkpoint.validate(); !result) {
        return core::Result<AgentTaskCheckpoint>::failure(result.error());
    }
    return core::Result<AgentTaskCheckpoint>::success(std::move(checkpoint));
}


// Redacted demonstration codec and capture path. This record is intentionally
// not an AgentPlan and has no loader path back into task execution.
core::Result<AgentDemonstration> AgentDemonstration::capture(
    const AgentTaskSnapshot& snapshot) {
    if (snapshot.status != TaskStatus::accepted) {
        return core::Result<AgentDemonstration>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "only a human-accepted agent task can be recorded as a demonstration"));
    }
    const std::uint64_t base_revision = snapshot.base_revision.value();
    const std::uint64_t result_revision = snapshot.speculative_revision.value();
    if (result_revision < base_revision) {
        return core::Result<AgentDemonstration>::failure(malformed(
            "accepted task revision moved behind its source revision"));
    }

    AgentDemonstration demonstration;
    demonstration.recipe_id = snapshot.recipe_id;
    demonstration.objective_digest = snapshot.objective_digest;
    demonstration.diff_digest = snapshot.diff.digest;
    demonstration.result_revision_span = result_revision - base_revision;
    demonstration.repair_count = snapshot.repairs.size();
    demonstration.rebase_count = snapshot.rebases.size();
    demonstration.ghost_correction_count = snapshot.ghost_corrections.size();

    std::vector<scene::ObjectId> object_aliases;
    object_aliases.reserve(std::min<std::size_t>(snapshot.steps.size(), 512U));
    const auto revision_offset = [base_revision, result_revision](
        core::Revision revision, std::uint64_t& offset) {
        const auto value = revision.value();
        if (value < base_revision || value > result_revision) return false;
        offset = value - base_revision;
        return true;
    };
    for (const auto& receipt : snapshot.steps) {
        if (receipt.status != StepStatus::passed) {
            return core::Result<AgentDemonstration>::failure(malformed(
                "accepted task contains a step that did not pass"));
        }
        auto alias = std::find(
            object_aliases.begin(), object_aliases.end(), receipt.target_object);
        if (alias == object_aliases.end()) {
            if (object_aliases.size() >= 512U) {
                return core::Result<AgentDemonstration>::failure(malformed(
                    "accepted task exceeds the demonstration object alias bound"));
            }
            object_aliases.push_back(receipt.target_object);
            alias = object_aliases.end() - 1;
        }
        std::uint64_t before_offset = 0U;
        std::uint64_t after_offset = 0U;
        if (!revision_offset(receipt.revision_before, before_offset) ||
            !revision_offset(receipt.revision_after, after_offset)) {
            return core::Result<AgentDemonstration>::failure(malformed(
                "accepted task step revision is outside its source and result span"));
        }
        if (!receipt.demonstration_parameters ||
            !receipt.demonstration_parameters->validate(receipt.tool_id)) {
            return core::Result<AgentDemonstration>::failure(malformed(
                "accepted task step has no safe typed demonstration parameterization"));
        }
        demonstration.steps.push_back(AgentDemonstrationStep{
            digest_text(receipt.intent),
            receipt.tool_id,
            static_cast<std::uint32_t>(
                std::distance(object_aliases.begin(), alias) + 1),
            before_offset,
            after_offset,
            receipt.status,
            receipt.demonstration_parameters});
    }
    for (const auto& gate : snapshot.gates) {
        std::uint64_t source_offset = 0U;
        std::uint64_t evaluated_offset = 0U;
        if (!revision_offset(gate.source_revision, source_offset) ||
            !revision_offset(gate.evaluated_revision, evaluated_offset)) {
            return core::Result<AgentDemonstration>::failure(malformed(
                "acceptance gate revision is outside its source and result span"));
        }
        demonstration.gates.push_back(AgentDemonstrationGate{
            gate.id,
            gate.required,
            gate.passed,
            gate.validator_version,
            source_offset,
            evaluated_offset,
            gate.affected_objects.size(),
            gate.affected_asset_ids.size(),
            gate.result_digest,
            gate.evidence_digest});
    }
    if (auto result = demonstration.validate(); !result) {
        return core::Result<AgentDemonstration>::failure(result.error());
    }
    return core::Result<AgentDemonstration>::success(std::move(demonstration));
}

core::Result<void> AgentDemonstration::validate() const {
    if (format_version < 1U || format_version > kFormatVersion ||
        (!recipe_id.empty() && !safe_demonstration_text(recipe_id, 256U)) ||
        steps.empty() || steps.size() > kMaxDemonstrationSteps ||
        gates.empty() || gates.size() > kMaxDemonstrationGates ||
        repair_count > 4U || rebase_count > 16U ||
        ghost_correction_count > 256U) {
        return core::Result<void>::failure(malformed(
            "demonstration metadata exceeds its bounds"));
    }

    std::set<std::string> gate_ids;
    bool has_required_gate = false;
    for (const auto& step : steps) {
        if (!safe_demonstration_text(step.tool_id, 256U) ||
            step.target_alias == 0U || step.target_alias > 512U ||
            step.status != StepStatus::passed ||
            step.revision_before_offset > step.revision_after_offset ||
            step.revision_after_offset > result_revision_span) {
            return core::Result<void>::failure(malformed(
                "demonstration step is invalid or exceeds its revision span"));
        }
        if (format_version >= 2U &&
            (!step.parameters || !step.parameters->validate(step.tool_id))) {
            return core::Result<void>::failure(malformed(
                "demonstration step typed parameters do not match its native tool"));
        }
        if (format_version < 3U && step.parameters.has_value() &&
            step.parameters->parameterization ==
                AgentDemonstrationParameterization::parameterless) {
            return core::Result<void>::failure(malformed(
                "legacy demonstration format cannot encode parameterless operations"));
        }
        if (format_version == 1U && step.parameters.has_value()) {
            return core::Result<void>::failure(malformed(
                "legacy demonstration format cannot contain typed parameters"));
        }
    }
    for (const auto& gate : gates) {
        if (!safe_demonstration_text(gate.id, 256U) ||
            !safe_demonstration_text(gate.validator_version, 128U) ||
            gate.source_revision_offset > gate.evaluated_revision_offset ||
            gate.evaluated_revision_offset > result_revision_span ||
            !gate_ids.insert(gate.id).second) {
            return core::Result<void>::failure(malformed(
                "demonstration gate metadata is invalid"));
        }
        if (gate.required) {
            has_required_gate = true;
            if (!gate.passed) {
                return core::Result<void>::failure(malformed(
                    "demonstration contains a failed required acceptance gate"));
            }
        }
    }
    if (!has_required_gate) {
        return core::Result<void>::failure(malformed(
            "demonstration has no required acceptance gate"));
    }
    return core::Result<void>::success();
}

core::Result<std::string> AgentDemonstration::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    Writer payload;
    payload.number(format_version);
    payload.field(recipe_id);
    payload.digest(objective_digest);
    payload.digest(diff_digest);
    payload.number(result_revision_span);
    payload.number(repair_count);
    payload.number(rebase_count);
    payload.number(ghost_correction_count);
    payload.number(steps.size());
    for (const auto& step : steps) {
        payload.digest(step.intent_digest);
        payload.field(step.tool_id);
        payload.number(step.target_alias);
        payload.number(step.revision_before_offset);
        payload.number(step.revision_after_offset);
        payload.number(static_cast<std::uint64_t>(step.status));
        if (format_version >= 2U) {
            payload.boolean(step.parameters.has_value());
            if (step.parameters.has_value()) {
                payload.number(static_cast<std::uint64_t>(
                    step.parameters->parameterization));
                if (step.parameters->parameterization ==
                    AgentDemonstrationParameterization::normalized_face_distance) {
                    payload.real(step.parameters->normalized_distance);
                } else if (step.parameters->parameterization ==
                    AgentDemonstrationParameterization::relative_displacement) {
                    payload.real(step.parameters->relative_displacement.x);
                    payload.real(step.parameters->relative_displacement.y);
                    payload.real(step.parameters->relative_displacement.z);
                }
            }
        }
    }
    payload.number(gates.size());
    for (const auto& gate : gates) {
        payload.field(gate.id);
        payload.boolean(gate.required);
        payload.boolean(gate.passed);
        payload.field(gate.validator_version);
        payload.number(gate.source_revision_offset);
        payload.number(gate.evaluated_revision_offset);
        payload.number(gate.affected_object_count);
        payload.number(gate.affected_asset_count);
        payload.digest(gate.result_digest);
        payload.digest(gate.evidence_digest);
    }
    const std::string body = std::move(payload).take();
    Writer outer;
    outer.field(body);
    outer.digest(digest_text(body));
    std::string result(format_version == 1U
        ? kDemonstrationMagicV1
        : format_version == 2U ? kDemonstrationMagicV2 : kDemonstrationMagicV3);
    result += std::move(outer).take();
    if (result.size() > kMaxDemonstrationBytes) {
        return core::Result<std::string>::failure(malformed(
            "serialized demonstration exceeds its file size bound"));
    }
    return core::Result<std::string>::success(std::move(result));
}

core::Result<AgentDemonstration> AgentDemonstration::deserialize(
    std::string_view bytes) {
    const bool version_one = bytes.starts_with(kDemonstrationMagicV1);
    const bool version_two = bytes.starts_with(kDemonstrationMagicV2);
    const bool version_three = bytes.starts_with(kDemonstrationMagicV3);
    if (bytes.size() > kMaxDemonstrationBytes ||
        (!version_one && !version_two && !version_three)) {
        return core::Result<AgentDemonstration>::failure(malformed(
            "demonstration header is unknown or file exceeds its size bound"));
    }
    const std::string_view magic = version_one
        ? kDemonstrationMagicV1
        : version_two ? kDemonstrationMagicV2 : kDemonstrationMagicV3;
    const std::uint32_t expected_version = version_one ? 1U : version_two ? 2U : 3U;
    Reader outer(bytes.substr(magic.size()));
    const std::string body = outer.field(kMaxDemonstrationBytes);
    const auto supplied_digest = outer.digest();
    if (!outer.finished()) {
        return core::Result<AgentDemonstration>::failure(outer.error());
    }
    if (digest_text(body) != supplied_digest) {
        return core::Result<AgentDemonstration>::failure(malformed(
            "demonstration checksum does not match its contents"));
    }

    Reader reader(body);
    if (reader.number() != expected_version) {
        return core::Result<AgentDemonstration>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "agent demonstration version is unsupported"));
    }
    AgentDemonstration demonstration;
    demonstration.format_version = expected_version;
    demonstration.recipe_id = reader.field(256U);
    demonstration.objective_digest = reader.digest();
    demonstration.diff_digest = reader.digest();
    demonstration.result_revision_span = reader.number();
    demonstration.repair_count = reader.count(4U);
    demonstration.rebase_count = reader.count(16U);
    demonstration.ghost_correction_count = reader.count(256U);
    const auto step_count = reader.count(kMaxDemonstrationSteps);
    demonstration.steps.reserve(step_count);
    for (std::size_t index = 0U; index < step_count; ++index) {
        AgentDemonstrationStep step;
        step.intent_digest = reader.digest();
        step.tool_id = reader.field(256U);
        const auto target_alias = reader.number();
        if (target_alias > 512U) {
            reader.fail("demonstration target alias exceeds its bound");
        }
        step.target_alias = target_alias <= 512U
            ? static_cast<std::uint32_t>(target_alias) : 0U;
        step.revision_before_offset = reader.number();
        step.revision_after_offset = reader.number();
        const auto status = reader.number();
        if (status > static_cast<std::uint64_t>(StepStatus::invalidated)) {
            reader.fail("demonstration step status is unknown");
        }
        step.status = static_cast<StepStatus>(status);
        if (expected_version >= 2U && reader.boolean()) {
            AgentDemonstrationParameters parameters;
            const auto parameterization = reader.number();
            const auto maximum_parameterization = expected_version >= 3U
                ? AgentDemonstrationParameterization::parameterless
                : AgentDemonstrationParameterization::relative_displacement;
            if (parameterization > static_cast<std::uint64_t>(maximum_parameterization)) {
                reader.fail("demonstration parameterization kind is unknown");
            }
            parameters.parameterization = static_cast<
                AgentDemonstrationParameterization>(parameterization);
            if (parameters.parameterization ==
                AgentDemonstrationParameterization::normalized_face_distance) {
                parameters.normalized_distance = reader.real();
            } else if (parameters.parameterization ==
                AgentDemonstrationParameterization::relative_displacement) {
                parameters.relative_displacement = {
                    reader.real(), reader.real(), reader.real()};
            }
            step.parameters = parameters;
        }
        demonstration.steps.push_back(std::move(step));
    }
    const auto gate_count = reader.count(kMaxDemonstrationGates);
    demonstration.gates.reserve(gate_count);
    for (std::size_t index = 0U; index < gate_count; ++index) {
        AgentDemonstrationGate gate;
        gate.id = reader.field(256U);
        gate.required = reader.boolean();
        gate.passed = reader.boolean();
        gate.validator_version = reader.field(128U);
        gate.source_revision_offset = reader.number();
        gate.evaluated_revision_offset = reader.number();
        gate.affected_object_count = reader.count(512U);
        gate.affected_asset_count = reader.count(4096U);
        gate.result_digest = reader.digest();
        gate.evidence_digest = reader.digest();
        demonstration.gates.push_back(std::move(gate));
    }
    if (!reader.finished()) {
        return core::Result<AgentDemonstration>::failure(reader.error());
    }
    if (auto result = demonstration.validate(); !result) {
        return core::Result<AgentDemonstration>::failure(result.error());
    }
    return core::Result<AgentDemonstration>::success(std::move(demonstration));
}

core::Result<AgentDemonstrationTrace> AgentDemonstrationTrace::capture(
    std::span<const application::NativeOperationTraceEntry> operations) {
    if (operations.empty() || operations.size() > kMaxDemonstrationTraceSteps) {
        return core::Result<AgentDemonstrationTrace>::failure(malformed(
            "selected native command range is empty or exceeds its step bound"));
    }
    const std::uint64_t base_revision = operations.front().revision_before.value();
    std::uint64_t previous_revision = base_revision;
    std::map<scene::ObjectId, std::uint32_t> object_aliases;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::uint32_t>
        component_aliases;
    AgentDemonstrationTrace trace;
    trace.steps.reserve(operations.size());
    for (const auto& operation : operations) {
        if (operation.source != application::OperationSource::human ||
            operation.operation_id == 0U || operation.tool_id.empty() ||
            !operation.target_object ||
            operation.revision_before.value() != previous_revision ||
            operation.revision_after.value() <= operation.revision_before.value()) {
            return core::Result<AgentDemonstrationTrace>::failure(malformed(
                "selected native command range contains a non-human or discontinuous receipt"));
        }
        auto [object_alias, inserted_object] = object_aliases.emplace(
            operation.target_object, static_cast<std::uint32_t>(object_aliases.size() + 1U));
        (void)inserted_object;
        if (object_aliases.size() > 512U) {
            return core::Result<AgentDemonstrationTrace>::failure(malformed(
                "selected native command range exceeds the object alias bound"));
        }

        AgentDemonstrationTargetKind target_kind = AgentDemonstrationTargetKind::object;
        std::uint32_t component_alias = 0U;
        if (const auto* face = std::get_if<geometry::FaceId>(&operation.target_component)) {
            if (!*face) {
                return core::Result<AgentDemonstrationTrace>::failure(malformed(
                    "selected native command range contains a zero face identity"));
            }
            target_kind = AgentDemonstrationTargetKind::face;
            const auto key = std::make_tuple(operation.target_object.value, 1U, face->value);
            auto [alias, inserted] = component_aliases.emplace(
                key, static_cast<std::uint32_t>(component_aliases.size() + 1U));
            (void)inserted;
            component_alias = alias->second;
        } else if (const auto* vertex =
                std::get_if<geometry::VertexId>(&operation.target_component)) {
            if (!*vertex) {
                return core::Result<AgentDemonstrationTrace>::failure(malformed(
                    "selected native command range contains a zero vertex identity"));
            }
            target_kind = AgentDemonstrationTargetKind::vertex;
            const auto key = std::make_tuple(operation.target_object.value, 2U, vertex->value);
            auto [alias, inserted] = component_aliases.emplace(
                key, static_cast<std::uint32_t>(component_aliases.size() + 1U));
            (void)inserted;
            component_alias = alias->second;
        }

        AgentDemonstrationParameters parameters;
        switch (operation.parameterization) {
        case application::NativeTraceParameterization::normalized_face_distance:
            parameters.parameterization =
                AgentDemonstrationParameterization::normalized_face_distance;
            parameters.normalized_distance = operation.normalized_distance;
            break;
        case application::NativeTraceParameterization::relative_displacement:
            parameters.parameterization =
                AgentDemonstrationParameterization::relative_displacement;
            parameters.relative_displacement = operation.relative_displacement;
            break;
        case application::NativeTraceParameterization::parameterless:
            parameters.parameterization =
                AgentDemonstrationParameterization::parameterless;
            break;
        default:
            return core::Result<AgentDemonstrationTrace>::failure(malformed(
                "selected native command range has an unknown parameterization"));
        }
        if (auto result = parameters.validate(operation.tool_id); !result) {
            return core::Result<AgentDemonstrationTrace>::failure(result.error());
        }
        const bool valid_face_parameterization =
            (operation.tool_id == "mesh.poke-face" &&
                parameters.parameterization ==
                    AgentDemonstrationParameterization::parameterless) ||
            ((operation.tool_id == "mesh.inset-face" ||
              operation.tool_id == "mesh.extrude-face") &&
                parameters.parameterization ==
                    AgentDemonstrationParameterization::normalized_face_distance);
        if ((target_kind == AgentDemonstrationTargetKind::face &&
                !valid_face_parameterization) ||
            (target_kind == AgentDemonstrationTargetKind::vertex &&
                parameters.parameterization !=
                    AgentDemonstrationParameterization::relative_displacement) ||
            (target_kind == AgentDemonstrationTargetKind::object &&
                (operation.tool_id != "object.translate" ||
                 parameters.parameterization !=
                    AgentDemonstrationParameterization::relative_displacement))) {
            return core::Result<AgentDemonstrationTrace>::failure(malformed(
                "selected native command target does not match its typed operation"));
        }

        trace.steps.push_back(AgentDemonstrationTraceStep{
            operation.tool_id,
            target_kind,
            object_alias->second,
            component_alias,
            operation.revision_before.value() - base_revision,
            operation.revision_after.value() - base_revision,
            parameters,
            operation.structure_signature,
            operation.result_object_structure_signature});
        previous_revision = operation.revision_after.value();
    }
    trace.result_revision_span = previous_revision - base_revision;
    if (auto result = trace.validate(); !result) {
        return core::Result<AgentDemonstrationTrace>::failure(result.error());
    }
    return core::Result<AgentDemonstrationTrace>::success(std::move(trace));
}

core::Result<void> AgentDemonstrationTrace::validate() const {
    if (steps.empty() || steps.size() > kMaxDemonstrationTraceSteps) {
        return core::Result<void>::failure(malformed(
            "demonstration trace is empty or exceeds its step bound"));
    }
    std::uint64_t previous_after = 0U;
    std::map<std::pair<std::uint32_t, std::uint32_t>, AgentDemonstrationTargetKind>
        binding_kinds;
    std::map<std::uint32_t,
        std::pair<std::uint32_t, AgentDemonstrationTargetKind>> component_aliases;
    for (std::size_t index = 0U; index < steps.size(); ++index) {
        const auto& step = steps[index];
        if (!safe_demonstration_text(step.tool_id, 256U) ||
            step.target_alias == 0U || step.target_alias > 512U ||
            step.revision_before_offset != previous_after ||
            step.revision_after_offset <= step.revision_before_offset ||
            step.revision_after_offset > result_revision_span ||
            !step.parameters.validate(step.tool_id) ||
            (step.structure_signature.has_value() &&
             step.structure_signature->is_zero()) ||
            (step.result_object_structure_signature.has_value() &&
             step.result_object_structure_signature->is_zero())) {
            return core::Result<void>::failure(malformed(
                "demonstration trace step is invalid or discontinuous"));
        }
        const auto binding_key = std::make_pair(
            step.target_alias, step.component_alias);
        const auto [binding, inserted_binding] = binding_kinds.emplace(
            binding_key, step.target_kind);
        if (!inserted_binding && binding->second != step.target_kind) {
            return core::Result<void>::failure(malformed(
                "demonstration trace reuses a target binding with a different kind"));
        }
        if (step.target_kind == AgentDemonstrationTargetKind::object) {
            if (step.tool_id != "object.translate" || step.component_alias != 0U ||
                step.parameters.parameterization !=
                    AgentDemonstrationParameterization::relative_displacement) {
                return core::Result<void>::failure(malformed(
                    "demonstration trace object target is invalid"));
            }
        } else if (step.target_kind == AgentDemonstrationTargetKind::face) {
            const bool supported_face_demo =
                ((step.tool_id == "mesh.inset-face" ||
                  step.tool_id == "mesh.extrude-face") &&
                    step.parameters.parameterization ==
                        AgentDemonstrationParameterization::normalized_face_distance) ||
                (step.tool_id == "mesh.poke-face" &&
                    step.parameters.parameterization ==
                        AgentDemonstrationParameterization::parameterless);
            if (step.component_alias == 0U || step.component_alias > 4096U ||
                !supported_face_demo) {
                return core::Result<void>::failure(malformed(
                    "demonstration trace face target is invalid"));
            }
            const auto [component, inserted_component] = component_aliases.emplace(
                step.component_alias,
                std::make_pair(step.target_alias, step.target_kind));
            if (!inserted_component && component->second !=
                    std::make_pair(step.target_alias, step.target_kind)) {
                return core::Result<void>::failure(malformed(
                    "demonstration component alias maps to conflicting targets"));
            }
        } else if (step.target_kind == AgentDemonstrationTargetKind::vertex) {
            if (step.component_alias == 0U || step.component_alias > 4096U ||
                step.tool_id != "mesh.set-vertex-position" ||
                step.parameters.parameterization !=
                    AgentDemonstrationParameterization::relative_displacement) {
                return core::Result<void>::failure(malformed(
                    "demonstration trace vertex target is invalid"));
            }
            const auto [component, inserted_component] = component_aliases.emplace(
                step.component_alias,
                std::make_pair(step.target_alias, step.target_kind));
            if (!inserted_component && component->second !=
                    std::make_pair(step.target_alias, step.target_kind)) {
                return core::Result<void>::failure(malformed(
                    "demonstration component alias maps to conflicting targets"));
            }
        } else {
            return core::Result<void>::failure(malformed(
                "demonstration trace target kind is unknown"));
        }
        previous_after = step.revision_after_offset;
    }
    if (previous_after != result_revision_span) {
        return core::Result<void>::failure(malformed(
            "demonstration trace revision span does not match its final step"));
    }
    return core::Result<void>::success();
}

core::Result<std::string> AgentDemonstrationTrace::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    Writer payload;
    payload.number(kFormatVersion);
    payload.number(result_revision_span);
    payload.number(steps.size());
    for (const auto& step : steps) {
        payload.field(step.tool_id);
        payload.number(static_cast<std::uint64_t>(step.target_kind));
        payload.number(step.target_alias);
        payload.number(step.component_alias);
        payload.number(step.revision_before_offset);
        payload.number(step.revision_after_offset);
        payload.number(static_cast<std::uint64_t>(step.parameters.parameterization));
        if (step.parameters.parameterization ==
            AgentDemonstrationParameterization::normalized_face_distance) {
            payload.real(step.parameters.normalized_distance);
        } else if (step.parameters.parameterization ==
            AgentDemonstrationParameterization::relative_displacement) {
            payload.real(step.parameters.relative_displacement.x);
            payload.real(step.parameters.relative_displacement.y);
            payload.real(step.parameters.relative_displacement.z);
        }
        payload.number(step.structure_signature.has_value() ? 1U : 0U);
        if (step.structure_signature.has_value()) {
            payload.digest(*step.structure_signature);
        }
        payload.number(step.result_object_structure_signature.has_value() ? 1U : 0U);
        if (step.result_object_structure_signature.has_value()) {
            payload.digest(*step.result_object_structure_signature);
        }
    }
    const std::string body = std::move(payload).take();
    Writer outer;
    outer.field(body);
    outer.digest(digest_text(body));
    std::string result(kDemonstrationTraceMagic);
    result += std::move(outer).take();
    if (result.size() > kMaxDemonstrationTraceBytes) {
        return core::Result<std::string>::failure(malformed(
            "serialized demonstration trace exceeds its size bound"));
    }
    return core::Result<std::string>::success(std::move(result));
}

core::Result<AgentDemonstrationTrace> AgentDemonstrationTrace::deserialize(
    std::string_view bytes) {
    if (bytes.size() > kMaxDemonstrationTraceBytes ||
        !bytes.starts_with(kDemonstrationTraceMagic)) {
        return core::Result<AgentDemonstrationTrace>::failure(malformed(
            "demonstration trace header is unknown or file exceeds its size bound"));
    }
    Reader outer(bytes.substr(kDemonstrationTraceMagic.size()));
    const std::string body = outer.field(kMaxDemonstrationTraceBytes);
    const auto supplied_digest = outer.digest();
    if (!outer.finished()) {
        return core::Result<AgentDemonstrationTrace>::failure(outer.error());
    }
    if (digest_text(body) != supplied_digest) {
        return core::Result<AgentDemonstrationTrace>::failure(malformed(
            "demonstration trace checksum does not match its contents"));
    }
    Reader reader(body);
    const std::uint64_t format_version = reader.number();
    if (format_version < 1U || format_version > kFormatVersion) {
        return core::Result<AgentDemonstrationTrace>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "agent demonstration trace version is unsupported"));
    }
    AgentDemonstrationTrace trace;
    trace.result_revision_span = reader.number();
    const auto step_count = reader.count(kMaxDemonstrationTraceSteps);
    trace.steps.reserve(step_count);
    for (std::size_t index = 0U; index < step_count; ++index) {
        AgentDemonstrationTraceStep step;
        step.tool_id = reader.field(256U);
        const auto target_kind = reader.number();
        if (target_kind > static_cast<std::uint64_t>(
                AgentDemonstrationTargetKind::vertex)) {
            reader.fail("demonstration trace target kind is unknown");
        }
        step.target_kind = static_cast<AgentDemonstrationTargetKind>(target_kind);
        const auto target_alias = reader.number();
        const auto component_alias = reader.number();
        if (target_alias > 512U || component_alias > 4096U) {
            reader.fail("demonstration trace alias exceeds its bound");
        }
        step.target_alias = static_cast<std::uint32_t>(target_alias <= 512U
            ? target_alias : 0U);
        step.component_alias = static_cast<std::uint32_t>(component_alias <= 4096U
            ? component_alias : 0U);
        step.revision_before_offset = reader.number();
        step.revision_after_offset = reader.number();
        const auto parameterization = reader.number();
        const auto maximum_parameterization = format_version >= 3U
            ? AgentDemonstrationParameterization::parameterless
            : AgentDemonstrationParameterization::relative_displacement;
        if (parameterization > static_cast<std::uint64_t>(maximum_parameterization)) {
            reader.fail("demonstration trace parameterization is unknown");
        }
        step.parameters.parameterization = static_cast<
            AgentDemonstrationParameterization>(parameterization);
        if (step.parameters.parameterization ==
            AgentDemonstrationParameterization::normalized_face_distance) {
            step.parameters.normalized_distance = reader.real();
        } else if (step.parameters.parameterization ==
            AgentDemonstrationParameterization::relative_displacement) {
            step.parameters.relative_displacement = {
                reader.real(), reader.real(), reader.real()};
        }
        if (format_version >= 2U) {
            const auto has_signature = reader.number();
            if (has_signature > 1U) {
                reader.fail("demonstration structural signature marker is invalid");
            } else if (has_signature == 1U) {
                step.structure_signature = reader.digest();
            }
            const auto has_result_signature = reader.number();
            if (has_result_signature > 1U) {
                reader.fail("demonstration result structure signature marker is invalid");
            } else if (has_result_signature == 1U) {
                step.result_object_structure_signature = reader.digest();
            }
        }
        trace.steps.push_back(std::move(step));
    }
    if (!reader.finished()) {
        return core::Result<AgentDemonstrationTrace>::failure(reader.error());
    }
    if (auto result = trace.validate(); !result) {
        return core::Result<AgentDemonstrationTrace>::failure(result.error());
    }
    return core::Result<AgentDemonstrationTrace>::success(std::move(trace));
}

core::Result<void> AgentDemonstrationRecipe::validate() const {
    if (!safe_demonstration_text(name, 128U)) {
        return core::Result<void>::failure(malformed(
            "demonstration recipe name is empty, contains controls, or exceeds its bound"));
    }
    return trace.validate();
}

core::Result<std::string> AgentDemonstrationRecipe::serialize() const {
    if (auto result = validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    const auto encoded_trace = trace.serialize();
    if (!encoded_trace) {
        return core::Result<std::string>::failure(encoded_trace.error());
    }
    Writer payload;
    payload.number(kFormatVersion);
    payload.field(name);
    payload.field(encoded_trace.value());
    const std::string body = std::move(payload).take();
    Writer outer;
    outer.field(body);
    outer.digest(digest_text(body));
    std::string result(kDemonstrationRecipeMagic);
    result += std::move(outer).take();
    if (result.size() > kMaxDemonstrationRecipeBytes) {
        return core::Result<std::string>::failure(malformed(
            "serialized demonstration recipe exceeds its size bound"));
    }
    return core::Result<std::string>::success(std::move(result));
}

core::Result<AgentDemonstrationRecipe> AgentDemonstrationRecipe::deserialize(
    std::string_view bytes) {
    if (bytes.size() > kMaxDemonstrationRecipeBytes ||
        !bytes.starts_with(kDemonstrationRecipeMagic)) {
        return core::Result<AgentDemonstrationRecipe>::failure(malformed(
            "demonstration recipe header is unknown or file exceeds its size bound"));
    }
    Reader outer(bytes.substr(kDemonstrationRecipeMagic.size()));
    const std::string body = outer.field(kMaxDemonstrationRecipeBytes);
    const auto supplied_digest = outer.digest();
    if (!outer.finished()) {
        return core::Result<AgentDemonstrationRecipe>::failure(outer.error());
    }
    if (digest_text(body) != supplied_digest) {
        return core::Result<AgentDemonstrationRecipe>::failure(malformed(
            "demonstration recipe checksum does not match its contents"));
    }
    Reader reader(body);
    if (reader.number() != kFormatVersion) {
        return core::Result<AgentDemonstrationRecipe>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "agent demonstration recipe version is unsupported"));
    }
    AgentDemonstrationRecipe recipe;
    recipe.name = reader.field(128U);
    const std::string encoded_trace = reader.field(kMaxDemonstrationTraceBytes);
    if (!reader.finished()) {
        return core::Result<AgentDemonstrationRecipe>::failure(reader.error());
    }
    auto trace = AgentDemonstrationTrace::deserialize(encoded_trace);
    if (!trace) {
        return core::Result<AgentDemonstrationRecipe>::failure(trace.error());
    }
    recipe.trace = std::move(trace.value());
    if (auto result = recipe.validate(); !result) {
        return core::Result<AgentDemonstrationRecipe>::failure(result.error());
    }
    return core::Result<AgentDemonstrationRecipe>::success(std::move(recipe));
}
} // namespace carto::agent
