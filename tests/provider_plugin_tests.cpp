#include <carto/io/builtin_provider.hpp>
#include <carto/plugin_protocol/process_runner.hpp>
#include <carto/plugin_protocol/protocol.hpp>
#include <carto/providers/registry.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

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

void builtin_gltf_export_provider_is_lifecycle_gated() {
    carto::providers::Registry registry;
    REQUIRE(carto::io::register_builtin_gltf_providers(registry));
    const auto exporter = registry.resolve("geometry.export.gltf");
    REQUIRE(exporter);
    REQUIRE(exporter.value().id == "cartographer.gltf.export");
    REQUIRE((exporter.value().version == carto::providers::Version{1U, 0U, 0U}));
    const auto binary_exporter = registry.resolve("geometry.export.glb");
    REQUIRE(binary_exporter);
    REQUIRE(binary_exporter.value().id == exporter.value().id);
    REQUIRE(binary_exporter.value().version == exporter.value().version);
    REQUIRE(!carto::io::register_builtin_gltf_providers(registry));
}

void builtin_polygon_and_triangle_export_providers_are_lifecycle_gated() {
    carto::providers::Registry registry;
    REQUIRE(carto::io::register_builtin_ply_providers(registry));
    REQUIRE(carto::io::register_builtin_stl_providers(registry));
    REQUIRE(registry.resolve("geometry.import.ply"));
    REQUIRE(registry.resolve("geometry.export.ply"));
    REQUIRE(registry.resolve("geometry.import.stl"));
    REQUIRE(registry.resolve("geometry.export.stl"));
}

void plugin_protocol_is_bounded_and_defaults_to_non_network_sandboxing() {
    const carto::plugin_protocol::PluginManifest manifest{
        "org.example.obj", {1U, 2U, 0U}, 1U, {"geometry.import.obj"},
        carto::providers::TrustClass::sandboxed_process, {},
        {},
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
    auto invalid_budget = manifest;
    invalid_budget.budget.max_wall_time_ms = 0U;
    REQUIRE(!carto::plugin_protocol::Protocol::validate_manifest(invalid_budget));
    REQUIRE(!carto::plugin_protocol::Protocol::validate_execution_budget(
        carto::plugin_protocol::ExecutionBudget{
            30'000U, 30'000U, carto::plugin_protocol::Protocol::kMaxFrameBytes + 1U}));

    const carto::plugin_protocol::Envelope envelope{
        "request-1", "geometry.import.obj", "import", "{\"source\":\"input.obj\"}",
    };
    REQUIRE(carto::plugin_protocol::Protocol::validate_admission(manifest, envelope));
    auto ungranted = envelope;
    ungranted.capability = "geometry.export.obj";
    REQUIRE(!carto::plugin_protocol::Protocol::validate_admission(manifest, ungranted));

    const carto::plugin_protocol::NeutralResult neutral{
        envelope.request_id, envelope.capability, "{\"vertices\":[]}", 7U};
    REQUIRE(carto::plugin_protocol::Protocol::validate_neutral_result(
        manifest, envelope, neutral, 7U));
    REQUIRE(!carto::plugin_protocol::Protocol::validate_neutral_result(
        manifest, envelope, neutral, 8U));
    auto replayed = neutral;
    replayed.request_id = "other-request";
    REQUIRE(!carto::plugin_protocol::Protocol::validate_neutral_result(
        manifest, envelope, replayed));
    auto non_object = neutral;
    non_object.payload_json = "[]";
    REQUIRE(!carto::plugin_protocol::Protocol::validate_neutral_result(
        manifest, envelope, non_object, 7U));
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

#if defined(_WIN32)
void provider_process_runner_contains_crashes_and_enforces_budgets(
    const std::filesystem::path& executable) {
    using carto::plugin_protocol::ExecutionBudget;
    using carto::plugin_protocol::ProcessRequest;

    const std::array<std::uint8_t, 7U> input{{0U, 1U, 2U, 0U, 0xfeU, 0xffU, 7U}};
    const ExecutionBudget normal_budget{2'000U, 1'000U, 1'024U};
    const auto echoed = carto::plugin_protocol::run_isolated_process(ProcessRequest{
        executable, {"--provider-child-echo"}, input, normal_budget});
    if (!echoed) throw TestFailure(echoed.error().message);
    REQUIRE(echoed.value().exit_code == 0U);
    if (echoed.value().bytes.size() != sizeof(DWORD) + input.size()) {
        throw TestFailure("provider echo returned " +
            std::to_string(echoed.value().bytes.size()) + " bytes");
    }
    DWORD child_process_id = 0U;
    std::memcpy(&child_process_id, echoed.value().bytes.data(), sizeof(child_process_id));
    REQUIRE(child_process_id != GetCurrentProcessId());
    REQUIRE(std::equal(input.begin(), input.end(),
        echoed.value().bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(DWORD))));

    const auto crashed = carto::plugin_protocol::run_isolated_process(ProcessRequest{
        executable, {"--provider-child-crash"}, {}, normal_budget});
    REQUIRE(!crashed);
    REQUIRE(GetCurrentProcessId() != child_process_id);

    const auto timed_out = carto::plugin_protocol::run_isolated_process(ProcessRequest{
        executable, {"--provider-child-sleep"}, {}, ExecutionBudget{100U, 1'000U, 64U}});
    REQUIRE(!timed_out);

    const auto cpu_limited = carto::plugin_protocol::run_isolated_process(ProcessRequest{
        executable, {"--provider-child-cpu"}, {}, ExecutionBudget{5'000U, 100U, 64U}});
    REQUIRE(!cpu_limited);

    const auto output_limited = carto::plugin_protocol::run_isolated_process(ProcessRequest{
        executable, {"--provider-child-spam"}, {}, ExecutionBudget{2'000U, 1'000U, 64U}});
    REQUIRE(!output_limited);
}
#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc != 2) {
        std::cerr << "usage: cartographer_provider_plugin_tests <process-child>\n";
        return EXIT_FAILURE;
    }
#endif
    try {
        provider_registry_preserves_lifecycle_and_capability_failures();
        builtin_obj_providers_use_the_registry_lifecycle();
        builtin_gltf_export_provider_is_lifecycle_gated();
        builtin_polygon_and_triangle_export_providers_are_lifecycle_gated();
        plugin_protocol_is_bounded_and_defaults_to_non_network_sandboxing();
#if defined(_WIN32)
        provider_process_runner_contains_crashes_and_enforces_budgets(argv[1]);
#endif
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
