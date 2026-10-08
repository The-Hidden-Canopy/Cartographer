#pragma once

#include <carto/core/result.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/preparation_scope.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {

inline constexpr std::uint32_t kPreparationProfileVersion = 1U;
inline constexpr std::size_t kMaxPreparationProfileBytes = 64U * 1024U;

enum class PreparationStage : std::uint8_t {
    resolve,
    plan,
    validate,
    execute,
    verify,
    publish,
};

enum class PreparationDiagnosticSeverity : std::uint8_t {
    information,
    warning,
    error,
};

struct PreparationDiagnostic {
    std::string code;
    PreparationDiagnosticSeverity severity = PreparationDiagnosticSeverity::error;
    PreparationStage stage = PreparationStage::resolve;
    std::string source_namespace;
    std::optional<std::uint64_t> source_primary;
    std::optional<std::uint64_t> source_secondary;
    std::string capability;
    std::string message;
    std::vector<std::string> context;
};

// Engine-independent preparation policy. It describes deterministic source
// interpretation, not a destination-engine implementation or support claim.
struct PreparationProfile {
    std::string identity;
    bool use_active_render_uv = true;
    bool include_unique_lightmap_uv = true;
    bool generate_tangents_when_uv_present = true;
    std::optional<std::string> material_region_layer{
        "condition.material_region"};
    std::optional<std::string> vertex_color_layer;
    bool include_lookdev = true;
    bool require_lookdev_for_material_regions = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PreparationProfile> deserialize(
        std::string_view text);
};

[[nodiscard]] PreparationProfile portable_preparation_profile();

struct PreparationOperationRequest {
    PreparationScopeRequest scope;
    PreparationProfile profile;
    // Texture source locators are resolved beneath this directory. The path
    // is execution context only and is never serialized into prepared output.
    std::filesystem::path source_root;
    std::filesystem::path output_root;
    // Mandatory publication compare-and-swap expectation. nullopt means the
    // operation expects no currently selected prepared revision.
    std::optional<assets::Sha256Digest> expected_selected_digest;
    // Cancellation is observed at deterministic stage and item boundaries.
    // Once atomic publication begins it completes; cancellation never exposes
    // a partial revision or replaces the last valid selector.
    std::stop_token stop_token;
};

struct PreparationOperationReport {
    assets::Sha256Digest operation_id;
    PreparationStage terminal_stage = PreparationStage::resolve;
    core::Revision source_revision;
    std::optional<ResolvedPreparationScope> resolved_scope;
    std::optional<PreparedSceneEnvelope> envelope;
    std::optional<PreparedPublicationReceipt> publication;
    std::vector<PreparationDiagnostic> diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return publication.has_value() &&
            std::none_of(diagnostics.begin(), diagnostics.end(),
                [](const PreparationDiagnostic& diagnostic) {
                    return diagnostic.severity == PreparationDiagnosticSeverity::error;
                });
    }
};

// The single resolve -> plan -> validate -> execute -> verify -> publish
// operation used by desktop, CLI, and headless callers. Every source identity
// is captured before compilation. Failures return a structured report and do
// not replace the last valid selected publication.
[[nodiscard]] PreparationOperationReport prepare_and_publish(
    const project::ProjectDocument& document,
    const PreparationOperationRequest& request);

} // namespace carto::production
