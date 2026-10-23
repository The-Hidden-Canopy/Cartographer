#pragma once

#include <carto/engineering/authoring.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace carto::engineering {

inline constexpr std::size_t kMaxStepHeaderProbeBytes = 64U * 1024U;

struct ImportProbe {
    bool recognized = false;
    bool supported = false;
    std::string provider_identity;
    std::string provider_version;
    std::string schema_profile;
    std::string units;
    std::vector<std::string> diagnostics;
    [[nodiscard]] core::Result<void> validate() const;
};

// Provider-neutral records extracted by an external CAD kernel. Admission
// attaches the actual source-byte digest and rejects unresolved provenance;
// it never invents parts, occurrences, or topology from a header probe.
struct ExtractedAssemblyInput {
    std::string project_namespace;
    Id source_id = 0U;
    std::string_view source_bytes;
    std::string locator;
    std::string revision;
    std::string provider_identity;
    std::string provider_version;
    assets::Sha256Digest import_settings_digest;
    std::string import_run_id;
    std::string units;
    bool lossy = false;
    std::vector<std::string> diagnostics;
    std::vector<EngineeringPart> parts;
    std::vector<EngineeringOccurrence> occurrences;
    std::vector<TopologyBinding> topology;
};

[[nodiscard]] core::Result<AuthoringDocument> admit_extracted_assembly(
    ExtractedAssemblyInput input);

struct CadProviderContract {
    virtual ~CadProviderContract() = default;
    [[nodiscard]] virtual std::string_view identity() const noexcept = 0;
    [[nodiscard]] virtual std::string_view version() const noexcept = 0;
    [[nodiscard]] virtual core::Result<ImportProbe> probe(
        std::string_view source_bytes) const = 0;
    [[nodiscard]] virtual core::Result<AuthoringDocument> read_assembly(
        std::string_view source_bytes, std::string locator, std::string revision,
        assets::Sha256Digest import_settings_digest) const = 0;
};

// Public builds expose the provider boundary and fail closed when no external
// CAD kernel is connected. A text probe is never presented as a B-Rep import.
class StepAp242Provider final : public CadProviderContract {
public:
    [[nodiscard]] std::string_view identity() const noexcept override;
    [[nodiscard]] std::string_view version() const noexcept override;
    [[nodiscard]] core::Result<ImportProbe> probe(
        std::string_view source_bytes) const override;
    [[nodiscard]] core::Result<AuthoringDocument> read_assembly(
        std::string_view source_bytes, std::string locator, std::string revision,
        assets::Sha256Digest import_settings_digest) const override;
};

} // namespace carto::engineering
