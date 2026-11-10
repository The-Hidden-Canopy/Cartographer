#include <carto/assets/source_registry.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace carto::assets {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_token(std::string_view value, std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum || value.front() == '.' ||
        value.front() == '-' || value.front() == '_') return false;
    return std::all_of(value.begin(), value.end(), [](char character) {
        const bool letter = (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        return letter || digit || character == '.' || character == '_' || character == '-';
    });
}

bool valid_authored_tag(std::string_view value) noexcept {
    if (!valid_token(value, SourceImportRecord::kMaxAuthoredTagBytes)) return false;
    return std::none_of(value.begin(), value.end(), [](char character) {
        return character >= 'A' && character <= 'Z';
    });
}

bool valid_relative_path(std::string_view path) noexcept {
    if (path.empty() || path.size() > SourceImportRecord::kMaxRelativePathBytes ||
        path.front() == '/' || path.front() == '\\' || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) return false;
    if (std::any_of(path.begin(), path.end(), [](unsigned char byte) {
            return byte < 0x20U || byte == 0x7fU;
        })) return false;
    std::size_t begin = 0U;
    while (begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view component = end == std::string_view::npos
            ? path.substr(begin) : path.substr(begin, end - begin);
        if (component.empty() || component == "." || component == "..") return false;
        if (component.back() == '.' || component.back() == ' ') return false;
        std::string_view device_base = component.substr(0U, component.find('.'));
        while (!device_base.empty() && device_base.back() == ' ') {
            device_base.remove_suffix(1U);
        }
        const auto equals_ascii_case_insensitive = [](std::string_view left,
                std::string_view right) noexcept {
            if (left.size() != right.size()) return false;
            for (std::size_t index = 0U; index < left.size(); ++index) {
                char value = left[index];
                if (value >= 'a' && value <= 'z') {
                    value = static_cast<char>(value - ('a' - 'A'));
                }
                if (value != right[index]) return false;
            }
            return true;
        };
        const bool numbered_device = device_base.size() == 4U &&
            (equals_ascii_case_insensitive(device_base.substr(0U, 3U), "COM") ||
             equals_ascii_case_insensitive(device_base.substr(0U, 3U), "LPT")) &&
            device_base[3U] >= '1' && device_base[3U] <= '9';
        if (equals_ascii_case_insensitive(device_base, "CON") ||
            equals_ascii_case_insensitive(device_base, "PRN") ||
            equals_ascii_case_insensitive(device_base, "AUX") ||
            equals_ascii_case_insensitive(device_base, "NUL") ||
            equals_ascii_case_insensitive(device_base, "CONIN$") ||
            equals_ascii_case_insensitive(device_base, "CONOUT$") ||
            numbered_device) return false;
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return true;
}

bool read_unsigned(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value,
        std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() &&
        std::isfinite(value);
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed || parsed.value().is_zero()) return false;
    digest = parsed.value();
    return true;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::optional<SourceAssetDomain> changed_domain(
    const SourceImportRecord& previous,
    const SourceImportRecord& incoming,
    std::size_t& changed_count) {
    changed_count = 0U;
    std::optional<SourceAssetDomain> first_changed;
    for (std::uint8_t value = 0U; value <=
         static_cast<std::uint8_t>(SourceAssetDomain::material); ++value) {
        const auto domain = static_cast<SourceAssetDomain>(value);
        const auto find_digest = [domain](const auto& record) -> assets::Sha256Digest {
            const auto found = std::find_if(record.domain_digests.begin(),
                record.domain_digests.end(), [domain](const auto& item) {
                    return item.domain == domain;
                });
            return found == record.domain_digests.end() ? assets::Sha256Digest{} : found->digest;
        };
        if (find_digest(previous) != find_digest(incoming)) {
            if (!first_changed.has_value()) first_changed = domain;
            ++changed_count;
        }
    }
    return first_changed;
}

SourceReimportChange classify(
    const SourceImportRecord* previous,
    const SourceImportRecord* incoming) {
    if (previous == nullptr) return SourceReimportChange::added;
    if (incoming == nullptr) return SourceReimportChange::removed;
    if (previous->artifact_identity != incoming->artifact_identity ||
        previous->kind != incoming->kind ||
        previous->document_products != incoming->document_products) {
        return SourceReimportChange::identity_conflict;
    }
    std::size_t changed_count = 0U;
    const auto first_changed = changed_domain(*previous, *incoming, changed_count);
    if (changed_count == 1U) {
        switch (*first_changed) {
        case SourceAssetDomain::geometry: return SourceReimportChange::geometry_changed;
        case SourceAssetDomain::hierarchy: return SourceReimportChange::hierarchy_changed;
        case SourceAssetDomain::material: return SourceReimportChange::material_changed;
        }
    }
    if (changed_count > 1U) return SourceReimportChange::multiple_domains_changed;
    if (previous->serialize() == incoming->serialize()) return SourceReimportChange::unchanged;
    if (previous->artifact_digest == incoming->artifact_digest) {
        return SourceReimportChange::metadata_only;
    }
    return SourceReimportChange::source_changed_unclassified;
}

} // namespace

Sha256Digest source_document_product_digest(
    const Sha256Digest& source_digest, std::span<const SourceDocumentProduct> products) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTOGRAPHER_SOURCE_DOCUMENT_PRODUCTS 1\n" << source_digest.hex() << '\n'
           << products.size() << '\n';
    for (const auto& product : products) {
        output << static_cast<unsigned int>(product.kind) << ' ' << product.source_index
               << ' ' << product.product_id << '\n';
    }
    return digest_text(output.str());
}

core::Result<void> BotanicalGenerationReceipt::validate() const {
    if (source_snapshot.empty() ||
        source_snapshot.size() > kMaxSourceSnapshotBytes ||
        job_identity_digest.is_zero() || input_artifact_digest.is_zero() ||
        output_artifact_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "botanical generation receipt identity, snapshot, or digests are invalid"));
    }
    if (digest_text(source_snapshot) != input_artifact_digest) {
        return core::Result<void>::failure(validation(
            "botanical generation input digest does not match the canonical source snapshot"));
    }
    const auto source = botanical::Source::deserialize(source_snapshot);
    if (!source || source.value().serialize() != source_snapshot) {
        return core::Result<void>::failure(validation(
            "botanical generation receipt source is not canonical"));
    }
    return core::Result<void>::success();
}

