#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
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

} // namespace carto::plugin_package
