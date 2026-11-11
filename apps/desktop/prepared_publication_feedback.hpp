#pragma once

#include <carto/production/preparation_operation.hpp>
#include <carto/production/prepared_publication.hpp>

#include <filesystem>
#include <string>

namespace carto::desktop {

// The desktop publishes beside the saved project so a runtime handoff can find
// it without a second prompt. Appending to the native path keeps long and
// non-ASCII project paths intact.
[[nodiscard]] inline std::filesystem::path prepared_publication_root(
    const std::filesystem::path& project_path) {
    auto root = project_path;
    root += ".prepared";
    return root;
}

[[nodiscard]] inline std::string prepared_publication_receipt_feedback(
    const production::PreparationOperationReport& report) {
    if (!report.succeeded() || !report.envelope.has_value() ||
        !report.publication.has_value() || !report.resolved_scope.has_value()) {
        return "Preparation did not publish a revision.";
    }
    std::string message = "Prepared " +
        std::to_string(report.resolved_scope->asset_ids.size()) + " asset(s), " +
        std::to_string(report.resolved_scope->node_ids.size()) + " node(s), and " +
        std::to_string(report.envelope->products.size()) + " verified product(s).\n\n" +
        "Revision " + report.publication->prepared_scene_digest.hex() + "\n" +
        report.publication->revision_path.string();
    const production::SelectedPreparedScene selected{
        report.publication->prepared_scene_digest, *report.envelope,
        report.publication->revision_path};
    message += "\n\nRuntime admission pins:\n" +
        production::prepared_publication_admission_pins(selected);
    if (!report.diagnostics.empty()) {
        message += "\nCompleted with " + std::to_string(report.diagnostics.size()) +
            " warning(s).";
    }
    return message;
}

[[nodiscard]] inline std::string prepared_publication_status_feedback(
    const production::PreparedPublicationStatus& status,
    const std::filesystem::path& root, bool unsaved_changes) {
    if (!status.selected.has_value() || !status.freshness.has_value()) {
        return "No prepared publication is selected at\n" + root.string() +
            "\n\nSave the project and use File > Prepare scene to publish one.";
    }
    const auto& selected = *status.selected;
    std::string message;
    switch (*status.freshness) {
    case production::PreparedPublicationFreshness::current:
        message = "The selected prepared publication matches the saved project at revision " +
            std::to_string(selected.envelope.source_revision.value()) + ".";
        break;
    case production::PreparedPublicationFreshness::stale_source:
        message = "The selected prepared publication is stale: it was prepared from source "
            "revision " + std::to_string(selected.envelope.source_revision.value()) +
            ", which no longer matches the saved project. Prepare scene again before "
            "handing it to a runtime.";
        break;
    case production::PreparedPublicationFreshness::foreign_source:
        message = "The selected prepared publication belongs to a different source project (" +
            selected.envelope.source_namespace + "). Do not hand it off as this project.";
        break;
    }
    if (unsaved_changes) {
        message += "\n\nUnsaved edits are not part of any publication until the project is "
            "saved and prepared again.";
    }
    message += "\n\nRevision " + selected.prepared_scene_digest.hex() + "\n" +
        selected.revision_path.string() + "\n\nRuntime admission pins:\n" +
        production::prepared_publication_admission_pins(selected);
    return message;
}

} // namespace carto::desktop
