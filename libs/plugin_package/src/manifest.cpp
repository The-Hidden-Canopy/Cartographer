#include <carto/plugin_package/manifest.hpp>

#include <carto/core/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::plugin_package {

namespace {

constexpr std::size_t kMaxTokenBytes = 128U;
constexpr std::size_t kMaxDisplayBytes = 256U;
constexpr std::size_t kMaxCapabilities = 64U;
constexpr std::size_t kMaxPlatforms = 32U;
constexpr std::size_t kMaxFilesystemInputs = 32U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_token(std::string_view value) {
    return !value.empty() && value.size() <= kMaxTokenBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

bool printable_text(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU;
        });
}

bool safe_relative_path(std::string_view value, std::string_view required_prefix) {
    if (value.empty() || value.size() > 512U || value.front() == '/' ||
        value.find('\\') != std::string_view::npos || value.find('\0') != std::string_view::npos ||
        value.find(':') != std::string_view::npos || value.ends_with('/')) {
        return false;
    }
    if (!required_prefix.empty() && !value.starts_with(required_prefix)) return false;
    std::size_t component_start = 0U;
    while (component_start < value.size()) {
        const std::size_t separator = value.find('/', component_start);
        const std::size_t component_end = separator == std::string_view::npos ? value.size() : separator;
        const auto component = value.substr(component_start, component_end - component_start);
        if (component.empty() || component == "." || component == "..") return false;
        component_start = separator == std::string_view::npos ? value.size() : separator + 1U;
    }
    return true;
}

core::Result<std::string> required_string(
    const std::vector<core::json::Member>& object,
    std::string_view name,
    std::size_t maximum) {
    const auto* member = core::json::find_member(object, name);
    if (member == nullptr) {
        return core::Result<std::string>::failure(validation("plugin manifest field is missing: " + std::string(name)));
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) return decoded;
    if (!printable_text(decoded.value(), maximum)) {
        return core::Result<std::string>::failure(invalid("plugin manifest string is empty, too long, or contains a control byte"));
    }
    return decoded;
}

