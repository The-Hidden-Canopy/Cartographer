#include <carto/biological/provider_contract.hpp>

#include <span>
#include <string_view>
#include <utility>

namespace carto::biological {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool valid_operation(std::string_view operation) {
    if (operation.empty() || operation.size() > BiologicalProviderRequest::kMaxOperationBytes) {
        return false;
    }
    for (const char character : operation) {
        const auto value = static_cast<unsigned char>(character);
        const bool ascii_alphanumeric =
            (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9');
        if (!(ascii_alphanumeric || value == '.' || value == '_' || value == '-')) {
            return false;
        }
    }
    return true;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

} // namespace

core::Result<void> BiologicalProviderRequest::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (!valid_operation(operation) || inputs_digest.is_zero() ||
        parameters_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "biological provider request operation or input digests are invalid"));
    }
    return core::Result<void>::success();
}

assets::Sha256Digest BiologicalProviderRequest::digest() const {
    const std::string canonical =
        "biological-provider-request-v1\n" +
        std::to_string(source.source_namespace.size()) + ":" + source.source_namespace + "\n" +
        std::to_string(source.asset_id) + "\n" +
        std::to_string(source.revision.value()) + "\n" + source.digest.hex() + "\n" +
        operation + "\n" + inputs_digest.hex() + "\n" + parameters_digest.hex() + "\n";
    return digest_text(canonical);
}

core::Result<void> BiologicalProviderResult::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (request_digest.is_zero() || output_digest.is_zero() || output_bytes == 0U ||
        output_bytes > kMaxOutputBytes) {
        return core::Result<void>::failure(invalid(
            "biological provider result digest or output size is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_biological_provider_result(
    const AuthoringSourceIdentity& current_source,
    const BiologicalProviderRequest& request,
    const BiologicalProviderResult& result,
    const std::uint64_t max_output_bytes) {
    if (auto status = request.validate(); !status) return status;
    if (auto status = current_source.validate(); !status) return status;
    if (auto status = result.validate(); !status) return status;
    if (max_output_bytes == 0U ||
        max_output_bytes > BiologicalProviderResult::kMaxOutputBytes) {
        return core::Result<void>::failure(invalid(
            "biological provider output budget is outside its hard bound"));
    }
    if (request.source != current_source || result.source != request.source ||
        result.request_digest != request.digest()) {
        return core::Result<void>::failure(stale(
            "biological provider result does not match the current source and exact request"));
    }
    if (result.output_bytes > max_output_bytes) {
        return core::Result<void>::failure(invalid(
            "biological provider result exceeds the caller output budget"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_biological_provider_artifact(
    const BiologicalProviderResult& result,
    const std::span<const std::uint8_t> artifact_bytes,
    const std::uint64_t max_output_bytes) {
    if (auto status = result.validate(); !status) return status;
    if (max_output_bytes == 0U ||
        max_output_bytes > BiologicalProviderResult::kMaxOutputBytes) {
        return core::Result<void>::failure(invalid(
            "biological provider artifact budget is outside its hard bound"));
    }
    if (result.output_bytes > max_output_bytes || artifact_bytes.size() > max_output_bytes) {
        return core::Result<void>::failure(invalid(
            "biological provider artifact exceeds the caller output budget"));
    }
    if (artifact_bytes.size() != result.output_bytes) {
        return core::Result<void>::failure(invalid(
            "biological provider artifact size differs from its result receipt"));
    }
    if (assets::sha256(artifact_bytes) != result.output_digest) {
        return core::Result<void>::failure(stale(
            "biological provider artifact digest differs from its result receipt"));
    }
    return core::Result<void>::success();
}

} // namespace carto::biological
