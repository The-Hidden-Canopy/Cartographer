#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/eval/ids.hpp>

#include <string>
#include <string_view>

namespace carto::eval {

enum class EvaluationStatus {
    evaluated,
    cache_hit,
    failed,
    stale,
};

struct EvaluationTicket {
    NodeId node;
    core::Revision graph_revision;
    core::Revision parameter_revision;
    std::string dependency_digest;
};

struct EvaluationReceipt {
    NodeId node;
    core::Revision graph_revision;
    core::Revision parameter_revision;
    std::string dependency_digest;
    std::string output_digest;
    EvaluationStatus status = EvaluationStatus::failed;
};

[[nodiscard]] core::Result<void> validate(const EvaluationTicket& ticket);
[[nodiscard]] core::Result<void> validate(const EvaluationReceipt& receipt);
[[nodiscard]] core::Result<void> validate_publication(
    const EvaluationTicket& ticket,
    core::Revision current_graph_revision,
    core::Revision current_parameter_revision,
    std::string_view current_dependency_digest);

} // namespace carto::eval
