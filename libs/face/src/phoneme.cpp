#include <carto/face/phoneme.hpp>

#include "text_format.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::face {
namespace {

constexpr std::string_view kInventoryMagic = "CARTO_FACE_PHONEME_INVENTORY";
constexpr std::string_view kMappingMagic = "CARTO_FACE_VISEME_MAPPING";

using detail::invalid;
using detail::parse_error;
using detail::safe_token;
using detail::validation;
using detail::version_error;

core::Result<void> validate_phoneme_token(std::string_view phoneme) {
    if (!safe_token(phoneme, PhonemeInventory::kMaxPhonemeBytes)) {
        return core::Result<void>::failure(invalid(
            "phoneme token is not a bounded safe token"));
    }
    return core::Result<void>::success();
}

core::Result<std::uint64_t> read_count(std::istringstream& input, const char* label) {
    const auto token = detail::read_token(input, label);
    if (!token) return core::Result<std::uint64_t>::failure(token.error());
    std::uint64_t count = 0U;
    const auto parsed = std::from_chars(
        token.value().data(), token.value().data() + token.value().size(), count);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != token.value().data() + token.value().size()) {
        return core::Result<std::uint64_t>::failure(parse_error(
            std::string(label) + " count is invalid"));
    }
    return core::Result<std::uint64_t>::success(count);
}

void write_digest(std::ostringstream& output, const assets::Sha256Digest& digest) {
    detail::write_json_string(output, digest.hex());
}

core::Result<assets::Sha256Digest> read_digest(
    std::istringstream& input,
    const char* label) {
    const auto hex = detail::read_quoted(input, label, 64U);
    if (!hex) return core::Result<assets::Sha256Digest>::failure(hex.error());
    const auto parsed = assets::Sha256Digest::from_hex(hex.value());
    if (!parsed) {
        return core::Result<assets::Sha256Digest>::failure(parse_error(
            std::string(label) + " digest is malformed"));
    }
    return parsed;
}

