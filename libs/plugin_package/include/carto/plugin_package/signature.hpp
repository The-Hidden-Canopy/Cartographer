#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::plugin_package {

enum class SignatureVerdict {
    verified,
    unsigned_package,
    unknown_key,
    invalid,
    algorithm_unsupported,
};

struct KeyRing {
    std::vector<std::pair<std::string, std::array<std::uint8_t, 32>>> keys;
};

struct Signature {
    std::string schema;
    std::string key_id;
    std::string algorithm;
    std::vector<std::uint8_t> signature;
};

[[nodiscard]] core::Result<SignatureVerdict> verify_package_signature(
    const std::map<std::string, std::vector<std::uint8_t>>& entries,
    const assets::Sha256Digest& manifest_digest,
    const assets::Sha256Digest& archive_digest,
    const KeyRing& key_ring);

// Verifies a caller-defined, already domain-separated detached signature.
// The key ring contains public verification keys only; private signing keys
// stay in the publisher or service boundary.
[[nodiscard]] core::Result<SignatureVerdict> verify_detached_signature(
    std::string_view key_id,
    std::string_view algorithm,
    std::span<const std::uint8_t> message,
    std::span<const std::uint8_t, 64> signature,
    const KeyRing& key_ring);

} // namespace carto::plugin_package