std::string BotanicalGenerationReceipt::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "BOTANICAL_GENERATION_RECEIPT 1\n"
           << "SOURCE_SNAPSHOT " << std::quoted(source_snapshot) << '\n'
           << "JOB_ID_DIGEST " << job_identity_digest.hex() << '\n'
           << "INPUT_ARTIFACT_DIGEST " << input_artifact_digest.hex() << '\n'
           << "OUTPUT_ARTIFACT_DIGEST " << output_artifact_digest.hex() << '\n'
           << "END\n";
    return output.str();
}

core::Result<BotanicalGenerationReceipt> BotanicalGenerationReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSourceSnapshotBytes + 1024U) {
        return core::Result<BotanicalGenerationReceipt>::failure(validation(
            "botanical generation receipt is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string label;
    std::uint64_t version = 0U;
    BotanicalGenerationReceipt result;
    if (!(input >> label >> version) || label != "BOTANICAL_GENERATION_RECEIPT" ||
        version != 1U || !(input >> label >> std::quoted(result.source_snapshot)) ||
        label != "SOURCE_SNAPSHOT" || !(input >> label) || label != "JOB_ID_DIGEST" ||
        !read_digest(input, result.job_identity_digest) || !(input >> label) ||
        label != "INPUT_ARTIFACT_DIGEST" || !read_digest(input, result.input_artifact_digest) ||
        !(input >> label) || label != "OUTPUT_ARTIFACT_DIGEST" ||
        !read_digest(input, result.output_artifact_digest) || !(input >> label) ||
        label != "END") {
        return core::Result<BotanicalGenerationReceipt>::failure(validation(
            "botanical generation receipt is malformed"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<BotanicalGenerationReceipt>::failure(validation(
            "botanical generation receipt contains unbound trailing data"));
    }
    if (auto checked = result.validate(); !checked) {
        return core::Result<BotanicalGenerationReceipt>::failure(checked.error());
    }
    return core::Result<BotanicalGenerationReceipt>::success(std::move(result));
}

core::Result<void> SourceImportOptions::validate() const {
    if (!std::isfinite(meters_per_source_unit) || meters_per_source_unit <= 0.0 ||
        meters_per_source_unit > 1.0e6 || up_axis > 2U ||
        !std::isfinite(weld_tolerance_meters) || weld_tolerance_meters < 0.0 ||
        weld_tolerance_meters > 1.0e3 || (weld_vertices && weld_tolerance_meters <= 0.0)) {
        return core::Result<void>::failure(validation(
            "source import options contain invalid units, axis, or weld tolerance"));
    }
    return core::Result<void>::success();
}

std::string SourceImportOptions::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "OPTIONS " << meters_per_source_unit << ' '
        << static_cast<unsigned int>(up_axis) << ' ' << (right_handed ? 1 : 0) << ' '
        << (triangulate ? 1 : 0) << ' ' << (import_hierarchy ? 1 : 0) << ' '
        << (import_materials ? 1 : 0) << ' ' << (import_normals ? 1 : 0) << ' '
        << (weld_vertices ? 1 : 0) << ' ' << weld_tolerance_meters;
    return output.str();
}

assets::Sha256Digest SourceImportOptions::digest() const {
    return digest_text(serialize());
}

core::Result<void> SourceImportRecord::validate() const {
    if (!valid_token(identity, kMaxIdentityBytes) || !valid_relative_path(relative_source_path) ||
        source_generation.value() == 0U ||
        static_cast<std::uint8_t>(kind) > static_cast<std::uint8_t>(SourceAssetKind::other) ||
        source_digest.is_zero() || !valid_token(importer_identity, kMaxImporterIdentityBytes) ||
        importer_version == 0U || !valid_token(artifact_identity, kMaxIdentityBytes) ||
        artifact_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "source import identity, path, generation, kind, importer, or digests are invalid"));
    }
    if (importer_identity == "carto.import.gltf-scene" && importer_version > 2U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "glTF scene source lineage version " + std::to_string(importer_version) +
                " is newer than this reader supports (maximum 2)"));
    }
    if (auto result = options.validate(); !result) return result;
    if (domain_digests.size() > kMaxDomainDigests) {
        return core::Result<void>::failure(validation(
            "source import contains too many domain digests"));
    }
    std::optional<SourceAssetDomain> previous_domain;
    for (const auto& item : domain_digests) {
        if (static_cast<std::uint8_t>(item.domain) >
                static_cast<std::uint8_t>(SourceAssetDomain::material) ||
            item.digest.is_zero() || (previous_domain.has_value() &&
                static_cast<std::uint8_t>(item.domain) <=
                    static_cast<std::uint8_t>(*previous_domain))) {
            return core::Result<void>::failure(validation(
                "source import domain digests must be nonzero, sorted, and unique"));
        }
        previous_domain = item.domain;
    }
    if (authored_tags.size() > kMaxAuthoredTags || dependencies.size() > kMaxDependencies) {
        return core::Result<void>::failure(validation(
            "source authored tags or dependencies exceed their bounds"));
    }
    std::string previous_tag;
    for (const auto& tag : authored_tags) {
        if (!valid_authored_tag(tag) || (!previous_tag.empty() && tag <= previous_tag)) {
            return core::Result<void>::failure(validation(
                "source authored tags must be canonical, sorted, and unique"));
        }
        previous_tag = tag;
    }
    std::string previous_dependency;
    for (const auto& dependency : dependencies) {
        if (!valid_token(dependency, kMaxIdentityBytes) || dependency == identity ||
            (!previous_dependency.empty() && dependency <= previous_dependency)) {
            return core::Result<void>::failure(validation(
                "source dependencies must be distinct sorted identities other than themselves"));
        }
        previous_dependency = dependency;
    }
    if (importer_identity == "carto.import.gltf-scene" && document_products.empty()) {
        return core::Result<void>::failure(validation(
            "glTF scene source requires its product mapping"));
    }
    if (!document_products.empty()) {
        if (kind != SourceAssetKind::other || importer_identity != "carto.import.gltf-scene" ||
            (importer_version != 1U && importer_version != 2U) || botanical_generation.has_value() ||
            document_products.size() > kMaxDocumentProducts) {
            return core::Result<void>::failure(validation(
                "document product bindings require the bounded glTF scene importer"));
        }
        std::set<std::pair<std::uint8_t, std::uint64_t>> destinations;
        std::optional<std::pair<std::uint8_t, std::uint64_t>> previous;
        std::array<std::uint64_t, 5U> next_index{1U, 1U, 1U, 1U, 1U};
        for (const auto& product : document_products) {
            const auto kind_value = static_cast<std::uint8_t>(product.kind);
            const auto key = std::make_pair(kind_value, product.source_index);
            if (kind_value > static_cast<std::uint8_t>(SourceDocumentProductKind::sampler) ||
                product.source_index == 0U || product.product_id == 0U ||
                (previous && *previous >= key) ||
                product.source_index != next_index[kind_value]++ ||
                !destinations.emplace(kind_value, product.product_id).second) {
                return core::Result<void>::failure(validation(
                    "document product bindings must be sorted and one-to-one"));
            }
            previous = key;
        }
        if (next_index[0] == 1U || next_index[1] == 1U ||
            (importer_version == 1U && next_index[2] == 1U) ||
            artifact_digest != source_document_product_digest(source_digest, document_products)) {
            return core::Result<void>::failure(validation(
                "document product mapping is incomplete or does not match its digest"));
        }
    }
    if (botanical_generation.has_value()) {
        if (kind != SourceAssetKind::mesh ||
            (importer_identity != "carto.import.generated-gltf" &&
             importer_identity != "carto.import.generated-glb") ||
            source_digest != botanical_generation->output_artifact_digest) {
            return core::Result<void>::failure(validation(
                "botanical generation receipt does not match its imported interchange source"));
        }
        if (auto result = botanical_generation->validate(); !result) return result;
        const auto geometry = std::find_if(domain_digests.begin(), domain_digests.end(),
            [](const SourceDomainDigest& item) {
                return item.domain == SourceAssetDomain::geometry;
            });
        if (geometry == domain_digests.end() || geometry->digest != artifact_digest) {
            return core::Result<void>::failure(validation(
                "botanical generated mesh receipt lacks its exact geometry digest"));
        }
    }
    if (serialize().size() > kMaxSerializedBytes) {
        return core::Result<void>::failure(validation(
            "source import record exceeds its serialized byte limit"));
    }
    return core::Result<void>::success();
}