core::Result<std::uint32_t> required_uint32(
    const std::vector<core::json::Member>& object,
    std::string_view name) {
    const auto* member = core::json::find_member(object, name);
    if (member == nullptr) {
        return core::Result<std::uint32_t>::failure(validation("plugin manifest integer field is missing: " + std::string(name)));
    }
    const auto parsed = core::json::parse_uint(member->raw_value);
    if (!parsed || parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(invalid("plugin manifest integer field is invalid: " + std::string(name)));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

core::Result<bool> required_bool(
    const std::vector<core::json::Member>& object,
    std::string_view name) {
    const auto* member = core::json::find_member(object, name);
    if (member == nullptr) {
        return core::Result<bool>::failure(validation("plugin manifest boolean field is missing: " + std::string(name)));
    }
    if (member->raw_value == "true") return core::Result<bool>::success(true);
    if (member->raw_value == "false") return core::Result<bool>::success(false);
    return core::Result<bool>::failure(invalid("plugin manifest boolean field is invalid: " + std::string(name)));
}

bool allowed_fields(
    const std::vector<core::json::Member>& object,
    std::initializer_list<std::string_view> allowed) {
    for (const auto& member : object) {
        if (std::find(allowed.begin(), allowed.end(), member.key) == allowed.end()) return false;
    }
    return true;
}

core::Result<std::vector<std::string>> string_array(
    std::string_view raw,
    std::string_view field,
    std::size_t maximum,
    bool require_non_empty,
    bool tokens) {
    if (raw.empty() || raw.front() != '[' || raw.back() != ']') {
        return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest array is invalid: " + std::string(field)));
    }
    std::vector<std::string> values;
    std::size_t cursor = 1U;
    const auto skip_space = [&]() {
        while (cursor + 1U < raw.size() &&
               (raw[cursor] == ' ' || raw[cursor] == '\n' || raw[cursor] == '\r' || raw[cursor] == '\t')) {
            ++cursor;
        }
    };
    skip_space();
    if (cursor + 1U == raw.size()) {
        if (require_non_empty) {
            return core::Result<std::vector<std::string>>::failure(validation("plugin manifest array must not be empty: " + std::string(field)));
        }
        return core::Result<std::vector<std::string>>::success(std::move(values));
    }
    while (cursor + 1U < raw.size()) {
        if (values.size() >= maximum || raw[cursor] != '"') {
            return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest array contains too many or non-string values: " + std::string(field)));
        }
        const std::size_t start = cursor;
        ++cursor;
        bool escaped = false;
        while (cursor < raw.size() - 1U) {
            const char byte = raw[cursor++];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                break;
            }
        }
        if (cursor > raw.size() - 1U || raw[cursor - 1U] != '"') {
            return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest string array is unterminated: " + std::string(field)));
        }
        const auto decoded = core::json::decode_string(raw.substr(start, cursor - start));
        if (!decoded || !printable_text(decoded.value(), kMaxTokenBytes)) {
            return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest string array contains an invalid string: " + std::string(field)));
        }
        if (tokens && !safe_token(decoded.value())) {
            return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest token array contains an invalid token: " + std::string(field)));
        }
        values.push_back(decoded.value());
        skip_space();
        if (cursor >= raw.size() - 1U) break;
        if (raw[cursor] != ',') {
            return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest string array separator is invalid: " + std::string(field)));
        }
        ++cursor;
        skip_space();
        if (cursor >= raw.size() - 1U || raw[cursor] == ']') {
            return core::Result<std::vector<std::string>>::failure(invalid("plugin manifest string array has a trailing separator: " + std::string(field)));
        }
    }
    if (values.empty() && require_non_empty) {
        return core::Result<std::vector<std::string>>::failure(validation("plugin manifest array must not be empty: " + std::string(field)));
    }
    return core::Result<std::vector<std::string>>::success(std::move(values));
}

core::Result<PlatformEntry> platform_entry(
    std::string_view raw,
    std::string_view field_prefix) {
    const auto object = core::json::parse_object(raw);
    if (!object) return core::Result<PlatformEntry>::failure(object.error());
    if (!allowed_fields(object.value(), {"entrypoint", "sha256"})) {
        return core::Result<PlatformEntry>::failure(validation("platform entry contains an unknown field"));
    }
    const auto entrypoint = required_string(object.value(), "entrypoint", 512U);
    const auto digest_text = required_string(object.value(), "sha256", 64U);
    if (!entrypoint || !digest_text) {
        return core::Result<PlatformEntry>::failure(!entrypoint ? entrypoint.error() : digest_text.error());
    }
    if (!safe_relative_path(entrypoint.value(), field_prefix)) {
        return core::Result<PlatformEntry>::failure(validation("platform entrypoint is not a safe package-relative path"));
    }
    const auto digest = assets::Sha256Digest::from_hex(digest_text.value());
    if (!digest) return core::Result<PlatformEntry>::failure(digest.error());
    return core::Result<PlatformEntry>::success(PlatformEntry{entrypoint.value(), digest.value()});
}

core::Result<std::map<std::string, PlatformEntry>> platform_map(
    std::string_view raw,
    std::string_view field_prefix) {
    const auto object = core::json::parse_object(raw);
    if (!object) return core::Result<std::map<std::string, PlatformEntry>>::failure(object.error());
    if (object.value().empty() || object.value().size() > kMaxPlatforms) {
        return core::Result<std::map<std::string, PlatformEntry>>::failure(validation("plugin manifest platform map is empty or too large"));
    }
    std::map<std::string, PlatformEntry> result;
    for (const auto& member : object.value()) {
        if (!safe_token(member.key)) {
            return core::Result<std::map<std::string, PlatformEntry>>::failure(invalid("plugin manifest platform key is invalid"));
        }
        const auto entry = platform_entry(member.raw_value, field_prefix);
        if (!entry) return core::Result<std::map<std::string, PlatformEntry>>::failure(entry.error());
        result.emplace(member.key, entry.value());
    }
    return core::Result<std::map<std::string, PlatformEntry>>::success(std::move(result));
}

core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> parse_version(
    std::string_view value,
    std::string_view field) {
    std::array<std::uint32_t, 3> parts{};
    std::size_t cursor = 0U;
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        const std::size_t end = value.find('.', cursor);
        const std::size_t stop = end == std::string_view::npos ? value.size() : end;
        if (stop == cursor) {
            return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("plugin manifest version is invalid: " + std::string(field)));
        }
        std::uint32_t parsed = 0U;
        const auto result = std::from_chars(value.data() + cursor, value.data() + stop, parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + stop) {
            return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("plugin manifest version is invalid: " + std::string(field)));
        }
        parts[index] = parsed;
        if (index + 1U == parts.size()) {
            if (end != std::string_view::npos) {
                return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("plugin manifest version has too many components: " + std::string(field)));
            }
        } else if (end == std::string_view::npos) {
            return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::failure(invalid("plugin manifest version has too few components: " + std::string(field)));
        }
        cursor = end == std::string_view::npos ? value.size() : end + 1U;
    }
    return core::Result<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>>::success(
        {parts[0], parts[1], parts[2]});
}

} // namespace

