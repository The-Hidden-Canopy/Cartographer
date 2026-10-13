#include <carto/engineering/cad_provider.hpp>

#include <algorithm>

namespace carto::engineering {
namespace {

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

} // namespace

core::Result<void> ImportProbe::validate() const {
    if (provider_identity.empty() || provider_version.empty() ||
        (recognized && schema_profile.empty()) || diagnostics.size() > 256U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed, "CAD import probe is malformed"));
    }
    return core::Result<void>::success();
}

std::string_view StepAp242Provider::identity() const noexcept {
    return "cartographer.step-ap242.boundary";
}

std::string_view StepAp242Provider::version() const noexcept {
    return "1.0.0";
}

core::Result<ImportProbe> StepAp242Provider::probe(std::string_view source_bytes) const {
    ImportProbe result;
    result.provider_identity = std::string(identity());
    result.provider_version = std::string(version());
    const bool has_step_marker = source_bytes.find("ISO-10303-21") != std::string_view::npos;
    const bool has_ap242_marker = source_bytes.find("AP242") != std::string_view::npos ||
        source_bytes.find("ap242") != std::string_view::npos;
    result.recognized = has_step_marker;
    result.supported = false;
    result.schema_profile = has_ap242_marker ? "AP242" : "unknown";
    result.units.clear();
    if (!has_step_marker) {
        result.diagnostics.push_back("source does not contain a STEP exchange marker");
    } else {
        result.diagnostics.push_back(
            "STEP/AP242 source recognized, but no public CAD kernel is connected in this build");
        result.diagnostics.push_back(
            "no topology, tessellation, PMI, or units were synthesized from the probe");
    }
    if (auto validation = result.validate(); !validation) {
        return core::Result<ImportProbe>::failure(validation.error());
    }
    return core::Result<ImportProbe>::success(std::move(result));
}

core::Result<AuthoringDocument> StepAp242Provider::read_assembly(
    std::string_view source_bytes, std::string locator, std::string revision,
    assets::Sha256Digest import_settings_digest) const {
    static_cast<void>(source_bytes);
    static_cast<void>(locator);
    static_cast<void>(revision);
    static_cast<void>(import_settings_digest);
    return core::Result<AuthoringDocument>::failure(unsupported(
        "STEP/AP242 import requires an external CAD kernel/provider; probe results are not an assembly"));
}

} // namespace carto::engineering