std::string SourceImportRecord::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    const bool has_authored_metadata = !authored_tags.empty() || !dependencies.empty();
    const std::uint32_t record_version = !document_products.empty() ? 4U : has_authored_metadata ? 3U
        : botanical_generation.has_value() ? 2U : 1U;
    output << "SOURCE_IMPORT_RECORD " << record_version << '\n';
    output << "IDENTITY " << std::quoted(identity) << ' '
        << std::quoted(relative_source_path) << ' ' << source_generation.value() << ' '
        << static_cast<unsigned int>(kind) << '\n';
    output << "SOURCE " << source_digest.hex() << '\n';
    output << "IMPORTER " << std::quoted(importer_identity) << ' ' << importer_version << '\n';
    output << options.serialize() << '\n';
    output << "ARTIFACT " << std::quoted(artifact_identity) << ' '
        << artifact_digest.hex() << '\n';
    output << "DOMAINS " << domain_digests.size() << '\n';
    for (const auto& item : domain_digests) {
        output << "DOMAIN " << static_cast<unsigned int>(item.domain) << ' '
            << item.digest.hex() << '\n';
    }
    if (record_version >= 3U) {
        output << "AUTHORED_TAGS " << authored_tags.size() << '\n';
        for (const auto& tag : authored_tags) output << "TAG " << std::quoted(tag) << '\n';
        output << "DEPENDENCIES " << dependencies.size() << '\n';
        for (const auto& dependency : dependencies) {
            output << "DEPENDENCY " << std::quoted(dependency) << '\n';
        }
    }
    if (record_version >= 4U) {
        output << "DOCUMENT_PRODUCTS " << document_products.size() << '\n';
        for (const auto& product : document_products) {
            output << "PRODUCT " << static_cast<unsigned int>(product.kind) << ' '
                   << product.source_index << ' ' << product.product_id << '\n';
        }
    }
    if (botanical_generation.has_value()) {
        output << "BOTANICAL_GENERATION "
               << std::quoted(botanical_generation->serialize()) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<SourceImportRecord> SourceImportRecord::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<SourceImportRecord>::failure(invalid(
            "source import record is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string token;
    std::uint64_t number = 0U;
    if (!(input >> token) || token != "SOURCE_IMPORT_RECORD" || !read_unsigned(input, number) || number == 0U) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import record header or schema is invalid"));
    }
    if (number > 4U) {
        return core::Result<SourceImportRecord>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "source import record schema is newer than this reader supports (maximum 4)"));
    }
    const std::uint64_t record_version = number;
    SourceImportRecord result;
    std::uint64_t generation = 0U;
    std::uint64_t kind_value = 0U;
    if (!(input >> token) || token != "IDENTITY" ||
        !(input >> std::quoted(result.identity) >>
            std::quoted(result.relative_source_path)) ||
        !read_unsigned(input, generation) || !read_unsigned(input, kind_value) ||
        kind_value > static_cast<std::uint64_t>(SourceAssetKind::other)) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import identity record is malformed"));
    }
    result.source_generation = core::Revision{generation};
    result.kind = static_cast<SourceAssetKind>(kind_value);
    if (!(input >> token) || token != "SOURCE" || !read_digest(input, result.source_digest) ||
        !(input >> token) || token != "IMPORTER" ||
        !(input >> std::quoted(result.importer_identity)) ||
        !read_unsigned(input, number) || number > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import source or importer record is malformed"));
    }
    result.importer_version = static_cast<std::uint32_t>(number);
    std::uint64_t axis = 0U;
    std::uint64_t right_handed = 0U;
    std::uint64_t triangulate = 0U;
    std::uint64_t import_hierarchy = 0U;
    std::uint64_t import_materials = 0U;
    std::uint64_t import_normals = 0U;
    std::uint64_t weld_vertices = 0U;
    if (!(input >> token) || token != "OPTIONS" ||
        !read_double(input, result.options.meters_per_source_unit) ||
        !read_unsigned(input, axis) || !read_unsigned(input, right_handed) ||
        !read_unsigned(input, triangulate) || !read_unsigned(input, import_hierarchy) ||
        !read_unsigned(input, import_materials) || !read_unsigned(input, import_normals) ||
        !read_unsigned(input, weld_vertices) ||
        !read_double(input, result.options.weld_tolerance_meters) || axis > 2U ||
        right_handed > 1U || triangulate > 1U || import_hierarchy > 1U ||
        import_materials > 1U || import_normals > 1U || weld_vertices > 1U) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import option record is malformed"));
    }
    result.options.up_axis = static_cast<std::uint8_t>(axis);
    result.options.right_handed = right_handed != 0U;
    result.options.triangulate = triangulate != 0U;
    result.options.import_hierarchy = import_hierarchy != 0U;
    result.options.import_materials = import_materials != 0U;
    result.options.import_normals = import_normals != 0U;
    result.options.weld_vertices = weld_vertices != 0U;
    if (!(input >> token) || token != "ARTIFACT" ||
        !(input >> std::quoted(result.artifact_identity)) ||
        !read_digest(input, result.artifact_digest) ||
        !(input >> token) || token != "DOMAINS" || !read_unsigned(input, number) ||
        number > kMaxDomainDigests) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import artifact or domain table is malformed"));
    }
    result.domain_digests.reserve(static_cast<std::size_t>(number));
    for (std::uint64_t index = 0U; index < number; ++index) {
        std::uint64_t domain = 0U;
        SourceDomainDigest item;
        if (!(input >> token) || token != "DOMAIN" || !read_unsigned(input, domain) ||
            domain > static_cast<std::uint64_t>(SourceAssetDomain::material) ||
            !read_digest(input, item.digest)) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source import domain digest record is malformed"));
        }
        item.domain = static_cast<SourceAssetDomain>(domain);
        result.domain_digests.push_back(item);
    }
    if (record_version >= 3U) {
        if (!(input >> token) || token != "AUTHORED_TAGS" ||
            !read_unsigned(input, number) || number > kMaxAuthoredTags) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source authored tag table is malformed"));
        }
        result.authored_tags.reserve(static_cast<std::size_t>(number));
        for (std::uint64_t index = 0U; index < number; ++index) {
            std::string tag;
            if (!(input >> token) || token != "TAG" || !(input >> std::quoted(tag))) {
                return core::Result<SourceImportRecord>::failure(validation(
                    "source authored tag record is malformed"));
            }
            result.authored_tags.push_back(std::move(tag));
        }
        if (!(input >> token) || token != "DEPENDENCIES" ||
            !read_unsigned(input, number) || number > kMaxDependencies) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source dependency table is malformed"));
        }
        result.dependencies.reserve(static_cast<std::size_t>(number));
        for (std::uint64_t index = 0U; index < number; ++index) {
            std::string dependency;
            if (!(input >> token) || token != "DEPENDENCY" ||
                !(input >> std::quoted(dependency))) {
                return core::Result<SourceImportRecord>::failure(validation(
                    "source dependency record is malformed"));
            }
            result.dependencies.push_back(std::move(dependency));
        }
        if (record_version == 3U && result.authored_tags.empty() && result.dependencies.empty()) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source record version 3 requires authored metadata"));
        }
    }
    if (record_version >= 4U) {
        if (!(input >> token) || token != "DOCUMENT_PRODUCTS" ||
            !read_unsigned(input, number) || number == 0U || number > kMaxDocumentProducts) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source document product table is malformed"));
        }
        result.document_products.reserve(static_cast<std::size_t>(number));
        for (std::uint64_t index = 0U; index < number; ++index) {
            std::uint64_t kind_value = 0U, source_index = 0U, product_id = 0U;
            if (!(input >> token) || token != "PRODUCT" || !read_unsigned(input, kind_value) ||
                kind_value > static_cast<std::uint64_t>(SourceDocumentProductKind::sampler) ||
                !read_unsigned(input, source_index) || !read_unsigned(input, product_id)) {
                return core::Result<SourceImportRecord>::failure(validation(
                    "source document product record is malformed"));
            }
            result.document_products.push_back({static_cast<SourceDocumentProductKind>(kind_value),
                                                source_index, product_id});
        }
    }
    if (record_version >= 2U) {
        if (!(input >> token)) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source import record is missing its end marker"));
        }
        const bool has_botanical_receipt = token == "BOTANICAL_GENERATION";
        if (record_version == 2U && !has_botanical_receipt) {
            return core::Result<SourceImportRecord>::failure(validation(
                "source import botanical generation receipt is malformed"));
        }
        if (has_botanical_receipt) {
            std::string encoded_receipt;
            if (!(input >> std::quoted(encoded_receipt))) {
                return core::Result<SourceImportRecord>::failure(validation(
                    "source import botanical generation receipt is malformed"));
            }
            auto receipt = BotanicalGenerationReceipt::deserialize(encoded_receipt);
            if (!receipt) {
                return core::Result<SourceImportRecord>::failure(receipt.error());
            }
            result.botanical_generation = std::move(receipt.value());
            if (!(input >> token)) {
                return core::Result<SourceImportRecord>::failure(validation(
                    "source import record is missing its end marker"));
            }
        }
    } else if (!(input >> token)) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import record is missing its end marker"));
    }
    if (token != "END") {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import record is missing END"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<SourceImportRecord>::failure(validation(
            "source import record contains unbound trailing data"));
    }
    if (auto checked = result.validate(); !checked) {
        return core::Result<SourceImportRecord>::failure(checked.error());
    }
    return core::Result<SourceImportRecord>::success(std::move(result));
}

