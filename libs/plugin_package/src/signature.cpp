#include "internal.hpp"

#include <carto/core/json.hpp>
#include <carto/plugin_package/signature.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace carto::plugin_package {

namespace {

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool allowed_fields(
    const std::vector<core::json::Member>& object,
    std::initializer_list<std::string_view> allowed) {
    for (const auto& member : object) {
        if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) return false;
    }
    return true;
}

core::Result<std::string> required_string(
    const std::vector<core::json::Member>& object,
    std::string_view name,
    std::size_t maximum) {
    const auto* member = core::json::find_member(object, name);
    if (member == nullptr) return core::Result<std::string>::failure(validation("signature field is missing: " + std::string(name)));
    const auto value = core::json::decode_string(member->raw_value);
    if (!value || value.value().empty() || value.value().size() > maximum) {
        return core::Result<std::string>::failure(validation("signature field is invalid: " + std::string(name)));
    }
    return value;
}

core::Result<Signature> parse_signature(std::string_view json) {
    if (json.size() > 8192U) return core::Result<Signature>::failure(validation("signature file exceeds its size limit"));
    const auto object = core::json::parse_object(json);
    if (!object) return core::Result<Signature>::failure(object.error());
    if (!allowed_fields(object.value(), {"schema", "key_id", "algorithm", "signature"})) {
        return core::Result<Signature>::failure(validation("signature file contains an unknown field"));
    }
    const auto schema = required_string(object.value(), "schema", 128U);
    const auto key_id = required_string(object.value(), "key_id", 128U);
    const auto algorithm = required_string(object.value(), "algorithm", 64U);
    const auto encoded = required_string(object.value(), "signature", 128U);
    if (!schema || !key_id || !algorithm || !encoded) {
        return core::Result<Signature>::failure(!schema ? schema.error() : !key_id ? key_id.error() :
            !algorithm ? algorithm.error() : encoded.error());
    }
    Signature result{schema.value(), key_id.value(), algorithm.value(), decode_hex(encoded.value())};
    if (result.schema != "cartographer-plugin-signature.v1") {
        return core::Result<Signature>::failure(validation("signature schema is unsupported"));
    }
    if (result.signature.size() != 64U) {
        return core::Result<Signature>::failure(validation("signature must contain exactly 64 bytes"));
    }
    return core::Result<Signature>::success(std::move(result));
}

std::vector<std::uint8_t> signature_message(
    const assets::Sha256Digest& manifest_digest,
    const assets::Sha256Digest& archive_digest,
    std::string_view key_id,
    std::string_view algorithm) {
    std::vector<std::uint8_t> message;
    constexpr std::string_view domain = "cartographer-plugin-signature.v1";
    const auto append_u32 = [&message](std::uint32_t value) {
        for (unsigned index = 0U; index < 4U; ++index) {
            message.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
        }
    };
    const auto append_text = [&message, &append_u32](std::string_view value) {
        append_u32(static_cast<std::uint32_t>(value.size()));
        message.insert(message.end(), value.begin(), value.end());
    };
    append_text(domain);
    append_text(key_id);
    append_text(algorithm);
    message.insert(message.end(), manifest_digest.bytes.begin(), manifest_digest.bytes.end());
    message.insert(message.end(), archive_digest.bytes.begin(), archive_digest.bytes.end());
    return message;
}

} // namespace

core::Result<SignatureVerdict> verify_package_signature(
    const std::map<std::string, std::vector<std::uint8_t>>& entries,
    const assets::Sha256Digest& manifest_digest,
    const assets::Sha256Digest& archive_digest,
    const KeyRing& key_ring) {
    const auto found = entries.find("signature.json");
    if (found == entries.end()) return core::Result<SignatureVerdict>::success(SignatureVerdict::unsigned_package);
    const std::string_view json(reinterpret_cast<const char*>(found->second.data()), found->second.size());
    const auto parsed = parse_signature(json);
    if (!parsed) return core::Result<SignatureVerdict>::success(SignatureVerdict::invalid);
    if (parsed.value().algorithm != "ed25519") {
        return core::Result<SignatureVerdict>::success(SignatureVerdict::algorithm_unsupported);
    }
    const auto key = std::find_if(key_ring.keys.begin(), key_ring.keys.end(), [&parsed](const auto& candidate) {
        return candidate.first == parsed.value().key_id;
    });
    if (key == key_ring.keys.end()) return core::Result<SignatureVerdict>::success(SignatureVerdict::unknown_key);
    const auto message = signature_message(
        manifest_digest, archive_digest, parsed.value().key_id, parsed.value().algorithm);
    const std::span<const std::uint8_t, 64> signature(parsed.value().signature.data(), 64U);
    const std::span<const std::uint8_t, 32> public_key(key->second.data(), 32U);
    if (!ed25519_verify(public_key, message, signature)) {
        return core::Result<SignatureVerdict>::success(SignatureVerdict::invalid);
    }
    return core::Result<SignatureVerdict>::success(SignatureVerdict::verified);
}

} // namespace carto::plugin_package