core::Result<PluginPackageManifest> parse_manifest(std::string_view json) {
    if (json.size() > 65536U) {
        return core::Result<PluginPackageManifest>::failure(validation("plugin manifest exceeds the size limit"));
    }
    const auto object = core::json::parse_object(json);
    if (!object) return core::Result<PluginPackageManifest>::failure(object.error());
    if (!allowed_fields(object.value(), {
        "schema", "plugin_id", "display_name", "publisher", "version", "cartographer_api",
        "min_cartographer_version", "trust_class", "capabilities", "permissions", "platforms", "vanta"})) {
        return core::Result<PluginPackageManifest>::failure(validation("plugin manifest contains an unknown field"));
    }

    PluginPackageManifest manifest;
    const auto schema = required_string(object.value(), "schema", kMaxTokenBytes);
    const auto plugin_id = required_string(object.value(), "plugin_id", kMaxTokenBytes);
    const auto display_name = required_string(object.value(), "display_name", kMaxDisplayBytes);
    const auto publisher = required_string(object.value(), "publisher", kMaxDisplayBytes);
    const auto version = required_string(object.value(), "version", kMaxTokenBytes);
    const auto minimum = required_string(object.value(), "min_cartographer_version", kMaxTokenBytes);
    const auto api = required_uint32(object.value(), "cartographer_api");
    const auto trust = required_string(object.value(), "trust_class", kMaxTokenBytes);
    if (!schema || !plugin_id || !display_name || !publisher || !version || !minimum || !api || !trust) {
        const auto& error = !schema ? schema.error() : !plugin_id ? plugin_id.error() :
            !display_name ? display_name.error() : !publisher ? publisher.error() :
            !version ? version.error() : !minimum ? minimum.error() : !api ? api.error() : trust.error();
        return core::Result<PluginPackageManifest>::failure(error);
    }
    if (schema.value() != "cartographer-plugin.v1" || !safe_token(plugin_id.value())) {
        return core::Result<PluginPackageManifest>::failure(validation("plugin manifest schema or plugin id is invalid"));
    }
    if (api.value() != plugin_protocol::Protocol::kApiVersion) {
        return core::Result<PluginPackageManifest>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "plugin manifest API version is unsupported"));
    }
    if (!parse_version(version.value(), "version") || !parse_version(minimum.value(), "min_cartographer_version")) {
        const auto parsed = parse_version(version.value(), "version");
        return core::Result<PluginPackageManifest>::failure(parsed ? parse_version(minimum.value(), "min_cartographer_version").error() : parsed.error());
    }
    if (trust.value() == "sandboxed_process") manifest.trust_class = TrustClass::sandboxed_process;
    else if (trust.value() == "trusted_native") manifest.trust_class = TrustClass::trusted_native;
    else if (trust.value() == "data_only") manifest.trust_class = TrustClass::data_only;
    else return core::Result<PluginPackageManifest>::failure(validation("plugin manifest trust class is invalid"));

    const auto* capabilities = core::json::find_member(object.value(), "capabilities");
    if (capabilities == nullptr) {
        return core::Result<PluginPackageManifest>::failure(validation("plugin manifest capabilities are missing"));
    }
    const auto parsed_capabilities = string_array(capabilities->raw_value, "capabilities", kMaxCapabilities, true, true);
    if (!parsed_capabilities) return core::Result<PluginPackageManifest>::failure(parsed_capabilities.error());
    manifest.capabilities = parsed_capabilities.value();
    if (std::set<std::string>(manifest.capabilities.begin(), manifest.capabilities.end()).size() != manifest.capabilities.size()) {
        return core::Result<PluginPackageManifest>::failure(validation("plugin manifest capabilities contain duplicates"));
    }

    const auto* permissions = core::json::find_member(object.value(), "permissions");
    if (permissions == nullptr) return core::Result<PluginPackageManifest>::failure(validation("plugin manifest permissions are missing"));
    const auto permission_object = core::json::parse_object(permissions->raw_value);
    if (!permission_object || !allowed_fields(permission_object.value(), {"network", "project_write_new_assets", "filesystem_inputs"})) {
        return core::Result<PluginPackageManifest>::failure(permission_object ? validation("plugin manifest permissions contain an unknown field") : permission_object.error());
    }
    const auto network = required_bool(permission_object.value(), "network");
    const auto write_assets = required_bool(permission_object.value(), "project_write_new_assets");
    const auto* filesystem = core::json::find_member(permission_object.value(), "filesystem_inputs");
    if (!network || !write_assets || filesystem == nullptr) {
        return core::Result<PluginPackageManifest>::failure(!network ? network.error() : !write_assets ? write_assets.error() : validation("plugin manifest filesystem_inputs is missing"));
    }
    const auto parsed_filesystem = string_array(filesystem->raw_value, "filesystem_inputs", kMaxFilesystemInputs, false, true);
    if (!parsed_filesystem) return core::Result<PluginPackageManifest>::failure(parsed_filesystem.error());
    manifest.permissions = PermissionSet{network.value(), write_assets.value(), parsed_filesystem.value()};
    if (manifest.permissions.network || manifest.permissions.project_write_new_assets) {
        return core::Result<PluginPackageManifest>::failure(validation("plugin package permissions require a host authority that is not available"));
    }

    const auto* platforms = core::json::find_member(object.value(), "platforms");
    if (platforms == nullptr) return core::Result<PluginPackageManifest>::failure(validation("plugin manifest platforms are missing"));
    if (manifest.trust_class == TrustClass::data_only) {
        const auto platform_object = core::json::parse_object(platforms->raw_value);
        if (!platform_object || !platform_object.value().empty()) {
            return core::Result<PluginPackageManifest>::failure(validation("data-only plugin packages must not declare executable platforms"));
        }
    } else {
        const auto parsed_platforms = platform_map(platforms->raw_value, "cartographer/");
        if (!parsed_platforms) return core::Result<PluginPackageManifest>::failure(parsed_platforms.error());
        manifest.platforms = parsed_platforms.value();
    }

    if (const auto* vanta = core::json::find_member(object.value(), "vanta"); vanta != nullptr) {
        const auto vanta_object = core::json::parse_object(vanta->raw_value);
        if (!vanta_object || !allowed_fields(vanta_object.value(), {"optional", "api", "platforms"})) {
            return core::Result<PluginPackageManifest>::failure(vanta_object ? validation("plugin manifest VANTA section contains an unknown field") : vanta_object.error());
        }
        const auto optional = required_bool(vanta_object.value(), "optional");
        const auto vanta_api = required_uint32(vanta_object.value(), "api");
        const auto* vanta_platforms = core::json::find_member(vanta_object.value(), "platforms");
        if (!optional || !vanta_api || vanta_platforms == nullptr) {
            return core::Result<PluginPackageManifest>::failure(!optional ? optional.error() : !vanta_api ? vanta_api.error() : validation("plugin manifest VANTA platforms are missing"));
        }
        VantaSection section;
        section.optional = optional.value();
        section.api = vanta_api.value();
        if (section.api == 0U) return core::Result<PluginPackageManifest>::failure(validation("plugin manifest VANTA API must be non-zero"));
        const auto parsed_vanta_platforms = platform_map(vanta_platforms->raw_value, "vanta/");
        if (!parsed_vanta_platforms) return core::Result<PluginPackageManifest>::failure(parsed_vanta_platforms.error());
        section.platforms = parsed_vanta_platforms.value();
        manifest.vanta = std::move(section);
    }

    manifest.schema = schema.value();
    manifest.plugin_id = plugin_id.value();
    manifest.display_name = display_name.value();
    manifest.publisher = publisher.value();
    manifest.version = version.value();
    manifest.cartographer_api = api.value();
    manifest.min_cartographer_version = minimum.value();
    return core::Result<PluginPackageManifest>::success(std::move(manifest));
}