namespace {

core::Result<void> validate_registry_storage_bounds(const ProjectAssetRegistry& registry) {
    if (registry.records().size() > ProjectAssetRegistry::kMaxRecords ||
        registry.lineage().size() > ProjectAssetRegistry::kMaxRecords * 2U) {
        return core::Result<void>::failure(validation(
            "project asset registry exceeds its active or lineage record bound"));
    }
    std::size_t aggregate_bytes = 0U;
    for (const auto& [identity, record] : registry.records()) {
        static_cast<void>(identity);
        const std::size_t size = record.serialize().size();
        if (size > SourceImportRecord::kMaxSerializedBytes ||
            size > ProjectAssetRegistry::kMaxAggregateRecordBytes - aggregate_bytes) {
            return core::Result<void>::failure(validation(
                "project asset registry record bytes exceed their serialized bounds"));
        }
        aggregate_bytes += size;
    }
    if (registry.serialize().size() > ProjectAssetRegistry::kMaxSerializedBytes) {
        return core::Result<void>::failure(validation(
            "project asset registry exceeds its serialized byte limit"));
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> ProjectAssetRegistry::set_record(SourceImportRecord record) {
    if (auto result = record.validate(); !result) return result;
    const std::string identity = record.identity;
    const auto current = records_.find(identity);
    const auto lineage = lineage_.find(identity);
    if (current != records_.end() && current->second.serialize() == record.serialize()) {
        return core::Result<void>::success();
    }
    if (current != records_.end() &&
        (current->second.kind != record.kind ||
         current->second.artifact_identity != record.artifact_identity ||
         current->second.document_products != record.document_products)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source reimport cannot silently change kind or imported artifact identity"));
    }
    core::Revision expected{1U};
    if (lineage != lineage_.end()) {
        if (lineage->second.exhausted()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "source asset generation space is exhausted"));
        }
        expected = lineage->second.next();
    }
    if (record.source_generation != expected) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source asset update must advance its retained generation exactly once"));
    }
    std::set<std::pair<SourceDocumentProductKind, std::uint64_t>> products;
    for (const auto& product : record.document_products) {
        products.emplace(product.kind, product.product_id);
    }
    for (const auto& [other_identity, other] : records_) {
        if (other_identity != identity &&
            (other.relative_source_path == record.relative_source_path ||
             other.artifact_identity == record.artifact_identity)) {
            return core::Result<void>::failure(validation(
                "source path or imported artifact identity is already assigned"));
        }
        if (other_identity != identity) {
            for (const auto& product : other.document_products) {
                if (products.contains({product.kind, product.product_id})) {
                    return core::Result<void>::failure(validation(
                        "source document product is already bound to another source"));
                }
            }
        }
    }
    if (records_.size() >= kMaxRecords && current == records_.end()) {
        return core::Result<void>::failure(validation(
            "project asset registry exceeds its record limit"));
    }
    ProjectAssetRegistry candidate = *this;
    candidate.records_.insert_or_assign(identity, std::move(record));
    candidate.lineage_.insert_or_assign(identity, expected);
    if (auto result = validate_registry_storage_bounds(candidate); !result) return result;
    records_.swap(candidate.records_);
    lineage_.swap(candidate.lineage_);
    return core::Result<void>::success();
}

