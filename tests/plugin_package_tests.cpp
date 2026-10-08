#include <carto/assets/blob_store.hpp>
#include <carto/plugin_package/install.hpp>
#include <carto/plugin_package/manifest.hpp>
#include <carto/plugin_package/package.hpp>
#include <carto/plugin_package/signature.hpp>
#include "../libs/plugin_package/src/internal.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::vector<std::uint8_t> zip_stored(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files,
    std::uint16_t method = 0U) {
    std::vector<std::uint8_t> output;
    struct DirectoryEntry {
        std::string path;
        std::uint32_t offset = 0U;
        std::uint32_t bytes = 0U;
    };
    std::vector<DirectoryEntry> directory;
    for (const auto& [path, bytes] : files) {
        const auto offset = static_cast<std::uint32_t>(output.size());
        append_u32(output, 0x04034b50U);
        append_u16(output, 20U);
        append_u16(output, 0U);
        append_u16(output, method);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, 0U);
        append_u32(output, static_cast<std::uint32_t>(bytes.size()));
        append_u32(output, static_cast<std::uint32_t>(bytes.size()));
        append_u16(output, static_cast<std::uint16_t>(path.size()));
        append_u16(output, 0U);
        output.insert(output.end(), path.begin(), path.end());
        output.insert(output.end(), bytes.begin(), bytes.end());
        directory.push_back(DirectoryEntry{path, offset, static_cast<std::uint32_t>(bytes.size())});
    }
    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& entry : directory) {
        append_u32(output, 0x02014b50U);
        append_u16(output, 20U);
        append_u16(output, 20U);
        append_u16(output, 0U);
        append_u16(output, method);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, 0U);
        append_u32(output, entry.bytes);
        append_u32(output, entry.bytes);
        append_u16(output, static_cast<std::uint16_t>(entry.path.size()));
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, 0U);
        append_u32(output, entry.offset);
        output.insert(output.end(), entry.path.begin(), entry.path.end());
    }
    const auto central_bytes = static_cast<std::uint32_t>(output.size()) - central_offset;
    append_u32(output, 0x06054b50U);
    append_u16(output, 0U);
    append_u16(output, 0U);
    append_u16(output, static_cast<std::uint16_t>(directory.size()));
    append_u16(output, static_cast<std::uint16_t>(directory.size()));
    append_u32(output, central_bytes);
    append_u32(output, central_offset);
    append_u16(output, 0U);
    return output;
}

std::string hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::vector<std::uint8_t> signature_message(
    const carto::assets::Sha256Digest& manifest_digest,
    const carto::assets::Sha256Digest& archive_digest,
    std::string_view key_id,
    std::string_view algorithm) {
    std::vector<std::uint8_t> message;
    const auto append_text = [&message](std::string_view value) {
        append_u32(message, static_cast<std::uint32_t>(value.size()));
        message.insert(message.end(), value.begin(), value.end());
    };
    append_text("cartographer-plugin-signature.v1");
    append_text(key_id);
    append_text(algorithm);
    message.insert(message.end(), manifest_digest.bytes.begin(), manifest_digest.bytes.end());
    message.insert(message.end(), archive_digest.bytes.begin(), archive_digest.bytes.end());
    return message;
}

std::string manifest_json(const std::vector<std::uint8_t>& executable) {
    return "{\"schema\":\"cartographer-plugin.v1\",\"plugin_id\":\"org.example.geometry\","
        "\"display_name\":\"Example Geometry\",\"publisher\":\"Example Publisher\","
        "\"version\":\"0.1.0\",\"cartographer_api\":1,\"min_cartographer_version\":\"0.1.0\","
        "\"trust_class\":\"sandboxed_process\",\"capabilities\":[\"carto.geometry_operator\"],"
        "\"permissions\":{\"network\":false,\"project_write_new_assets\":false,\"filesystem_inputs\":[]},"
        "\"platforms\":{\"windows-x64\":{\"entrypoint\":\"cartographer/windows-x64/plugin.exe\",\"sha256\":\"" +
        carto::assets::sha256(executable).hex() + "\"}}}";
}

