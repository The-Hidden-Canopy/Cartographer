#include <carto/verification/failure_injection.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace carto::verification {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

core::Result<std::filesystem::path> resolve_revision_file(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& revision_digest,
    const std::string& relative_path,
    std::string_view label) {
    if (output_root.empty() || output_root.filename().empty()) {
        return core::Result<std::filesystem::path>::failure(invalid(
            std::string(label) + " requires a publication root directory"));
    }
    if (revision_digest.is_zero()) {
        return core::Result<std::filesystem::path>::failure(invalid(
            std::string(label) + " requires a non-zero revision digest"));
    }
    const std::filesystem::path relative(relative_path);
    if (relative_path.empty() || relative.is_absolute() ||
        relative.has_root_name() || relative.has_root_directory()) {
        return core::Result<std::filesystem::path>::failure(invalid(
            std::string(label) + " relative path is not portable"));
    }
    for (const auto& component : relative) {
        if (component.empty() || component == "." || component == "..") {
            return core::Result<std::filesystem::path>::failure(invalid(
                std::string(label) + " relative path cannot traverse"));
        }
    }
    const auto full = output_root / "revisions" / revision_digest.hex() / relative;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(full, error);
    if (error &&
        error.default_error_condition() !=
            std::make_error_condition(std::errc::no_such_file_or_directory)) {
        return core::Result<std::filesystem::path>::failure(io_error(
            std::string(label) + " cannot be inspected"));
    }
    if (!error && std::filesystem::is_symlink(status)) {
        return core::Result<std::filesystem::path>::failure(validation(
            std::string(label) + " refuses symbolic-link indirection"));
    }
    if (error || !std::filesystem::is_regular_file(status)) {
        return core::Result<std::filesystem::path>::failure(io_error(
            std::string(label) + " is missing or not a regular file"));
    }
    return core::Result<std::filesystem::path>::success(full);
}

core::Result<std::vector<std::uint8_t>> read_file_bytes(
    const std::filesystem::path& path,
    std::string_view label) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > std::numeric_limits<std::size_t>::max()) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            std::string(label) + " size cannot be determined"));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            std::string(label) + " cannot be opened"));
    }
    std::vector<std::uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() != size) {
        return core::Result<std::vector<std::uint8_t>>::failure(io_error(
            std::string(label) + " cannot be read completely"));
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

core::Result<void> write_file_bytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    std::string_view label) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Result<void>::failure(io_error(
            std::string(label) + " cannot be opened for corruption"));
    }
    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    output.close();
    if (!output) {
        return core::Result<void>::failure(io_error(
            std::string(label) + " cannot be rewritten completely"));
    }
    return core::Result<void>::success();
}

bool is_hex_digit(char value) {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
        (byte >= 'A' && byte <= 'F');
}

} // namespace

core::Result<project::TransactionReceipt> inject_concurrent_edit(
    project::ProjectDocument& document,
    journal::Journal& journal,
    std::string actor) {
    auto transaction = project::ProjectTransaction::begin(
        document, journal, std::move(actor));
    if (!transaction) {
        return core::Result<project::TransactionReceipt>::failure(
            transaction.error());
    }
    const auto object = transaction.value().create_object(
        "failure-injection.concurrent-edit");
    if (!object) {
        const auto rollback = transaction.value().rollback();
        static_cast<void>(rollback);
        return core::Result<project::TransactionReceipt>::failure(object.error());
    }
    auto receipt = transaction.value().commit("failure-injection.concurrent-edit");
    if (!receipt) {
        return core::Result<project::TransactionReceipt>::failure(receipt.error());
    }
    return core::Result<project::TransactionReceipt>::success(receipt.value());
}

core::Result<void> prepublish_source_check(
    const project::ProjectDocument& document,
    const production::PreparedSceneEnvelope& prepared) {
    if (auto result = prepared.validate(); !result) {
        return core::Result<void>::failure(
            result.error().with_context("prepublish prepared envelope"));
    }
    if (prepared.source_revision != document.revision()) {
        return core::Result<void>::failure(stale(
            "prepared source revision changed after planning: prepared r" +
            std::to_string(prepared.source_revision.value()) + " document r" +
            std::to_string(document.revision().value())));
    }
    const auto current_digest = digest_text(document.serialize());
    if (prepared.source_digest != current_digest) {
        return core::Result<void>::failure(stale(
            "prepared source digest no longer matches the durable document"));
    }
    if (!document.source_namespace().empty() &&
        document.source_namespace() != prepared.source_namespace) {
        return core::Result<void>::failure(stale(
            "prepared source namespace does not match the durable document"));
    }
    return core::Result<void>::success();
}

core::Result<void> force_stale_render_mesh(production::RenderMeshArtifact& artifact) {
    if (artifact.source_revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "cannot force a revision-zero render mesh stale"));
    }
    artifact.source_revision = core::Revision{artifact.source_revision.value() - 1U};
    return core::Result<void>::success();
}

core::Result<void> force_stale_lookdev_package(
    production::LookdevDrawPackage& package) {
    if (package.mesh_source_revision.value() == 0U) {
        return core::Result<void>::failure(invalid(
            "cannot force a revision-zero lookdev package stale"));
    }
    package.mesh_source_revision =
        core::Revision{package.mesh_source_revision.value() - 1U};
    return package.refresh_digest();
}

