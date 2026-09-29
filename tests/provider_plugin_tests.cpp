#include <carto/io/builtin_provider.hpp>
#include <carto/plugin_protocol/protocol.hpp>
#include <carto/providers/registry.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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

void provider_registry_preserves_lifecycle_and_capability_failures() {
    carto::providers::Registry registry;
    carto::providers::ProviderDescriptor descriptor{
        "cartographer.obj", carto::providers::ProviderKind::geometry_import,
        {"geometry.import.obj"}, {1U, 0U, 0U}, carto::providers::TrustClass::trusted_in_process,
    };
    REQUIRE(registry.discover(descriptor));
    REQUIRE(!registry.discover(descriptor));
    REQUIRE(registry.resolve("geometry.import.obj").error().code ==
            carto::core::ErrorCode::invalid_state);
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::ready).error().code ==
            carto::core::ErrorCode::validation_failed);
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::inspected));
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::registered));
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::verified));
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::ready));
    REQUIRE(registry.resolve("geometry.import.obj"));
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::load_failed));
    REQUIRE(registry.resolve("geometry.import.obj").error().code ==
            carto::core::ErrorCode::invalid_state);
    REQUIRE(registry.transition("cartographer.obj", carto::providers::ProviderState::discovered));
    REQUIRE(registry.resolve("missing.capability").error().code ==
            carto::core::ErrorCode::not_found);
}

void builtin_obj_providers_use_the_registry_lifecycle() {
    carto::providers::Registry registry;
    REQUIRE(carto::io::register_builtin_obj_providers(registry));
    const auto importer = registry.resolve("geometry.import.obj");
    REQUIRE(importer);
    REQUIRE(importer.value().id == "cartographer.obj.import");
    const auto exporter = registry.resolve("geometry.export.obj");
    REQUIRE(exporter);
    REQUIRE(exporter.value().id == "cartographer.obj.export");
    REQUIRE(!carto::io::register_builtin_obj_providers(registry));
}

void plugin_protocol_is_bounded_and_defaults_to_non_network_sandboxing() {
    const carto::plugin_protocol::PluginManifest manifest{
        "org.example.obj", {1U, 2U, 0U}, 1U, {"geometry.import.obj"},
        carto::providers::TrustClass::sandboxed_process, {},
    };
    REQUIRE(carto::plugin_protocol::Protocol::validate_manifest(manifest));
    auto network = manifest;
    network.permissions.network = true;
    REQUIRE(!carto::plugin_protocol::Protocol::validate_manifest(network));
    auto traversal = manifest;
    traversal.permissions.filesystem_inputs = {".."};
    REQUIRE(!carto::plugin_protocol::Protocol::validate_manifest(traversal));
    auto write = manifest;
    write.permissions.project_write_new_assets = true;
    REQUIRE(!carto::plugin_protocol::Protocol::validate_manifest(write));

    const carto::plugin_protocol::Envelope envelope{
        "request-1", "geometry.import.obj", "import", "{\"source\":\"input.obj\"}",
    };
    REQUIRE(carto::plugin_protocol::Protocol::validate_admission(manifest, envelope));
    auto ungranted = envelope;
    ungranted.capability = "geometry.export.obj";
    REQUIRE(!carto::plugin_protocol::Protocol::validate_admission(manifest, ungranted));
    const auto frame = carto::plugin_protocol::Protocol::encode(envelope);
    REQUIRE(frame);
    const auto decoded = carto::plugin_protocol::Protocol::decode(frame.value());
    REQUIRE(decoded);
    REQUIRE(decoded.value().capability == envelope.capability);
    REQUIRE(decoded.value().payload_json == envelope.payload_json);

    auto truncated = frame.value();
    truncated.pop_back();
    REQUIRE(!carto::plugin_protocol::Protocol::decode(truncated));
    const std::string oversized(4U * 1024U * 1024U, 'x');
    REQUIRE(!carto::plugin_protocol::Protocol::encode({
        "request-2", "geometry.import.obj", "import", "{" + oversized + "}"}));
    REQUIRE(!carto::plugin_protocol::Protocol::encode({
        "request-3", "geometry.import.obj", "import", "{\"source\": {"}));

    const std::string nested_request_id =
        "{\"protocol\":\"carto.plugin.v1\","
        "\"capability\":\"geometry.import.obj\",\"method\":\"import\","
        "\"payload\":{\"nested\":{\"request_id\":\"request-1\"}}}";
    std::vector<std::uint8_t> nested_frame(nested_request_id.size() + 4U);
    const auto nested_size = static_cast<std::uint32_t>(nested_request_id.size());
    nested_frame[0] = static_cast<std::uint8_t>(nested_size);
    nested_frame[1] = static_cast<std::uint8_t>(nested_size >> 8U);
    nested_frame[2] = static_cast<std::uint8_t>(nested_size >> 16U);
    nested_frame[3] = static_cast<std::uint8_t>(nested_size >> 24U);
    std::copy(nested_request_id.begin(), nested_request_id.end(), nested_frame.begin() + 4U);
    REQUIRE(!carto::plugin_protocol::Protocol::decode(nested_frame));
}

} // namespace

int main() {
    try {
        provider_registry_preserves_lifecycle_and_capability_failures();
        builtin_obj_providers_use_the_registry_lifecycle();
        plugin_protocol_is_bounded_and_defaults_to_non_network_sandboxing();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