core::Result<void> validate_mapping_entry(const VisemeMappingEntry& entry) {
    if (auto result = validate_phoneme_token(entry.phoneme); !result) return result;
    if (!safe_token(entry.viseme, VisemeMapping::kMaxVisemeBytes)) {
        return core::Result<void>::failure(invalid(
            "viseme name is not a bounded safe token"));
    }
    if (!std::isfinite(entry.weight) || entry.weight <= 0.0 ||
        entry.weight > kMaxVisemeWeight) {
        return core::Result<void>::failure(invalid(
            "viseme mapping weight must be finite and within (0, kMaxVisemeWeight]"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> PhonemeInventory::insert(std::string phoneme) {
    if (auto result = validate_phoneme_token(phoneme); !result) return result;
    if (phonemes.size() >= kMaxPhonemes) {
        return core::Result<void>::failure(invalid(
            "phoneme inventory exceeds its phoneme bound"));
    }
    const auto position = std::lower_bound(phonemes.begin(), phonemes.end(), phoneme);
    if (position != phonemes.end() && *position == phoneme) {
        return core::Result<void>::failure(invalid(
            "phoneme inventory already contains this phoneme"));
    }
    phonemes.insert(position, std::move(phoneme));
    return core::Result<void>::success();
}

bool PhonemeInventory::contains(std::string_view phoneme) const noexcept {
    return std::binary_search(phonemes.begin(), phonemes.end(), phoneme);
}

core::Result<void> PhonemeInventory::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(version_error(
            "phoneme inventory schema version is not supported"));
    }
    if (!safe_token(locale, kMaxLocaleBytes)) {
        return core::Result<void>::failure(invalid(
            "phoneme inventory locale is not a bounded safe token"));
    }
    if (phonemes.size() > kMaxPhonemes) {
        return core::Result<void>::failure(validation(
            "phoneme inventory exceeds its phoneme bound"));
    }
    for (const std::string& phoneme : phonemes) {
        if (auto result = validate_phoneme_token(phoneme); !result) return result;
    }
    if (!std::is_sorted(phonemes.begin(), phonemes.end()) ||
        std::adjacent_find(phonemes.begin(), phonemes.end()) != phonemes.end()) {
        return core::Result<void>::failure(validation(
            "phoneme inventory must be sorted and unique"));
    }
    return core::Result<void>::success();
}

std::string PhonemeInventory::serialize() const {
    std::ostringstream output;
    output << kInventoryMagic << ' ' << kSchemaVersion << '\n';
    output << "LOCALE " << detail::json_string(locale) << '\n';
    output << "PHONEMES " << phonemes.size() << '\n';
    for (const std::string& phoneme : phonemes) {
        output << "PHONEME " << detail::json_string(phoneme) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PhonemeInventory::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<PhonemeInventory> PhonemeInventory::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<PhonemeInventory>::failure(parse_error(
            "phoneme inventory text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kInventoryMagic, kSchemaVersion); !result) {
        return core::Result<PhonemeInventory>::failure(result.error());
    }
    PhonemeInventory inventory;
    const auto locale_tag = detail::read_token(input, "LOCALE");
    if (!locale_tag) return core::Result<PhonemeInventory>::failure(locale_tag.error());
    if (locale_tag.value() != "LOCALE") {
        return core::Result<PhonemeInventory>::failure(parse_error(
            "phoneme inventory framing is invalid: LOCALE"));
    }
    const auto locale = detail::read_quoted(input, "LOCALE", kMaxLocaleBytes);
    if (!locale) return core::Result<PhonemeInventory>::failure(locale.error());
    inventory.locale = locale.value();
    const auto count_tag = detail::read_token(input, "PHONEMES");
    if (!count_tag) return core::Result<PhonemeInventory>::failure(count_tag.error());
    if (count_tag.value() != "PHONEMES") {
        return core::Result<PhonemeInventory>::failure(parse_error(
            "phoneme inventory framing is invalid: PHONEMES"));
    }
    const auto count = read_count(input, "PHONEMES");
    if (!count) return core::Result<PhonemeInventory>::failure(count.error());
    if (count.value() > kMaxPhonemes) {
        return core::Result<PhonemeInventory>::failure(parse_error(
            "phoneme inventory count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        const auto tag = detail::read_token(input, "PHONEME");
        if (!tag) return core::Result<PhonemeInventory>::failure(tag.error());
        if (tag.value() != "PHONEME") {
            return core::Result<PhonemeInventory>::failure(parse_error(
                "phoneme inventory framing is invalid: PHONEME"));
        }
        const auto phoneme = detail::read_quoted(input, "PHONEME", kMaxPhonemeBytes);
        if (!phoneme) return core::Result<PhonemeInventory>::failure(phoneme.error());
        if (auto result = inventory.insert(phoneme.value()); !result) {
            return core::Result<PhonemeInventory>::failure(result.error());
        }
    }
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<PhonemeInventory>::failure(result.error());
    }
    if (auto result = inventory.validate(); !result) {
        return core::Result<PhonemeInventory>::failure(result.error());
    }
    return core::Result<PhonemeInventory>::success(std::move(inventory));
}

core::Result<void> VisemeMapping::insert(VisemeMappingEntry entry) {
    if (auto result = validate_mapping_entry(entry); !result) return result;
    if (entries.size() >= kMaxEntries) {
        return core::Result<void>::failure(invalid(
            "viseme mapping exceeds its entry bound"));
    }
    const auto position = std::lower_bound(
        entries.begin(), entries.end(), entry,
        [](const VisemeMappingEntry& left, const VisemeMappingEntry& right) {
            return left.phoneme < right.phoneme;
        });
    if (position != entries.end() && position->phoneme == entry.phoneme) {
        return core::Result<void>::failure(invalid(
            "viseme mapping already contains this phoneme"));
    }
    entries.insert(position, std::move(entry));
    return core::Result<void>::success();
}

core::Result<VisemeMappingLookup> VisemeMapping::map(std::string_view phoneme) const {
    const auto position = std::lower_bound(
        entries.begin(), entries.end(), phoneme,
        [](const VisemeMappingEntry& left, std::string_view right) {
            return left.phoneme < right;
        });
    if (position == entries.end() || position->phoneme != phoneme) {
        return core::Result<VisemeMappingLookup>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "phoneme has no viseme mapping entry: " + std::string(phoneme)));
    }
    return core::Result<VisemeMappingLookup>::success(VisemeMappingLookup{
        std::string_view(position->viseme), position->weight});
}

core::Result<void> VisemeMapping::validate() const {
    if (schema_version != kSchemaVersion) {
        return core::Result<void>::failure(version_error(
            "viseme mapping schema version is not supported"));
    }
    if (!safe_token(name, kMaxNameBytes)) {
        return core::Result<void>::failure(invalid(
            "viseme mapping name is not a bounded safe token"));
    }
    if (!safe_token(locale, kMaxLocaleBytes)) {
        return core::Result<void>::failure(invalid(
            "viseme mapping locale is not a bounded safe token"));
    }
    if (inventory_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "viseme mapping inventory digest must be nonzero"));
    }
    if (entries.size() > kMaxEntries) {
        return core::Result<void>::failure(validation(
            "viseme mapping exceeds its entry bound"));
    }
    for (const VisemeMappingEntry& entry : entries) {
        if (auto result = validate_mapping_entry(entry); !result) return result;
    }
    if (!std::is_sorted(
            entries.begin(), entries.end(),
            [](const VisemeMappingEntry& left, const VisemeMappingEntry& right) {
                return left.phoneme < right.phoneme;
            })) {
        return core::Result<void>::failure(validation(
            "viseme mapping entries must be sorted by phoneme"));
    }
    for (std::size_t index = 1U; index < entries.size(); ++index) {
        if (entries[index - 1U].phoneme == entries[index].phoneme) {
            return core::Result<void>::failure(validation(
                "viseme mapping entries must be unique by phoneme"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> VisemeMapping::validate_against(
    const PhonemeInventory& inventory) const {
    if (auto result = validate(); !result) return result;
    if (auto result = inventory.validate(); !result) return result;
    if (locale != inventory.locale) {
        return core::Result<void>::failure(validation(
            "viseme mapping locale disagrees with the phoneme inventory"));
    }
    const auto digest = inventory.canonical_digest();
    if (!digest) return core::Result<void>::failure(digest.error());
    if (digest.value() != inventory_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "viseme mapping inventory digest is stale"));
    }
    for (const VisemeMappingEntry& entry : entries) {
        if (!inventory.contains(entry.phoneme)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "viseme mapping entry is not an inventory phoneme: " + entry.phoneme));
        }
    }
    return core::Result<void>::success();
}

std::string VisemeMapping::serialize() const {
    std::ostringstream output;
    output << kMappingMagic << ' ' << kSchemaVersion << '\n';
    output << "NAME " << detail::json_string(name) << '\n';
    output << "LOCALE " << detail::json_string(locale) << '\n';
    output << "INVENTORY_SHA256 ";
    write_digest(output, inventory_digest);
    output << '\n';
    output << "ENTRIES " << entries.size() << '\n';
    for (const VisemeMappingEntry& entry : entries) {
        output << "ENTRY " << detail::json_string(entry.phoneme) << ' '
               << detail::json_string(entry.viseme) << ' ';
        detail::write_double(output, entry.weight);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> VisemeMapping::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<VisemeMapping> VisemeMapping::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<VisemeMapping>::failure(parse_error(
            "viseme mapping text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    if (auto result = detail::expect_magic(input, kMappingMagic, kSchemaVersion); !result) {
        return core::Result<VisemeMapping>::failure(result.error());
    }
    VisemeMapping mapping;
    const auto name_tag = detail::read_token(input, "NAME");
    if (!name_tag) return core::Result<VisemeMapping>::failure(name_tag.error());
    if (name_tag.value() != "NAME") {
        return core::Result<VisemeMapping>::failure(parse_error(
            "viseme mapping framing is invalid: NAME"));
    }
    const auto name = detail::read_quoted(input, "NAME", kMaxNameBytes);
    if (!name) return core::Result<VisemeMapping>::failure(name.error());
    mapping.name = name.value();
    const auto locale_tag = detail::read_token(input, "LOCALE");
    if (!locale_tag) return core::Result<VisemeMapping>::failure(locale_tag.error());
    if (locale_tag.value() != "LOCALE") {
        return core::Result<VisemeMapping>::failure(parse_error(
            "viseme mapping framing is invalid: LOCALE"));
    }
    const auto locale = detail::read_quoted(input, "LOCALE", kMaxLocaleBytes);
    if (!locale) return core::Result<VisemeMapping>::failure(locale.error());
    mapping.locale = locale.value();
    const auto digest_tag = detail::read_token(input, "INVENTORY_SHA256");
    if (!digest_tag) return core::Result<VisemeMapping>::failure(digest_tag.error());
    if (digest_tag.value() != "INVENTORY_SHA256") {
        return core::Result<VisemeMapping>::failure(parse_error(
            "viseme mapping framing is invalid: INVENTORY_SHA256"));
    }
    const auto digest = read_digest(input, "INVENTORY_SHA256");
    if (!digest) return core::Result<VisemeMapping>::failure(digest.error());
    mapping.inventory_digest = digest.value();
    const auto count_tag = detail::read_token(input, "ENTRIES");
    if (!count_tag) return core::Result<VisemeMapping>::failure(count_tag.error());
    if (count_tag.value() != "ENTRIES") {
        return core::Result<VisemeMapping>::failure(parse_error(
            "viseme mapping framing is invalid: ENTRIES"));
    }
    const auto count = read_count(input, "ENTRIES");
    if (!count) return core::Result<VisemeMapping>::failure(count.error());
    if (count.value() > kMaxEntries) {
        return core::Result<VisemeMapping>::failure(parse_error(
            "viseme mapping entry count exceeds its bound"));
    }
    for (std::uint64_t index = 0U; index < count.value(); ++index) {
        const auto tag = detail::read_token(input, "ENTRY");
        if (!tag) return core::Result<VisemeMapping>::failure(tag.error());
        if (tag.value() != "ENTRY") {
            return core::Result<VisemeMapping>::failure(parse_error(
                "viseme mapping framing is invalid: ENTRY"));
        }
        const auto phoneme = detail::read_quoted(
            input, "ENTRY phoneme", PhonemeInventory::kMaxPhonemeBytes);
        if (!phoneme) return core::Result<VisemeMapping>::failure(phoneme.error());
        const auto viseme = detail::read_quoted(input, "ENTRY viseme", kMaxVisemeBytes);
        if (!viseme) return core::Result<VisemeMapping>::failure(viseme.error());
        const auto weight_token = detail::read_token(input, "ENTRY weight");
        if (!weight_token) {
            return core::Result<VisemeMapping>::failure(weight_token.error());
        }
        const auto weight = detail::parse_double_token(weight_token.value(), "ENTRY weight");
        if (!weight) return core::Result<VisemeMapping>::failure(weight.error());
        if (auto result = mapping.insert(VisemeMappingEntry{
                phoneme.value(), viseme.value(), weight.value()});
            !result) {
            return core::Result<VisemeMapping>::failure(result.error());
        }
    }
    if (auto result = detail::expect_end(input); !result) {
        return core::Result<VisemeMapping>::failure(result.error());
    }
    if (auto result = mapping.validate(); !result) {
        return core::Result<VisemeMapping>::failure(result.error());
    }
    return core::Result<VisemeMapping>::success(std::move(mapping));
}

} // namespace carto::face