core::Result<void> ProjectAssetRegistry::erase_record(
    std::string_view identity,
    core::Revision expected_generation) {
    const auto found = records_.find(std::string(identity));
    if (found == records_.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "source asset does not exist in the registry"));
    }
    if (found->second.source_generation != expected_generation) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source asset removal generation does not match the active record"));
    }
    records_.erase(found);
    return core::Result<void>::success();
}

core::Result<void> ProjectAssetRegistry::validate() const {
    if (auto result = validate_registry_storage_bounds(*this); !result) return result;
    std::set<std::string> paths;
    std::set<std::string> artifacts;
    std::set<std::pair<SourceDocumentProductKind, std::uint64_t>> products;
    for (const auto& [identity, record] : records_) {
        if (identity != record.identity) {
            return core::Result<void>::failure(validation(
                "project asset registry key does not match its source identity"));
        }
        if (auto result = record.validate(); !result) return result;
        const auto generation = lineage_.find(identity);
        if (generation == lineage_.end() || generation->second != record.source_generation) {
            return core::Result<void>::failure(validation(
                "active source asset does not match retained generation lineage"));
        }
        if (!paths.insert(record.relative_source_path).second ||
            !artifacts.insert(record.artifact_identity).second) {
            return core::Result<void>::failure(validation(
                "project asset registry contains duplicate paths or artifact identities"));
        }
        for (const auto& product : record.document_products) {
            if (!products.emplace(product.kind, product.product_id).second) {
                return core::Result<void>::failure(validation(
                    "source document product is bound to more than one source"));
            }
        }
        for (const auto& dependency : record.dependencies) {
            if (!records_.contains(dependency)) {
                return core::Result<void>::failure(validation(
                    "source dependency points to an identity outside the active registry"));
            }
        }
    }
    for (const auto& [identity, generation] : lineage_) {
        if (!valid_token(identity, SourceImportRecord::kMaxIdentityBytes) ||
            generation.value() == 0U) {
            return core::Result<void>::failure(validation(
                "project asset registry lineage contains an invalid identity or generation"));
        }
        const auto active = records_.find(identity);
        if (active != records_.end() && active->second.source_generation != generation) {
            return core::Result<void>::failure(validation(
                "active source asset generation disagrees with its lineage"));
        }
    }
    return core::Result<void>::success();
}