core::Result<plugin_protocol::PluginManifest> to_plugin_manifest(
    const PluginPackageManifest& manifest) {
    const auto version = parse_version(manifest.version, "version");
    if (!version) return core::Result<plugin_protocol::PluginManifest>::failure(version.error());
    plugin_protocol::PluginManifest result{
        manifest.plugin_id,
        providers::Version{
            std::get<0>(version.value()), std::get<1>(version.value()), std::get<2>(version.value())},
        manifest.cartographer_api,
        manifest.capabilities,
        map_trust_class(manifest.trust_class),
        plugin_protocol::PermissionSet{
            manifest.permissions.network,
            manifest.permissions.project_write_new_assets,
            manifest.permissions.filesystem_inputs},
    };
    if (auto valid = plugin_protocol::Protocol::validate_manifest(result); !valid) {
        return core::Result<plugin_protocol::PluginManifest>::failure(valid.error());
    }
    return core::Result<plugin_protocol::PluginManifest>::success(std::move(result));
}

providers::TrustClass map_trust_class(TrustClass trust) noexcept {
    switch (trust) {
    case TrustClass::sandboxed_process: return providers::TrustClass::sandboxed_process;
    case TrustClass::trusted_native: return providers::TrustClass::trusted_in_process;
    case TrustClass::data_only: return providers::TrustClass::data_only;
    }
    return providers::TrustClass::data_only;
}

} // namespace carto::plugin_package
