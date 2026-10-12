#include <carto/assets/blob_store.hpp>
#include <carto/market/market.hpp>
#include "../libs/plugin_package/src/internal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <optional>
#include <span>
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

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> counter{0U};
        root_ = std::filesystem::temp_directory_path() /
            ("cartographer_market_tests-" +
             std::to_string(static_cast<unsigned long long>(
                 std::chrono::steady_clock::now().time_since_epoch().count())) +
             "-" + std::to_string(static_cast<unsigned long long>(counter.fetch_add(1U))));
        std::filesystem::create_directories(root_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    REQUIRE(!error);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(stream));
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    REQUIRE(static_cast<bool>(stream));
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

std::vector<std::uint8_t> zip_store(
    const std::vector<std::pair<std::string, std::string>>& files) {
    struct DirectoryEntry {
        std::string path;
        std::uint32_t offset = 0U;
        std::uint32_t bytes = 0U;
    };
    std::vector<std::uint8_t> output;
    std::vector<DirectoryEntry> directory;
    constexpr std::uint16_t utf8 = 0x0800U;
    for (const auto& [path, contents] : files) {
        const auto offset = static_cast<std::uint32_t>(output.size());
        append_u32(output, 0x04034b50U);
        append_u16(output, 20U);
        append_u16(output, utf8);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, 0U);
        append_u32(output, static_cast<std::uint32_t>(contents.size()));
        append_u32(output, static_cast<std::uint32_t>(contents.size()));
        append_u16(output, static_cast<std::uint16_t>(path.size()));
        append_u16(output, 0U);
        output.insert(output.end(), path.begin(), path.end());
        output.insert(output.end(), contents.begin(), contents.end());
        directory.push_back(DirectoryEntry{
            path, offset, static_cast<std::uint32_t>(contents.size())});
    }
    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& entry : directory) {
        append_u32(output, 0x02014b50U);
        append_u16(output, 20U);
        append_u16(output, 20U);
        append_u16(output, utf8);
        append_u16(output, 0U);
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

carto::assets::Sha256Digest digest_of(std::string_view value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
    return carto::assets::sha256(std::span<const std::uint8_t>{bytes, value.size()});
}

carto::market::MarketPackageManifest base_manifest(
    std::string package_id,
    std::string version,
    std::vector<carto::market::DependencySpec> dependencies = {}) {
    carto::market::MarketPackageManifest manifest;
    manifest.package_id = std::move(package_id);
    manifest.listing_id = manifest.package_id + "-listing";
    manifest.listing_version_id = std::move(version);
    manifest.publisher_id = "community-publisher";
    manifest.source_revision = "source-revision-1";
    manifest.license_id = "cc0-1.0";
    manifest.entrypoints.push_back({"cartographer-project", "source/box.carto"});
    manifest.targets.push_back({"vanta", "2026.10", "target/default.glb", std::nullopt});
    manifest.dependencies = std::move(dependencies);
    manifest.provenance_receipt = "receipts/publication.json";
    return manifest;
}

struct PackageFixture {
    carto::market::MarketPackageManifest manifest;
    carto::assets::Sha256Digest package_digest;
};

PackageFixture make_package(
    const std::filesystem::path& root,
    bool executable = false,
    std::string package_id = "community.box",
    std::string version = "1.0.0",
    std::vector<carto::market::DependencySpec> dependencies = {}) {
    write_file(root / "source" / "box.carto", "cartographer-source");
    write_file(root / "target" / "default.glb", "prepared-target");
    write_file(root / "receipts" / "publication.json", "publication-receipt");
    if (executable) write_file(root / "payload.dll", "not-a-real-binary");
    auto manifest = base_manifest(std::move(package_id), std::move(version), std::move(dependencies));
    const auto content_digest = carto::market::MarketPackageValidator::directory_payload_digest(root);
    REQUIRE(content_digest);
    manifest.content_digest = content_digest.value();
    write_file(root / "manifest.json", manifest.serialize());
    const auto package_digest = carto::market::MarketPackageValidator::directory_package_digest(root);
    REQUIRE(package_digest);
    return PackageFixture{std::move(manifest), package_digest.value()};
}

carto::market::DependencyResolutionPlan singleton_plan(const PackageFixture& fixture) {
    carto::market::DependencyResolutionPlan plan;
    plan.packages.push_back({
        fixture.manifest.package_id,
        fixture.manifest.listing_version_id,
        fixture.manifest.content_digest,
        fixture.package_digest});
    REQUIRE(plan.refresh_digest());
    return plan;
}

carto::market::MarketPublicationReceipt signed_publication(
    const PackageFixture& fixture) {
    const std::array<std::uint8_t, 32> seed{
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    carto::market::MarketPublicationReceipt receipt;
    receipt.package_digest = fixture.package_digest;
    receipt.package_id = fixture.manifest.package_id;
    receipt.listing_version_id = fixture.manifest.listing_version_id;
    receipt.publisher_id = fixture.manifest.publisher_id;
    receipt.validator_version = "market-validator-1";
    receipt.observed_at = "2026-10-11T12:00:00Z";
    receipt.key_id = "market-key";
    receipt.algorithm = "ed25519";
    receipt.signature = carto::plugin_package::ed25519_sign(seed, receipt.signing_bytes());
    return receipt;
}

carto::plugin_package::KeyRing publication_key_ring() {
    const std::array<std::uint8_t, 32> public_key{
        0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7,
        0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
        0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25,
        0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a};
    return carto::plugin_package::KeyRing{{{"market-key", public_key}}};
}

void directory_validation_and_identity() {
    TempDirectory temporary;
    const auto package_root = temporary.path() / "package";
    const auto fixture = make_package(package_root);
    const auto report = carto::market::MarketPackageValidator::validate_directory(package_root);
    REQUIRE(report);
    REQUIRE(report.value().computed_content_digest == fixture.manifest.content_digest);
    REQUIRE(report.value().computed_package_digest == fixture.package_digest);
    REQUIRE(report.value().payload_files == 3U);
    const auto reopened_manifest = carto::market::MarketPackageManifest::deserialize(
        fixture.manifest.serialize());
    REQUIRE(reopened_manifest);
    const auto reopened_identity = reopened_manifest.value().package_identity_digest();
    REQUIRE(reopened_identity);
    REQUIRE(reopened_identity.value() == fixture.package_digest);

    auto tampered = fixture.manifest;
    tampered.publisher_id = "different-publisher";
    write_file(package_root / "manifest.json", tampered.serialize());
    const auto changed = carto::market::MarketPackageValidator::validate_directory(package_root);
    REQUIRE(changed);
    REQUIRE(changed.value().computed_content_digest == fixture.manifest.content_digest);
    REQUIRE(changed.value().computed_package_digest != fixture.package_digest);

    carto::market::MarketLibrary library(temporary.path() / "library");
    REQUIRE(!library.install_directory(package_root, fixture.package_digest));
}

void hostile_package_inputs_fail_closed() {
    TempDirectory temporary;
    const auto executable_root = temporary.path() / "executable";
    (void)make_package(executable_root, true);
    REQUIRE(!carto::market::MarketPackageValidator::validate_directory(executable_root));

    carto::market::PackageEntrypoint empty_entrypoint{"cartographer-project", ""};
    REQUIRE(!empty_entrypoint.validate());
    carto::market::TargetArtifact empty_target{"vanta", "2026.10", "", std::nullopt};
    REQUIRE(!empty_target.validate());

    auto dot_identity = base_manifest(".", "1.0.0");
    dot_identity.content_digest = digest_of("dot");
    REQUIRE(!dot_identity.validate());
    auto drive_identity = base_manifest("C:", "1.0.0");
    drive_identity.content_digest = digest_of("drive");
    REQUIRE(!drive_identity.validate());

    const auto traversal = zip_store({{"../escape.bin", "blocked"}});
    REQUIRE(!carto::market::MarketPackageValidator::validate_archive(traversal));

    carto::market::MarketValidationLimits overflowing;
    overflowing.max_total_payload_bytes = std::numeric_limits<std::uint64_t>::max();
    overflowing.max_manifest_bytes = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(!carto::market::MarketPackageValidator::validate_archive({}, overflowing));
}

void archive_validation_is_order_independent() {
    TempDirectory temporary;
    const auto package_root = temporary.path() / "package";
    const auto fixture = make_package(package_root);
    const auto manifest_text = fixture.manifest.serialize();
    const auto archive = zip_store({
        {"target/default.glb", "prepared-target"},
        {"manifest.json", manifest_text},
        {"receipts/publication.json", "publication-receipt"},
        {"source/box.carto", "cartographer-source"},
    });
    const auto report = carto::market::MarketPackageValidator::validate_archive(archive);
    REQUIRE(report);
    REQUIRE(report.value().computed_content_digest == fixture.manifest.content_digest);
    REQUIRE(report.value().computed_package_digest == fixture.package_digest);
}

void archive_install_is_extracted_and_revalidated() {
    TempDirectory temporary;
    const auto package_root = temporary.path() / "package";
    const auto fixture = make_package(package_root);
    const auto archive = zip_store({
        {"receipts/publication.json", "publication-receipt"},
        {"source/box.carto", "cartographer-source"},
        {"manifest.json", fixture.manifest.serialize()},
        {"target/default.glb", "prepared-target"},
    });
    const auto archive_path = temporary.path() / "community.cartomarket";
    {
        std::ofstream output(archive_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(output));
        output.write(reinterpret_cast<const char*>(archive.data()),
                     static_cast<std::streamsize>(archive.size()));
        REQUIRE(static_cast<bool>(output));
    }

    carto::market::MarketLibrary library(temporary.path() / "library");
    const auto installed = library.install_archive(archive_path, fixture.package_digest);
    if (!installed) throw TestFailure(installed.error().message);
    REQUIRE(installed.value().package_digest == fixture.package_digest);
    REQUIRE(std::filesystem::is_regular_file(
        installed.value().install_root / "source" / "box.carto"));
    REQUIRE(library.inspect("community.box", "1.0.0"));
    REQUIRE(library.list());

    {
        std::ofstream tampered(archive_path, std::ios::binary | std::ios::app);
        REQUIRE(static_cast<bool>(tampered));
        tampered << "not part of the archive";
    }
    REQUIRE(!library.install_archive(archive_path, fixture.package_digest));
}

void target_artifact_digests_are_verified() {
    TempDirectory temporary;
    const auto package_root = temporary.path() / "package";
    const auto fixture = make_package(package_root);
    auto manifest = fixture.manifest;
    manifest.targets.front().digest = digest_of("prepared-target");
    write_file(package_root / "manifest.json", manifest.serialize());
    REQUIRE(carto::market::MarketPackageValidator::validate_directory(package_root));

    const auto archive = zip_store({
        {"manifest.json", manifest.serialize()},
        {"receipts/publication.json", "publication-receipt"},
        {"source/box.carto", "cartographer-source"},
        {"target/default.glb", "prepared-target"},
    });
    REQUIRE(carto::market::MarketPackageValidator::validate_archive(archive));

    manifest.targets.front().digest = digest_of("tampered-target");
    write_file(package_root / "manifest.json", manifest.serialize());
    REQUIRE(!carto::market::MarketPackageValidator::validate_directory(package_root));
    const auto tampered_archive = zip_store({
        {"manifest.json", manifest.serialize()},
        {"receipts/publication.json", "publication-receipt"},
        {"source/box.carto", "cartographer-source"},
        {"target/default.glb", "prepared-target"},
    });
    REQUIRE(!carto::market::MarketPackageValidator::validate_archive(tampered_archive));
}

void dependency_resolution_and_lockfile_are_deterministic() {
    auto optional_root = base_manifest(
        "root.package", "1.0.0", {{"missing.optional", "1.0.0", true}});
    optional_root.content_digest = digest_of("root");
    const std::vector<carto::market::MarketPackageManifest> candidates{optional_root};
    const std::vector<carto::market::DependencySpec> roots{{"root.package", "1.0.0", false}};
    const auto plan = carto::market::resolve_dependencies(candidates, roots);
    REQUIRE(plan);
    REQUIRE(plan.value().packages.size() == 1U);
    const auto root_package_digest = optional_root.package_identity_digest();
    REQUIRE(root_package_digest);

    carto::market::ProjectMarketLockfile lockfile;
    lockfile.project_id = "project-1";
    lockfile.packages.push_back({
        "root.package", "1.0.0", "root.package-listing", "1.0.0",
        root_package_digest.value(), "license-snapshot-1", {{"vanta", "2026.10", digest_of("target")}}});
    REQUIRE(lockfile.matches(plan.value()));
    const auto serialized = lockfile.serialize();
    const auto reopened = carto::market::ProjectMarketLockfile::deserialize(serialized);
    REQUIRE(reopened);
    REQUIRE(reopened.value().matches(plan.value()));
    REQUIRE(serialized.find("paypal") == std::string::npos);
    REQUIRE(serialized.find("secret") == std::string::npos);

    auto cycle_a = base_manifest("cycle.a", "1.0.0", {{"cycle.b", "1.0.0", false}});
    auto cycle_b = base_manifest("cycle.b", "1.0.0", {{"cycle.a", "1.0.0", false}});
    cycle_a.content_digest = digest_of("cycle-a");
    cycle_b.content_digest = digest_of("cycle-b");
    const std::vector<carto::market::MarketPackageManifest> cycle_candidates{cycle_a, cycle_b};
    const std::vector<carto::market::DependencySpec> cycle_roots{{"cycle.a", "1.0.0", false}};
    const auto cycle = carto::market::resolve_dependencies(cycle_candidates, cycle_roots);
    REQUIRE(!cycle);
    REQUIRE(cycle.error().code == carto::core::ErrorCode::cycle_detected);
}

void compatibility_receipts_require_fresh_authoritative_evidence() {
    const auto package_digest = digest_of("package");
    const auto evidence_digest = digest_of("evidence");
    carto::market::CompatibilityReceipt receipt{
        1U, package_digest, "vanta", "2026.10",
        carto::market::CompatibilityStatus::verified_pass,
        "validator-1", "2026-01-01T00:00:00Z", "2999-01-01T00:00:00Z",
        evidence_digest, {}};
    REQUIRE(receipt.validate());
    REQUIRE(receipt.applies_to(package_digest, "vanta", "2026.10"));
    REQUIRE(!receipt.applies_to(package_digest, "vanta", "other"));

    receipt.status = carto::market::CompatibilityStatus::verified_fail;
    REQUIRE(receipt.validate());
    REQUIRE(!receipt.applies_to(package_digest, "vanta", "2026.10"));

    const auto reopened = carto::market::CompatibilityReceipt::deserialize(receipt.serialize());
    REQUIRE(reopened);
    REQUIRE(reopened.value().status == carto::market::CompatibilityStatus::verified_fail);

    receipt.status = carto::market::CompatibilityStatus::untested;
    receipt.valid_until.reset();
    receipt.evidence_digest.reset();
    REQUIRE(receipt.validate());
    REQUIRE(!receipt.applies_to(package_digest, "vanta", "2026.10"));

    receipt.status = carto::market::CompatibilityStatus::verified_pass;
    receipt.valid_until = "2020-01-01T00:00:00Z";
    receipt.evidence_digest = evidence_digest;
    REQUIRE(!receipt.validate());
    receipt.valid_until = "2999-01-01T00:00:00";
    REQUIRE(!receipt.validate());
}

void publication_evidence_is_signed_and_install_bound() {
    TempDirectory temporary;
    const auto package_root = temporary.path() / "package";
    const auto fixture = make_package(package_root);
    const auto report = carto::market::MarketPackageValidator::validate_directory(package_root);
    REQUIRE(report);
    const auto receipt = signed_publication(fixture);
    const auto key_ring = publication_key_ring();
    REQUIRE(receipt.validate());
    const auto verified = carto::market::MarketPackageValidator::verify_publication_receipt(
        report.value(), receipt, key_ring);
    REQUIRE(verified);
    REQUIRE(verified.value() == carto::plugin_package::SignatureVerdict::verified);
    const auto reopened = carto::market::MarketPublicationReceipt::deserialize(receipt.serialize());
    REQUIRE(reopened);
    const auto receipt_digest = receipt.canonical_digest();
    REQUIRE(receipt_digest);
    REQUIRE(reopened.value().canonical_digest().value() == receipt_digest.value());
    REQUIRE(receipt.verify({}).value() == carto::plugin_package::SignatureVerdict::unknown_key);

    carto::market::MarketLibrary library(temporary.path() / "library");
    const auto installed = library.install_directory(package_root, receipt, key_ring, fixture.package_digest);
    REQUIRE(installed);
    REQUIRE(installed.value().publication_receipt_digest.has_value());
    REQUIRE(installed.value().publication_receipt_digest.value() == receipt_digest.value());
    REQUIRE(library.inspect("community.box", "1.0.0"));
    REQUIRE(library.list());

    const auto publication_path = library.root() / "receipts" / "community.box" / "1.0.0.publication.json";
    auto tampered_receipt = receipt;
    tampered_receipt.publisher_id = "other-publisher";
    write_file(publication_path, tampered_receipt.serialize());
    REQUIRE(!library.inspect("community.box", "1.0.0"));
    write_file(publication_path, receipt.serialize());
    REQUIRE(library.inspect("community.box", "1.0.0"));

    auto invalid_signature = receipt;
    invalid_signature.signature[0] ^= 0xffU;
    const auto invalid = carto::market::MarketPackageValidator::verify_publication_receipt(
        report.value(), invalid_signature, key_ring);
    REQUIRE(invalid);
    REQUIRE(invalid.value() == carto::plugin_package::SignatureVerdict::invalid);
    carto::market::MarketLibrary rejected_library(temporary.path() / "rejected-library");
    REQUIRE(!rejected_library.install_directory(
        package_root, invalid_signature, key_ring, fixture.package_digest));
}

void install_is_transactional_and_immutable() {
    TempDirectory temporary;
    const auto package_root = temporary.path() / "package";
    const auto fixture = make_package(package_root);
    const auto plan = singleton_plan(fixture);
    carto::market::MarketLibrary library(temporary.path() / "library");

    const auto dependent_root = temporary.path() / "dependent-package";
    const auto dependent_fixture = make_package(
        dependent_root, false, "community.with-dependency", "1.0.0",
        {{"community.dependency", "1.0.0", false}});
    REQUIRE(!library.install_directory(dependent_root, dependent_fixture.package_digest));

    const auto installed = library.install_directory(package_root, plan, fixture.package_digest);
    REQUIRE(installed);
    REQUIRE(installed.value().package_digest == fixture.package_digest);
    REQUIRE(installed.value().dependency_graph_digest == plan.plan_digest);
    REQUIRE(library.inspect("community.box", "1.0.0"));
    const auto listed = library.list();
    REQUIRE(listed);
    REQUIRE(listed.value().size() == 1U);
    REQUIRE(listed.value().front().package_digest == fixture.package_digest);
    REQUIRE(listed.value().front().dependency_graph_digest == plan.plan_digest);
    REQUIRE(listed.value().front().install_root == installed.value().install_root);
    carto::market::ProjectMarketLockfile lockfile;
    lockfile.project_id = "project-1";
    lockfile.packages.push_back({
        "community.box", "1.0.0", "community.box-listing", "1.0.0",
        fixture.package_digest, "license-snapshot-1", {}});
    REQUIRE(library.verify_lockfile(lockfile));
    lockfile.packages.front().package_version = "2.0.0";
    REQUIRE(!library.verify_lockfile(lockfile));
    write_file(library.root() / "packages" / "orphan.txt", "invalid-library-entry");
    REQUIRE(!library.list());
    std::error_code orphan_error;
    REQUIRE(std::filesystem::remove(library.root() / "packages" / "orphan.txt", orphan_error));
    REQUIRE(!orphan_error);
    const auto receipt_path = library.root() / "receipts" / "community.box" / "1.0.0.json";
    REQUIRE(std::filesystem::exists(receipt_path));
    REQUIRE(library.install_directory(package_root, fixture.package_digest));

    std::error_code remove_error;
    REQUIRE(std::filesystem::remove(receipt_path, remove_error));
    REQUIRE(!remove_error);
    REQUIRE(!library.inspect("community.box", "1.0.0"));
    REQUIRE(!library.list());
    REQUIRE(library.install_directory(package_root, fixture.package_digest));

    const auto wrong_digest = digest_of("wrong-install");
    const auto wrong_dependency_digest = digest_of("wrong-dependency-graph");
    carto::market::MarketInstallReceipt tampered_receipt{
        "community.box", "1.0.0", wrong_digest, wrong_dependency_digest, {}, std::nullopt};
    write_file(receipt_path, tampered_receipt.serialize());
    REQUIRE(!library.inspect("community.box", "1.0.0"));
    REQUIRE(!library.list());
    REQUIRE(!library.install_directory(package_root, fixture.package_digest));
    REQUIRE(std::filesystem::remove(receipt_path, remove_error));
    REQUIRE(!remove_error);
    REQUIRE(library.install_directory(package_root, fixture.package_digest));

    write_file(package_root / "source" / "box.carto", "changed-source");
    const auto changed_content = carto::market::MarketPackageValidator::directory_payload_digest(package_root);
    REQUIRE(changed_content);
    auto changed_manifest = fixture.manifest;
    changed_manifest.content_digest = changed_content.value();
    write_file(package_root / "manifest.json", changed_manifest.serialize());
    const auto changed_package = carto::market::MarketPackageValidator::directory_package_digest(package_root);
    REQUIRE(changed_package);
    REQUIRE(changed_package.value() != fixture.package_digest);
    REQUIRE(!library.install_directory(package_root, changed_package.value()));
    REQUIRE(library.inspect("community.box", "1.0.0"));
}

} // namespace

int main() {
    try {
        directory_validation_and_identity();
        hostile_package_inputs_fail_closed();
        archive_validation_is_order_independent();
        archive_install_is_extracted_and_revalidated();
        target_artifact_digests_are_verified();
        dependency_resolution_and_lockfile_are_deterministic();
        compatibility_receipts_require_fresh_authoritative_evidence();
        publication_evidence_is_signed_and_install_bound();
        install_is_transactional_and_immutable();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