core::Result<void> flip_payload_byte(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset) {
    if (offset >= bytes.size()) {
        return core::Result<void>::failure(invalid(
            "payload flip offset is out of range"));
    }
    bytes[offset] = static_cast<std::uint8_t>(bytes[offset] ^ 0xffU);
    return core::Result<void>::success();
}

core::Result<void> zero_payload_range(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::size_t count) {
    if (count == 0U || offset >= bytes.size() || count > bytes.size() - offset) {
        return core::Result<void>::failure(invalid(
            "payload zero range is out of bounds"));
    }
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, 0U);
    return core::Result<void>::success();
}

core::Result<void> truncate_payload(
    std::vector<std::uint8_t>& bytes,
    std::size_t size) {
    if (size >= bytes.size()) {
        return core::Result<void>::failure(invalid(
            "payload truncation requires a strictly smaller size"));
    }
    bytes.resize(size);
    return core::Result<void>::success();
}

core::Result<void> corrupt_published_product(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& revision_digest,
    const std::string& relative_path,
    PublishedCorruptionKind kind,
    std::uint64_t offset,
    std::uint64_t length) {
    const auto path = resolve_revision_file(
        output_root, revision_digest, relative_path, "published product");
    if (!path) {
        return core::Result<void>::failure(path.error());
    }
    auto bytes = read_file_bytes(path.value(), "published product");
    if (!bytes) {
        return core::Result<void>::failure(bytes.error());
    }
    const auto size = bytes.value().size();
    const auto bounds = [&](std::uint64_t position, std::uint64_t span) {
        return position <= size && span <= size - position;
    };
    switch (kind) {
    case PublishedCorruptionKind::flip_byte: {
        if (!bounds(offset, 1U) || size == 0U) {
            return core::Result<void>::failure(invalid(
                "published product flip offset is out of range"));
        }
        bytes.value()[static_cast<std::size_t>(offset)] = static_cast<std::uint8_t>(
            bytes.value()[static_cast<std::size_t>(offset)] ^ 0xffU);
        break;
    }
    case PublishedCorruptionKind::zero_range: {
        if (length == 0U || !bounds(offset, length)) {
            return core::Result<void>::failure(invalid(
                "published product zero range is out of bounds"));
        }
        std::fill_n(
            bytes.value().begin() + static_cast<std::ptrdiff_t>(offset),
            static_cast<std::ptrdiff_t>(length), 0U);
        break;
    }
    case PublishedCorruptionKind::truncate_file: {
        if (!bounds(offset, 0U) || offset >= size) {
            return core::Result<void>::failure(invalid(
                "published product truncation size is out of range"));
        }
        bytes.value().resize(static_cast<std::size_t>(offset));
        break;
    }
    case PublishedCorruptionKind::append_garbage: {
        constexpr std::uint8_t kGarbage[] = {0xc0U, 0x1dU, 0xbaU, 0xd0U};
        bytes.value().insert(bytes.value().end(), std::begin(kGarbage), std::end(kGarbage));
        break;
    }
    }
    return write_file_bytes(path.value(), bytes.value(), "published product");
}

core::Result<void> tamper_published_manifest(
    const std::filesystem::path& output_root,
    const assets::Sha256Digest& revision_digest,
    ManifestTamperKind kind) {
    const auto path = resolve_revision_file(
        output_root, revision_digest, "manifest.carto-prepared", "prepared manifest");
    if (!path) {
        return core::Result<void>::failure(path.error());
    }
    auto bytes = read_file_bytes(path.value(), "prepared manifest");
    if (!bytes) {
        return core::Result<void>::failure(bytes.error());
    }
    switch (kind) {
    case ManifestTamperKind::flip_byte: {
        if (bytes.value().empty()) {
            return core::Result<void>::failure(validation(
                "prepared manifest is empty and cannot be flipped"));
        }
        bytes.value().front() = static_cast<std::uint8_t>(bytes.value().front() ^ 0xffU);
        break;
    }
    case ManifestTamperKind::append_garbage: {
        constexpr std::uint8_t kGarbage[] = {0xdeU, 0xadU, 0xbeU, 0xefU};
        bytes.value().insert(bytes.value().end(), std::begin(kGarbage), std::end(kGarbage));
        break;
    }
    case ManifestTamperKind::replace_digest_text: {
        // Rewrite one hex digit of the first digest token so the manifest may
        // still parse but can no longer match its selected identity.
        auto& text = bytes.value();
        std::size_t run_start = text.size();
        std::size_t run_length = 0U;
        for (std::size_t index = 0U; index < text.size(); ++index) {
            if (!is_hex_digit(static_cast<char>(text[index]))) continue;
            std::size_t end = index;
            while (end < text.size() && is_hex_digit(static_cast<char>(text[end]))) {
                ++end;
            }
            if (end - index >= 64U) {
                run_start = index;
                run_length = end - index;
                break;
            }
            index = end;
        }
        if (run_length == 0U) {
            return core::Result<void>::failure(validation(
                "prepared manifest contains no digest token to replace"));
        }
        auto& digit = text[run_start + run_length - 1U];
        digit = digit == '0' ? '1' : '0';
        break;
    }
    }
    return write_file_bytes(path.value(), bytes.value(), "prepared manifest");
}

} // namespace carto::verification
