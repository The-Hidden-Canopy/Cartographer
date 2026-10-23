#pragma once

#include <carto/biological/authoring.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace carto::biological {

// A provider request is inert data. It names the exact authored source and
// content digests supplied by the caller; it carries no mutation authority.
struct BiologicalProviderRequest {
    static constexpr std::size_t kMaxOperationBytes = 128U;

    AuthoringSourceIdentity source;
    std::string operation;
    assets::Sha256Digest inputs_digest;
    assets::Sha256Digest parameters_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

// Provider output remains an external, content-addressed artifact until a
// separate human-reviewed authoring command admits it.
struct BiologicalProviderResult {
    static constexpr std::uint64_t kMaxOutputBytes = 256ULL * 1024ULL * 1024ULL;

    AuthoringSourceIdentity source;
    assets::Sha256Digest request_digest;
    assets::Sha256Digest output_digest;
    std::uint64_t output_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Confirms that the request is still current, the result answers that exact
// request and source, and the artifact respects a caller-declared byte budget.
// This function is read-only and does not attach the result to the document.
[[nodiscard]] core::Result<void> validate_biological_provider_result(
    const AuthoringSourceIdentity& current_source,
    const BiologicalProviderRequest& request,
    const BiologicalProviderResult& result,
    std::uint64_t max_output_bytes);

// Verifies the fetched bytes against the result's declared size and digest.
// This still does not parse, attach, or publish the artifact.
[[nodiscard]] core::Result<void> validate_biological_provider_artifact(
    const BiologicalProviderResult& result,
    std::span<const std::uint8_t> artifact_bytes,
    std::uint64_t max_output_bytes);

} // namespace carto::biological
