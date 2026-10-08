#include <carto/eval/receipt.hpp>

#include <utility>

namespace carto::eval {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

bool valid_status(EvaluationStatus status) noexcept {
    switch (status) {
    case EvaluationStatus::evaluated:
    case EvaluationStatus::cache_hit:
    case EvaluationStatus::failed:
    case EvaluationStatus::stale:
        return true;
    }
    return false;
}

} // namespace

core::Result<void> validate(const EvaluationTicket& ticket) {
    if (!ticket.node || ticket.dependency_digest.empty()) {
        return invalid("evaluation tickets require a node and dependency digest");
    }
    if (ticket.graph_revision.exhausted() || ticket.parameter_revision.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "evaluation ticket revision space is exhausted"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const EvaluationReceipt& receipt) {
    if (!receipt.node || receipt.dependency_digest.empty()) {
        return invalid("evaluation receipts require a node and dependency digest");
    }
    if (receipt.graph_revision.exhausted() || receipt.parameter_revision.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "evaluation receipt revision space is exhausted"));
    }
    if (!valid_status(receipt.status)) return invalid("evaluation receipt contains an unknown status");
    if ((receipt.status == EvaluationStatus::evaluated ||
         receipt.status == EvaluationStatus::cache_hit) && receipt.output_digest.empty()) {
        return invalid("successful evaluation receipts require an output digest");
    }
    if ((receipt.status == EvaluationStatus::failed || receipt.status == EvaluationStatus::stale) &&
        !receipt.output_digest.empty()) {
        return invalid("failed or stale evaluation receipts may not publish an output digest");
    }
    return core::Result<void>::success();
}

core::Result<void> validate_publication(
    const EvaluationTicket& ticket,
    core::Revision current_graph_revision,
    core::Revision current_parameter_revision,
    std::string_view current_dependency_digest) {
    if (auto result = validate(ticket); !result) return result;
    if (ticket.graph_revision != current_graph_revision ||
        ticket.parameter_revision != current_parameter_revision ||
        ticket.dependency_digest != current_dependency_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "evaluation result is stale and cannot be published"));
    }
    return core::Result<void>::success();
}

} // namespace carto::eval