std::string ProjectAssetRegistry::serialize() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "CARTOGRAPHER_SOURCE_ASSET_REGISTRY " << kSchemaVersion << '\n';
    output << "RECORDS " << records_.size() << '\n';
    for (const auto& [identity, record] : records_) {
        output << "RECORD " << std::quoted(identity) << ' '
            << std::quoted(record.serialize()) << '\n';
    }
    output << "LINEAGE " << lineage_.size() << '\n';
    for (const auto& [identity, generation] : lineage_) {
        output << "LINEAGE_RECORD " << std::quoted(identity) << ' '
            << generation.value() << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ProjectAssetRegistry> ProjectAssetRegistry::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<ProjectAssetRegistry>::failure(invalid(
            "source asset registry is empty or exceeds its serialized bound"));
    }
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string token;
    std::uint64_t version = 0U;
    std::uint64_t count = 0U;
    if (!(input >> token) || token != "CARTOGRAPHER_SOURCE_ASSET_REGISTRY" ||
        !read_unsigned(input, version) || version != kSchemaVersion ||
        !(input >> token) || token != "RECORDS" || !read_unsigned(input, count) ||
        count > kMaxRecords) {
        return core::Result<ProjectAssetRegistry>::failure(validation(
            "source asset registry header or record count is invalid"));
    }
    ProjectAssetRegistry registry;
    std::size_t aggregate_bytes = 0U;
    for (std::uint64_t index = 0U; index < count; ++index) {
        std::string identity;
        std::string encoded;
        if (!(input >> token) || token != "RECORD" ||
            !(input >> std::quoted(identity) >> std::quoted(encoded)) ||
            encoded.size() > SourceImportRecord::kMaxSerializedBytes ||
            encoded.size() > kMaxAggregateRecordBytes - aggregate_bytes) {
            return core::Result<ProjectAssetRegistry>::failure(validation(
                "source asset registry contains an invalid or oversized record"));
        }
        aggregate_bytes += encoded.size();
        auto record = SourceImportRecord::deserialize(encoded);
        if (!record) {
            return core::Result<ProjectAssetRegistry>::failure(
                record.error().with_context("source asset record " + identity));
        }
        if (record.value().identity != identity ||
            !registry.records_.emplace(identity, std::move(record.value())).second) {
            return core::Result<ProjectAssetRegistry>::failure(validation(
                "source asset record identity is duplicate or unbound"));
        }
    }
    if (!(input >> token) || token != "LINEAGE" || !read_unsigned(input, count) ||
        count > kMaxRecords * 2U) {
        return core::Result<ProjectAssetRegistry>::failure(validation(
            "source asset registry lineage table is malformed"));
    }
    for (std::uint64_t index = 0U; index < count; ++index) {
        std::string identity;
        std::uint64_t generation = 0U;
        if (!(input >> token) || token != "LINEAGE_RECORD" ||
            !(input >> std::quoted(identity)) || !read_unsigned(input, generation) ||
            generation == 0U || !registry.lineage_.emplace(identity,
                core::Revision{generation}).second) {
            return core::Result<ProjectAssetRegistry>::failure(validation(
                "source asset lineage record is invalid or duplicated"));
        }
    }
    if (!(input >> token) || token != "END") {
        return core::Result<ProjectAssetRegistry>::failure(validation(
            "source asset registry is missing END"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<ProjectAssetRegistry>::failure(validation(
            "source asset registry contains trailing unbound data"));
    }
    if (auto result = registry.validate(); !result) {
        return core::Result<ProjectAssetRegistry>::failure(result.error());
    }
    return core::Result<ProjectAssetRegistry>::success(std::move(registry));
}

core::Result<assets::Sha256Digest> ProjectAssetRegistry::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<void> SourceReimportPlan::validate() const {
    if (base_registry_digest.is_zero() || incoming_set_digest.is_zero() ||
        entries.size() > ProjectAssetRegistry::kMaxRecords) {
        return core::Result<void>::failure(validation(
            "source reimport plan digests or entry count are invalid"));
    }
    std::string previous_identity;
    std::set<std::string> incoming_identities;
    for (const auto& entry : entries) {
        if (!valid_token(entry.identity, SourceImportRecord::kMaxIdentityBytes) ||
            (!previous_identity.empty() && entry.identity <= previous_identity) ||
            static_cast<std::uint8_t>(entry.change) >
                static_cast<std::uint8_t>(SourceReimportChange::identity_conflict)) {
            return core::Result<void>::failure(validation(
                "source reimport plan entries must be sorted and uniquely identified"));
        }
        if (entry.incoming.has_value() &&
            (entry.incoming->identity != entry.identity || !entry.incoming->validate())) {
            return core::Result<void>::failure(validation(
                "source reimport plan candidate does not match its entry identity"));
        }
        if (entry.incoming.has_value()) incoming_identities.insert(entry.identity);
        previous_identity = entry.identity;
    }
    for (const auto& entry : entries) {
        if (!entry.incoming.has_value()) continue;
        for (const auto& dependency : entry.incoming->dependencies) {
            if (!incoming_identities.contains(dependency)) {
                return core::Result<void>::failure(validation(
                    "source reimport dependency is absent from the reviewed candidate registry"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> SourceReimportPlan::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "SOURCE_REIMPORT_PLAN 1\nBASE " << base_registry_digest.hex()
        << "\nINCOMING " << incoming_set_digest.hex() << '\n';
    for (const auto& entry : entries) {
        output << "ENTRY " << std::quoted(entry.identity) << ' '
            << static_cast<unsigned int>(entry.change) << ' '
            << entry.previous_digest.hex() << ' ';
        if (entry.incoming.has_value()) output << std::quoted(entry.incoming->serialize());
        else output << "NONE";
        output << '\n';
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(output.str()));
}

core::Result<SourceReimportPlan> plan_source_reimport(
    const ProjectAssetRegistry& current,
    std::vector<SourceImportRecord> incoming) {
    const auto base_digest = current.canonical_digest();
    if (!base_digest) return core::Result<SourceReimportPlan>::failure(base_digest.error());
    if (incoming.size() > ProjectAssetRegistry::kMaxRecords) {
        return core::Result<SourceReimportPlan>::failure(validation(
            "incoming source set exceeds the registry record limit"));
    }
    std::sort(incoming.begin(), incoming.end(), [](const auto& left, const auto& right) {
        return left.identity < right.identity;
    });
    std::set<std::string> candidate_ids;
    std::set<std::string> candidate_paths;
    std::set<std::string> candidate_artifacts;
    std::set<std::pair<SourceDocumentProductKind, std::uint64_t>> candidate_products;
    std::ostringstream incoming_canonical;
    incoming_canonical.imbue(std::locale::classic());
    incoming_canonical << "SOURCE_ASSET_CANDIDATE_SET 1\n";
    for (const auto& record : incoming) {
        if (auto result = record.validate(); !result) {
            return core::Result<SourceReimportPlan>::failure(result.error());
        }
        if (!candidate_ids.insert(record.identity).second) {
            return core::Result<SourceReimportPlan>::failure(validation(
                "incoming source set contains a duplicate stable identity"));
        }
        if (!candidate_paths.insert(record.relative_source_path).second ||
            !candidate_artifacts.insert(record.artifact_identity).second) {
            return core::Result<SourceReimportPlan>::failure(validation(
                "incoming source set contains conflicting paths or artifact identities"));
        }
        for (const auto& product : record.document_products) {
            if (!candidate_products.emplace(product.kind, product.product_id).second) {
                return core::Result<SourceReimportPlan>::failure(validation(
                    "incoming source set contains conflicting document products"));
            }
        }
        const auto active = current.records().find(record.identity);
        const auto lineage = current.lineage().find(record.identity);
        core::Revision expected{1U};
        if (lineage != current.lineage().end()) {
            if (lineage->second.exhausted()) {
                return core::Result<SourceReimportPlan>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "source asset generation space is exhausted"));
            }
            expected = lineage->second.next();
        }
        const bool exact_current = active != current.records().end() &&
            active->second.serialize() == record.serialize();
        if ((!exact_current && record.source_generation != expected) ||
            (exact_current && record.source_generation != active->second.source_generation)) {
            return core::Result<SourceReimportPlan>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "incoming source set generation is stale or skips retained lineage"));
        }
        incoming_canonical << std::quoted(record.serialize()) << '\n';
    }
    for (const auto& record : incoming) {
        for (const auto& dependency : record.dependencies) {
            if (!candidate_ids.contains(dependency)) {
                return core::Result<SourceReimportPlan>::failure(validation(
                    "incoming source dependency is absent from the candidate registry"));
            }
        }
    }
    SourceReimportPlan plan;
    plan.base_registry_digest = base_digest.value();
    plan.incoming_set_digest = digest_text(incoming_canonical.str());
    std::map<std::string, const SourceImportRecord*> incoming_by_identity;
    for (const auto& record : incoming) incoming_by_identity.emplace(record.identity, &record);
    std::set<std::string> identities;
    for (const auto& [identity, record] : current.records()) {
        static_cast<void>(record);
        identities.insert(identity);
    }
    for (const auto& record : incoming) identities.insert(record.identity);

    std::set<std::string> conflicting_identities;
    std::map<std::pair<SourceDocumentProductKind, std::uint64_t>, std::string> product_owners;
    for (const auto& [identity, existing] : current.records()) {
        for (const auto& product : existing.document_products) {
            product_owners.emplace(std::make_pair(product.kind, product.product_id), identity);
        }
    }
    for (const auto& record : incoming) {
        for (const auto& product : record.document_products) {
            const auto owner = product_owners.find({product.kind, product.product_id});
            if (owner != product_owners.end() && owner->second != record.identity) {
                conflicting_identities.insert(owner->second);
                conflicting_identities.insert(record.identity);
            }
        }
        for (const auto& [existing_identity, existing] : current.records()) {
            if (existing_identity != record.identity &&
                (existing.relative_source_path == record.relative_source_path ||
                 existing.artifact_identity == record.artifact_identity)) {
                conflicting_identities.insert(existing_identity);
                conflicting_identities.insert(record.identity);
            }
        }
    }
    for (const auto& identity : identities) {
        const auto previous = current.records().find(identity);
        const auto next = incoming_by_identity.find(identity);
        const SourceImportRecord* previous_record = previous == current.records().end()
            ? nullptr : &previous->second;
        const SourceImportRecord* incoming_record = next == incoming_by_identity.end()
            ? nullptr : next->second;
        SourceReimportPlanEntry entry;
        entry.identity = identity;
        entry.change = conflicting_identities.contains(identity)
            ? SourceReimportChange::identity_conflict
            : classify(previous_record, incoming_record);
        if (previous_record != nullptr) entry.previous_digest = previous_record->source_digest;
        if (incoming_record != nullptr) entry.incoming = *incoming_record;
        plan.entries.push_back(std::move(entry));
    }
    if (auto result = plan.validate(); !result) {
        return core::Result<SourceReimportPlan>::failure(result.error());
    }
    return core::Result<SourceReimportPlan>::success(std::move(plan));
}

core::Result<ProjectAssetRegistry> apply_source_reimport_plan(
    const ProjectAssetRegistry& current,
    const SourceReimportPlan& plan) {
    if (auto result = plan.validate(); !result) {
        return core::Result<ProjectAssetRegistry>::failure(result.error());
    }
    const auto base_digest = current.canonical_digest();
    if (!base_digest) {
        return core::Result<ProjectAssetRegistry>::failure(base_digest.error());
    }
    if (base_digest.value() != plan.base_registry_digest) {
        return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source reimport plan was reviewed against a different project asset registry"));
    }

    std::vector<SourceImportRecord> incoming;
    incoming.reserve(plan.entries.size());
    for (const auto& entry : plan.entries) {
        if (entry.incoming.has_value()) incoming.push_back(*entry.incoming);
    }
    const auto recomputed = plan_source_reimport(current, std::move(incoming));
    if (!recomputed) {
        return core::Result<ProjectAssetRegistry>::failure(recomputed.error());
    }
    const auto expected_digest = recomputed.value().canonical_digest();
    const auto supplied_digest = plan.canonical_digest();
    if (!expected_digest || !supplied_digest ||
        expected_digest.value() != supplied_digest.value()) {
        return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source reimport plan does not match its recomputed candidate classification"));
    }

    ProjectAssetRegistry updated = current;
    for (const auto& entry : recomputed.value().entries) {
        if (entry.change == SourceReimportChange::identity_conflict) {
            return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "source reimport identity conflicts must be resolved before adoption"));
        }
        if (entry.change == SourceReimportChange::unchanged) continue;
        if (entry.change == SourceReimportChange::removed) {
            const auto existing = updated.records().find(entry.identity);
            if (existing == updated.records().end()) {
                return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "source removal no longer matches the reviewed project state"));
            }
            if (auto result = updated.erase_record(
                    entry.identity, existing->second.source_generation); !result) {
                return core::Result<ProjectAssetRegistry>::failure(result.error());
            }
            continue;
        }
        if (!entry.incoming.has_value()) {
            return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "source reimport change is missing its reviewed incoming record"));
        }
        if (auto result = updated.set_record(*entry.incoming); !result) {
            return core::Result<ProjectAssetRegistry>::failure(result.error());
        }
    }
    if (auto result = updated.validate(); !result) {
        return core::Result<ProjectAssetRegistry>::failure(result.error());
    }
    return core::Result<ProjectAssetRegistry>::success(std::move(updated));
}

