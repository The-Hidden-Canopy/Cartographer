#include <carto/agent/workbench.hpp>

#include <carto/project/project.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

namespace carto::agent {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_AGENT_TASK_CHECKPOINT_V1\n";
constexpr std::size_t kMaxCheckpointBytes = 512U * 1024U * 1024U;
constexpr std::size_t kMaxProjectBytes = 240U * 1024U * 1024U;
constexpr std::size_t kMaxTextBytes = 1U * 1024U * 1024U;

core::Diagnostic malformed(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
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

void write_plan(Writer& writer, const AgentPlan& plan) {
    writer.field(plan.id);
    writer.field(plan.objective.id);
    writer.field(plan.objective.goal);
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
        writer.boolean(std::holds_alternative<geometry::FaceId>(step.target.component));
        if (const auto* face = std::get_if<geometry::FaceId>(&step.target.component)) {
            writer.number(face->value);
        } else {
            writer.number(std::get<geometry::VertexId>(step.target.component).value);
        }
        writer.field(step.intent);
        write_strings(writer, step.dependencies);
    }
}

AgentPlan read_plan(Reader& reader) {
    AgentPlan plan;
    plan.id = reader.field(64U);
    plan.objective.id = reader.field(64U);
    plan.objective.goal = reader.field(1024U);
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
        const bool is_face = reader.boolean();
        if (is_face) step.target.component = geometry::FaceId{reader.number()};
        else step.target.component = geometry::VertexId{reader.number()};
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
}

AgentWorkEnvelope read_envelope(Reader& reader) {
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
    writer.number(repair.previous_step_receipts.size());
    for (const auto& receipt : repair.previous_step_receipts) write_step_receipt(writer, receipt);
    writer.number(repair.previous_gate_results.size());
    for (const auto& gate : repair.previous_gate_results) write_gate(writer, gate);
}

AgentRepairReceipt read_repair(Reader& reader) {
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

void write_rebase(Writer& writer, const AgentRebaseReceipt& rebase) {
    writer.field(rebase.checkpoint_task_id);
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

AgentRebaseReceipt read_rebase(Reader& reader) {
    AgentRebaseReceipt rebase;
    rebase.checkpoint_task_id = reader.field(64U);
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
        rebase_lineage.size() > 16U ||
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
    payload.field(last_validation_report);
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
    if (version != 1U && version != kFormatVersion) {
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
    checkpoint.plan = read_plan(reader);
    checkpoint.envelope = read_envelope(reader);
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
        checkpoint.repair_lineage.push_back(read_repair(reader));
    }
    if (version >= 2U) {
        const auto rebase_count = reader.count(16U);
        checkpoint.rebase_lineage.reserve(rebase_count);
        for (std::size_t index = 0U; index < rebase_count; ++index) {
            checkpoint.rebase_lineage.push_back(read_rebase(reader));
        }
    }
    checkpoint.last_validation_report = reader.field(kMaxTextBytes);
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

} // namespace carto::agent
