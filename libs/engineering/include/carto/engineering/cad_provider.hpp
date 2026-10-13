#pragma once

#include <carto/engineering/authoring.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace carto::engineering {

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