std::map<std::string, std::vector<std::uint8_t>> to_map(
    const std::vector<carto::plugin_package::PackageEntry>& entries) {
    std::map<std::string, std::vector<std::uint8_t>> result;
    for (const auto& entry : entries) result.emplace(entry.path, entry.bytes);
    return result;
}

void zip_reader_rejects_hostile_paths_and_compression() {
    const std::vector<std::uint8_t> truncated{0x50U, 0x4bU, 0x05U};
    REQUIRE(!carto::plugin_package::read_cartoplug(truncated));
    const std::vector<std::uint8_t> manifest{'{', '}'};
    const auto traversal = zip_stored({{"manifest.json", manifest}, {"../escape", {1U}}});
    REQUIRE(!carto::plugin_package::read_cartoplug(traversal));
    const auto absolute = zip_stored({{"manifest.json", manifest}, {"/escape", {1U}}});
    REQUIRE(!carto::plugin_package::read_cartoplug(absolute));
    const auto backslash = zip_stored({{"manifest.json", manifest}, {"folder\\escape", {1U}}});
    REQUIRE(!carto::plugin_package::read_cartoplug(backslash));
    const auto compressed = zip_stored({{"manifest.json", manifest}}, 8U);
    REQUIRE(!carto::plugin_package::read_cartoplug(compressed));
    const auto duplicate = zip_stored({{"manifest.json", manifest}, {"manifest.json", manifest}});
    REQUIRE(!carto::plugin_package::read_cartoplug(duplicate));
    auto trailing_bytes = zip_stored({{"manifest.json", manifest}});
    trailing_bytes.push_back(0xffU);
    REQUIRE(!carto::plugin_package::read_cartoplug(trailing_bytes));
}