core::Result<void> SourceRelinkPlan::validate() const {
    if (base_registry_digest.is_zero() || staged_digest.is_zero() ||
        !valid_token(identity, SourceImportRecord::kMaxIdentityBytes) ||
        expected_generation.value() == 0U ||
        !valid_relative_path(staged_relative_path) ||
        static_cast<std::uint8_t>(disposition) >
            static_cast<std::uint8_t>(SourceRelinkDisposition::moved)) {
        return core::Result<void>::failure(validation(
            "source relink plan fields are invalid"));
    }
    if (!incoming.validate() || incoming.identity != identity ||
        incoming.relative_source_path != staged_relative_path ||
        incoming.source_digest != staged_digest) {
        return core::Result<void>::failure(validation(
            "source relink plan candidate does not match its staged locator"));
    }
    return core::Result<void>::success();
}

core::Result<assets::Sha256Digest> SourceRelinkPlan::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "SOURCE_RELINK_PLAN 1\nBASE " << base_registry_digest.hex() << '\n';
    output << "IDENTITY " << std::quoted(identity) << ' ' << expected_generation.value()
        << ' ' << std::quoted(staged_relative_path) << ' ' << staged_digest.hex() << ' '
        << static_cast<unsigned int>(disposition) << '\n';
    output << "INCOMING " << std::quoted(incoming.serialize()) << '\n';
    return core::Result<assets::Sha256Digest>::success(digest_text(output.str()));
}

