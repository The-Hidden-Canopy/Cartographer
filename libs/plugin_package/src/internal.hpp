#pragma once

#include <carto/assets/blob_store.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::plugin_package {

[[nodiscard]] std::array<std::uint8_t, 64> sha512(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::array<std::uint8_t, 32> ed25519_public_key_from_seed(
    std::span<const std::uint8_t, 32> seed);
[[nodiscard]] std::array<std::uint8_t, 64> ed25519_sign(
    std::span<const std::uint8_t, 32> seed,
    std::span<const std::uint8_t> message);
[[nodiscard]] bool ed25519_verify(
    std::span<const std::uint8_t, 32> pubkey,
    std::span<const std::uint8_t> message,
    std::span<const std::uint8_t, 64> sig);

[[nodiscard]] std::string canonical_manifest_json(const struct PluginPackageManifest& manifest);

[[nodiscard]] assets::Sha256Digest package_content_digest(
    const std::map<std::string, std::vector<std::uint8_t>>& entries);

[[nodiscard]] std::vector<std::uint8_t> decode_hex(std::string_view value);
[[nodiscard]] std::string encode_hex(std::span<const std::uint8_t> bytes);

} // namespace carto::plugin_package
