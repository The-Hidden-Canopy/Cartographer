#include <carto/motion/performance.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace carto::motion {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

} // namespace

core::Result<void> PerformanceTake::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "performance take requires a stable identity"));
    }
    if (!safe_text(take_name, kMaxNameBytes)) {
        return core::Result<void>::failure(invalid(
            "performance take name is empty or exceeds its byte bound"));
    }
    if (ingest_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "performance take requires the digest of its accepted ingest"));
    }
    if (tags.size() > kMaxTags) {
        return core::Result<void>::failure(validation(
            "performance take tag collection exceeds its bounded capacity"));
    }
    for (const std::string& tag : tags) {
        if (!safe_text(tag, kMaxTagBytes)) {
            return core::Result<void>::failure(invalid(
                "performance take tag is empty or exceeds its byte bound"));
        }
    }
    for (std::size_t index = 1U; index < tags.size(); ++index) {
        if (!(tags[index - 1U] < tags[index])) {
            return core::Result<void>::failure(validation(
                "performance take tags must be sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> PerformanceTakeCatalog::insert(PerformanceTake take) {
    std::sort(take.tags.begin(), take.tags.end());
    if (auto result = take.validate(); !result) return result;
    const auto position = std::lower_bound(
        takes.begin(), takes.end(), take.id.value,
        [](const PerformanceTake& existing, std::uint64_t wanted) {
            return existing.id.value < wanted;
        });
    if (position != takes.end() && position->id.value == take.id.value) {
        return core::Result<void>::failure(validation(
            "performance take identities are unique; revisions re-version, never reissue"));
    }
    if (takes.size() >= kMaxTakes) {
        return core::Result<void>::failure(validation(
            "performance take catalog exceeds its bounded capacity"));
    }
    takes.insert(position, std::move(take));
    return core::Result<void>::success();
}

const PerformanceTake* PerformanceTakeCatalog::find(
    PerformanceTakeId id) const noexcept {
    const auto position = std::lower_bound(
        takes.begin(), takes.end(), id.value,
        [](const PerformanceTake& existing, std::uint64_t wanted) {
            return existing.id.value < wanted;
        });
    if (position == takes.end() || position->id.value != id.value) return nullptr;
    return &*position;
}

core::Result<void> PerformanceTakeCatalog::validate() const {
    if (takes.size() > kMaxTakes) {
        return core::Result<void>::failure(validation(
            "performance take catalog exceeds its bounded capacity"));
    }
    for (const PerformanceTake& take : takes) {
        if (auto result = take.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < takes.size(); ++index) {
        if (!(takes[index - 1U].id.value < takes[index].id.value)) {
            return core::Result<void>::failure(validation(
                "performance takes must be sorted by identity and unique"));
        }
    }
    return core::Result<void>::success();
}

std::string PerformanceTakeCatalog::serialize() const {
    std::ostringstream output;
    output << "CARTO_PERFORMANCE_CATALOG 1\n";
    output << "TAKES " << takes.size() << '\n';
    for (const PerformanceTake& take : takes) {
        output << "TAKE " << take.id.value << " \"" << take.take_name << "\" "
               << take.revision.value() << ' ' << take.ingest_digest.hex() << ' ';
        if (take.retarget_receipt_digest.has_value()) {
            output << take.retarget_receipt_digest->hex();
        } else {
            output << "none";
        }
        output << ' ' << take.tags.size();
        for (const std::string& tag : take.tags) {
            output << " \"" << tag << "\"";
        }
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PerformanceTakeCatalog::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<PerformanceTakeCatalog> PerformanceTakeCatalog::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PerformanceTakeCatalog>::failure(parse_error(
            "performance catalog text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_PERFORMANCE_CATALOG" || version != kSchemaVersion) {
        return core::Result<PerformanceTakeCatalog>::failure(parse_error(
            "performance catalog framing or schema version is invalid"));
    }
    PerformanceTakeCatalog catalog;
    std::string token;
    std::uint64_t take_count = 0U;
    if (!(input >> token) || token != "TAKES" || !(input >> take_count) ||
        take_count > kMaxTakes) {
        return core::Result<PerformanceTakeCatalog>::failure(parse_error(
            "performance catalog take count is malformed or exceeds its bound"));
    }
    catalog.takes.reserve(static_cast<std::size_t>(take_count));
    for (std::uint64_t index = 0U; index < take_count; ++index) {
        PerformanceTake take;
        std::uint64_t revision = 0U;
        std::uint64_t tag_count = 0U;
        if (!(input >> token) || token != "TAKE" || !(input >> take.id.value) ||
            !(input >> std::quoted(take.take_name)) || !(input >> revision) ||
            !(input >> token)) {
            return core::Result<PerformanceTakeCatalog>::failure(parse_error(
                "performance catalog take header is malformed"));
        }
        const auto ingest = assets::Sha256Digest::from_hex(token);
        if (!ingest) {
            return core::Result<PerformanceTakeCatalog>::failure(parse_error(
                "performance catalog ingest digest is malformed"));
        }
        take.ingest_digest = ingest.value();
        if (!(input >> token)) {
            return core::Result<PerformanceTakeCatalog>::failure(parse_error(
                "performance catalog retarget receipt field is malformed"));
        }
        take.revision = core::Revision(revision);
        if (token != "none") {
            const auto receipt = assets::Sha256Digest::from_hex(token);
            if (!receipt) {
                return core::Result<PerformanceTakeCatalog>::failure(parse_error(
                    "performance catalog retarget receipt digest is malformed"));
            }
            take.retarget_receipt_digest = receipt.value();
        }
        if (!(input >> tag_count) || tag_count > PerformanceTake::kMaxTags) {
            return core::Result<PerformanceTakeCatalog>::failure(parse_error(
                "performance catalog tag count is malformed"));
        }
        take.tags.reserve(static_cast<std::size_t>(tag_count));
        for (std::uint64_t tag = 0U; tag < tag_count; ++tag) {
            std::string value;
            if (!(input >> std::quoted(value))) {
                return core::Result<PerformanceTakeCatalog>::failure(parse_error(
                    "performance catalog tag payload is malformed"));
            }
            take.tags.push_back(std::move(value));
        }
        catalog.takes.push_back(std::move(take));
    }
    if (!(input >> token) || token != "END") {
        return core::Result<PerformanceTakeCatalog>::failure(parse_error(
            "performance catalog terminator is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PerformanceTakeCatalog>::failure(parse_error(
            "performance catalog text contains trailing unbound data"));
    }
    if (auto result = catalog.validate(); !result) {
        return core::Result<PerformanceTakeCatalog>::failure(result.error());
    }
    return core::Result<PerformanceTakeCatalog>::success(std::move(catalog));
}

} // namespace carto::motion