core::Result<SourceRelinkPlan> plan_source_relink(
    const ProjectAssetRegistry& current,
    std::string_view identity,
    core::Revision expected_generation,
    std::string staged_relative_path,
    assets::Sha256Digest staged_digest) {
    const auto base_digest = current.canonical_digest();
    if (!base_digest) return core::Result<SourceRelinkPlan>::failure(base_digest.error());
    if (!valid_token(identity, SourceImportRecord::kMaxIdentityBytes)) {
        return core::Result<SourceRelinkPlan>::failure(invalid(
            "source relink identity is invalid"));
    }
    if (!valid_relative_path(staged_relative_path)) {
        return core::Result<SourceRelinkPlan>::failure(invalid(
            "staged source locator is not a valid project-relative path"));
    }
    if (staged_digest.is_zero()) {
        return core::Result<SourceRelinkPlan>::failure(invalid(
            "staged source digest is invalid"));
    }
    const auto active = current.records().find(std::string(identity));
    if (active == current.records().end()) {
        return core::Result<SourceRelinkPlan>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "source asset does not exist in the registry"));
    }
    if (active->second.source_generation != expected_generation) {
        return core::Result<SourceRelinkPlan>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source relink generation does not match the active record"));
    }
    if (active->second.source_digest != staged_digest) {
        return core::Result<SourceRelinkPlan>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "staged source digest differs from the bound source; changed content "
            "requires the source reimport flow"));
    }
    if (active->second.relative_source_path == staged_relative_path) {
        return core::Result<SourceRelinkPlan>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "staged source locator is already bound to this record"));
    }
    for (const auto& [other_identity, other] : current.records()) {
        if (other_identity != active->first &&
            other.relative_source_path == staged_relative_path) {
            return core::Result<SourceRelinkPlan>::failure(validation(
                "staged source locator is already assigned to another source asset"));
        }
    }
    const auto lineage = current.lineage().find(std::string(identity));
    if (lineage == current.lineage().end() || lineage->second.exhausted()) {
        return core::Result<SourceRelinkPlan>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source asset generation space is exhausted"));
    }
    SourceRelinkPlan plan;
    plan.base_registry_digest = base_digest.value();
    plan.identity = std::string(identity);
    plan.expected_generation = expected_generation;
    plan.staged_relative_path = std::move(staged_relative_path);
    plan.staged_digest = staged_digest;
    plan.incoming = active->second;
    plan.incoming.relative_source_path = plan.staged_relative_path;
    plan.incoming.source_generation = lineage->second.next();
    if (auto result = plan.validate(); !result) {
        return core::Result<SourceRelinkPlan>::failure(result.error());
    }
    return core::Result<SourceRelinkPlan>::success(std::move(plan));
}

core::Result<ProjectAssetRegistry> apply_source_relink_plan(
    const ProjectAssetRegistry& current,
    const SourceRelinkPlan& plan) {
    if (auto result = plan.validate(); !result) {
        return core::Result<ProjectAssetRegistry>::failure(result.error());
    }
    const auto base_digest = current.canonical_digest();
    if (!base_digest) {
        return core::Result<ProjectAssetRegistry>::failure(base_digest.error());
    }
    if (base_digest.value() != plan.base_registry_digest) {
        return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source relink plan was reviewed against a different project asset registry"));
    }
    const auto recomputed = plan_source_relink(current, plan.identity,
        plan.expected_generation, plan.staged_relative_path, plan.staged_digest);
    if (!recomputed) {
        return core::Result<ProjectAssetRegistry>::failure(recomputed.error());
    }
    const auto expected_digest = recomputed.value().canonical_digest();
    const auto supplied_digest = plan.canonical_digest();
    if (!expected_digest || !supplied_digest ||
        expected_digest.value() != supplied_digest.value()) {
        return core::Result<ProjectAssetRegistry>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "source relink plan does not match its recomputed review"));
    }
    ProjectAssetRegistry updated = current;
    if (auto result = updated.set_record(plan.incoming); !result) {
        return core::Result<ProjectAssetRegistry>::failure(result.error());
    }
    if (auto result = updated.validate(); !result) {
        return core::Result<ProjectAssetRegistry>::failure(result.error());
    }
    return core::Result<ProjectAssetRegistry>::success(std::move(updated));
}

} // namespace carto::assets