void manifest_and_signature_identity_are_verified() {
    const std::vector<std::uint8_t> executable{'p', 'l', 'u', 'g', 'i', 'n'};
    const std::string manifest_text = manifest_json(executable);
    const std::vector<std::uint8_t> manifest_bytes(manifest_text.begin(), manifest_text.end());
    const std::vector<std::uint8_t> readme{'r', 'e', 'a', 'd', 'm', 'e'};
    const auto unsigned_archive = zip_stored({
        {"manifest.json", manifest_bytes},
        {"cartographer/windows-x64/plugin.exe", executable},
        {"docs/readme.md", readme},
    });
    const auto unsigned_entries = carto::plugin_package::read_cartoplug(unsigned_archive);
    REQUIRE(unsigned_entries);
    const auto parsed_manifest = carto::plugin_package::parse_manifest(manifest_text);
    REQUIRE(parsed_manifest);
    const std::array<std::uint8_t, 32> seed{
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    const std::array<std::uint8_t, 32> public_key{
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    const auto known_signature = carto::plugin_package::ed25519_sign(
        seed, std::span<const std::uint8_t>{});
    REQUIRE(hex(known_signature) ==
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    const carto::plugin_package::KeyRing key_ring{{{"test-key", public_key}}};
    const auto unsigned_report = carto::plugin_package::verify_package(
        unsigned_archive, unsigned_entries.value(), parsed_manifest.value(), key_ring);
    REQUIRE(unsigned_report);
    REQUIRE(unsigned_report.value().signature_verdict == carto::plugin_package::SignatureVerdict::unsigned_package);

    const auto message = signature_message(
        unsigned_report.value().manifest_digest,
        unsigned_report.value().archive_digest,
        "test-key",
        "ed25519");
    const auto signature_array = carto::plugin_package::ed25519_sign(seed, message);
    const std::vector<std::uint8_t> signature(signature_array.begin(), signature_array.end());
    const auto signature_json_for = [](std::span<const std::uint8_t> value) {
        return "{\"schema\":\"cartographer-plugin-signature.v1\",\"key_id\":\"test-key\","
            "\"algorithm\":\"ed25519\",\"signature\":\"" + hex(value) + "\"}";
    };
    const std::string signature_json = signature_json_for(signature);
    const std::vector<std::uint8_t> signature_bytes(signature_json.begin(), signature_json.end());
    const auto signed_archive = zip_stored({
        {"manifest.json", manifest_bytes},
        {"cartographer/windows-x64/plugin.exe", executable},
        {"docs/readme.md", readme},
        {"signature.json", signature_bytes},
    });
    const auto signed_entries = carto::plugin_package::read_cartoplug(signed_archive);
    REQUIRE(signed_entries);
    const auto signed_report = carto::plugin_package::verify_package(
        signed_archive, signed_entries.value(), parsed_manifest.value(), key_ring);
    REQUIRE(signed_report);
    REQUIRE(signed_report.value().signature_verdict == carto::plugin_package::SignatureVerdict::verified);

    auto noncanonical_signature = signature;
    const std::array<std::uint8_t, 32> scalar_modulus{
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    std::uint16_t carry = 0U;
    for (std::size_t index = 0U; index < scalar_modulus.size(); ++index) {
        const auto sum = static_cast<std::uint16_t>(signature[32U + index]) +
            static_cast<std::uint16_t>(scalar_modulus[index]) + carry;
        noncanonical_signature[32U + index] = static_cast<std::uint8_t>(sum);
        carry = static_cast<std::uint16_t>(sum >> 8U);
    }
    REQUIRE(carry == 0U);
    const auto noncanonical_json = signature_json_for(noncanonical_signature);
    const std::vector<std::uint8_t> noncanonical_bytes(noncanonical_json.begin(), noncanonical_json.end());
    const auto noncanonical_archive = zip_stored({
        {"manifest.json", manifest_bytes},
        {"cartographer/windows-x64/plugin.exe", executable},
        {"docs/readme.md", readme},
        {"signature.json", noncanonical_bytes},
    });
    const auto noncanonical_entries = carto::plugin_package::read_cartoplug(noncanonical_archive);
    REQUIRE(noncanonical_entries);
    const auto noncanonical_report = carto::plugin_package::verify_package(
        noncanonical_archive, noncanonical_entries.value(), parsed_manifest.value(), key_ring);
    REQUIRE(noncanonical_report);
    REQUIRE(noncanonical_report.value().signature_verdict == carto::plugin_package::SignatureVerdict::invalid);

    auto tampered_entries = signed_entries.value();
    tampered_entries[1].bytes[0] ^= 0xffU;
    const auto tampered_report = carto::plugin_package::verify_package(
        signed_archive, tampered_entries, parsed_manifest.value(), key_ring);
    REQUIRE(!tampered_report);

    auto unknown_keys = carto::plugin_package::KeyRing{};
    const auto unknown = carto::plugin_package::verify_package_signature(
        to_map(signed_entries.value()), signed_report.value().manifest_digest,
        signed_report.value().archive_digest, unknown_keys);
    REQUIRE(unknown);
    REQUIRE(unknown.value() == carto::plugin_package::SignatureVerdict::unknown_key);

    const auto install_base = std::filesystem::temp_directory_path() /
        ("cartographer-plugin-signed-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto install_package = install_base / "signed.cartoplug";
    std::error_code install_error;
    std::filesystem::create_directories(install_base, install_error);
    REQUIRE(!install_error);
    {
        std::ofstream stream(install_package, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(signed_archive.data()),
            static_cast<std::streamsize>(signed_archive.size()));
        REQUIRE(static_cast<bool>(stream));
    }
    carto::plugin_package::PluginInstallStore signed_store(install_base / "installed");
    carto::plugin_package::InstallOptions signed_options;
    signed_options.key_ring = key_ring;
    const auto signed_install = signed_store.install(install_package, signed_options);
    REQUIRE(signed_install);
    REQUIRE(signed_install.value().signature_verdict == carto::plugin_package::SignatureVerdict::verified);
    REQUIRE(signed_store.list());
    std::filesystem::remove_all(install_base, install_error);
}

void local_install_requires_developer_mode_for_unsigned_packages() {
    const std::vector<std::uint8_t> executable{'p', 'l', 'u', 'g', 'i', 'n'};
    const std::string manifest_text = manifest_json(executable);
    const std::vector<std::uint8_t> manifest_bytes(manifest_text.begin(), manifest_text.end());
    const auto archive = zip_stored({
        {"manifest.json", manifest_bytes},
        {"cartographer/windows-x64/plugin.exe", executable},
    });
    const auto base = std::filesystem::temp_directory_path() /
        ("cartographer-plugin-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto package_path = base / "example.cartoplug";
    std::error_code error;
    std::filesystem::create_directories(base, error);
    REQUIRE(!error);
    {
        std::ofstream stream(package_path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
        REQUIRE(static_cast<bool>(stream));
    }
    carto::plugin_package::PluginInstallStore store(base / "installed");
    REQUIRE(!store.install(package_path, {}).has_value());
    carto::plugin_package::InstallOptions developer;
    developer.developer_mode = true;
    const auto installed = store.install(package_path, developer);
    REQUIRE(installed);
    REQUIRE(installed.value().signature_verdict == carto::plugin_package::SignatureVerdict::unsigned_package);
    const auto listed = store.list();
    REQUIRE(listed);
    REQUIRE(listed.value().size() == 1U);
    REQUIRE(store.find("org.example.geometry"));
    const auto receipt_path = base / "installed" / "org.example.geometry" / "0.1.0" / "install_receipt.json";
    std::string original_receipt;
    {
        std::ifstream stream(receipt_path, std::ios::binary);
        original_receipt.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        REQUIRE(static_cast<bool>(stream) || stream.eof());
    }
    {
        std::string tampered_receipt = original_receipt;
        const auto publisher = tampered_receipt.find("\"publisher\":\"Example Publisher\"");
        REQUIRE(publisher != std::string::npos);
        tampered_receipt.replace(publisher, std::string("\"publisher\":\"Example Publisher\"").size(),
            "\"publisher\":\"Tampered Publisher\"");
        std::ofstream stream(receipt_path, std::ios::binary | std::ios::trunc);
        stream.write(tampered_receipt.data(), static_cast<std::streamsize>(tampered_receipt.size()));
        REQUIRE(static_cast<bool>(stream));
    }
    REQUIRE(!store.list());
    {
        std::ofstream stream(receipt_path, std::ios::binary | std::ios::trunc);
        stream.write(original_receipt.data(), static_cast<std::streamsize>(original_receipt.size()));
        REQUIRE(static_cast<bool>(stream));
    }
    REQUIRE(store.list());
    {
        std::ofstream stream(base / "installed" / "org.example.geometry" / "0.1.0" / "package.cartoplug",
            std::ios::binary | std::ios::trunc);
        const std::array<std::uint8_t, 1> tamper{0xffU};
        stream.write(reinterpret_cast<const char*>(tamper.data()), static_cast<std::streamsize>(tamper.size()));
        REQUIRE(static_cast<bool>(stream));
    }
    REQUIRE(!store.list());
    REQUIRE(store.remove("org.example.geometry", "0.1.0"));
    REQUIRE(store.list().value().empty());
    std::filesystem::remove_all(base, error);
}

void invalid_manifest_authority_is_rejected() {
    const std::string network =
        "{\"schema\":\"cartographer-plugin.v1\",\"plugin_id\":\"org.example.geometry\","
        "\"display_name\":\"Example\",\"publisher\":\"Example\",\"version\":\"0.1.0\","
        "\"cartographer_api\":1,\"min_cartographer_version\":\"0.1.0\",\"trust_class\":\"sandboxed_process\","
        "\"capabilities\":[\"carto.geometry_operator\"],\"permissions\":{\"network\":true,"
        "\"project_write_new_assets\":false,\"filesystem_inputs\":[]},\"platforms\":{}}";
    REQUIRE(!carto::plugin_package::parse_manifest(network));
    std::string unknown = network;
    const auto network_field = unknown.find("\"network\":true");
    REQUIRE(network_field != std::string::npos);
    unknown.replace(network_field, std::string("\"network\":true").size(), "\"network\":false,\"unexpected\":1");
    REQUIRE(!carto::plugin_package::parse_manifest(unknown));
}

} // namespace

int main() {
    try {
        zip_reader_rejects_hostile_paths_and_compression();
        manifest_and_signature_identity_are_verified();
        local_install_requires_developer_mode_for_unsigned_packages();
        invalid_manifest_authority_is_rejected();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
