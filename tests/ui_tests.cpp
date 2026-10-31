#include <carto/ui/ui.hpp>
#include <carto/assets/blob_store.hpp>
#include <carto/journal/journal.hpp>
#include <carto/attributes/set.hpp>
#include <carto/connection/graph.hpp>
#include <carto/io/generated_mesh.hpp>
#include <carto/project/project.hpp>
#include <carto/runtime_api/client.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string(__func__) + ":" + std::to_string(__LINE__) + ": requirement failed: " + #condition); \
    } while (false)

bool same_positions(
    const std::vector<carto::core::Vec3d>& left,
    const std::vector<carto::core::Vec3d>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (left[index].x != right[index].x || left[index].y != right[index].y ||
            left[index].z != right[index].z) {
            return false;
        }
    }
    return true;
}

bool same_vec3(const carto::core::Vec3d& left, const carto::core::Vec3d& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void append_float(std::vector<std::uint8_t>& output, float value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

std::string encode_base64(const std::vector<std::uint8_t>& bytes) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t index = 0U; index < bytes.size(); index += 3U) {
        const std::uint32_t first = bytes[index];
        const std::uint32_t second = index + 1U < bytes.size() ? bytes[index + 1U] : 0U;
        const std::uint32_t third = index + 2U < bytes.size() ? bytes[index + 2U] : 0U;
        const std::uint32_t block = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(block >> 18U) & 63U]);
        output.push_back(alphabet[(block >> 12U) & 63U]);
        output.push_back(index + 1U < bytes.size() ? alphabet[(block >> 6U) & 63U] : '=');
        output.push_back(index + 2U < bytes.size() ? alphabet[block & 63U] : '=');
    }
    return output;
}

std::vector<std::uint8_t> botanical_gltf_fixture(
    std::string_view identity, std::uint64_t revision) {
    std::vector<std::uint8_t> binary;
    for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
             0.0F, 1.0F, 0.0F}) append_float(binary, value);
    for (int index = 0; index < 3; ++index) {
        append_float(binary, 0.0F);
        append_float(binary, 0.0F);
        append_float(binary, 1.0F);
    }
    append_u32(binary, 0U);
    append_u32(binary, 1U);
    append_u32(binary, 2U);

    const std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Cartographer\"},"
        "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
        "\"indices\":2,\"material\":0,\"mode\":4}]}],\"materials\":[{\"name\":\"Needles\"}],"
        "\"buffers\":[{\"byteLength\":" + std::to_string(binary.size()) +
        ",\"uri\":\"data:application/octet-stream;base64," + encode_base64(binary) + "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":12,\"target\":34963}],"
        "\"accessors\":[{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"byteOffset\":0,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"extras\":{\"cartographer\":{\"sourceIdentity\":\"" + std::string(identity) +
        "\",\"sourceRevision\":\"" + std::to_string(revision) + "\"}}}";
    return {json.begin(), json.end()};
}

std::vector<std::uint8_t> botanical_glb_fixture(
    std::string_view identity, std::uint64_t revision) {
    std::vector<std::uint8_t> binary;
    for (const float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
             0.0F, 1.0F, 0.0F}) append_float(binary, value);
    for (int index = 0; index < 3; ++index) {
        append_float(binary, 0.0F);
        append_float(binary, 0.0F);
        append_float(binary, 1.0F);
    }
    append_u32(binary, 0U);
    append_u32(binary, 1U);
    append_u32(binary, 2U);

    auto json_bytes = botanical_gltf_fixture(identity, revision);
    std::string json(json_bytes.begin(), json_bytes.end());
    const std::string uri_marker =
        ",\"uri\":\"data:application/octet-stream;base64,";
    const std::size_t uri_start = json.find(uri_marker);
    REQUIRE(uri_start != std::string::npos);
    const std::size_t uri_end = json.find('"', uri_start + uri_marker.size());
    REQUIRE(uri_end != std::string::npos);
    json.erase(uri_start, uri_end - uri_start + 1U);
    while ((json.size() & 3U) != 0U) json.push_back(' ');

    constexpr std::uint32_t kGlbMagic = 0x46546c67U;
    constexpr std::uint32_t kJsonChunkType = 0x4e4f534aU;
    constexpr std::uint32_t kBinChunkType = 0x004e4942U;
    const std::size_t total_length = 12U + 8U + json.size() + 8U + binary.size();
    std::vector<std::uint8_t> output;
    output.reserve(total_length);
    append_u32(output, kGlbMagic);
    append_u32(output, 2U);
    append_u32(output, static_cast<std::uint32_t>(total_length));
    append_u32(output, static_cast<std::uint32_t>(json.size()));
    append_u32(output, kJsonChunkType);
    output.insert(output.end(), json.begin(), json.end());
    append_u32(output, static_cast<std::uint32_t>(binary.size()));
    append_u32(output, kBinChunkType);
    output.insert(output.end(), binary.begin(), binary.end());
    REQUIRE(output.size() == total_length);
    return output;
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("ui-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> runtime_bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

std::string runtime_text(const std::vector<std::uint8_t>& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

class BotanicalPromotionTransport final : public carto::runtime_api::Transport {
public:
    BotanicalPromotionTransport(
        std::vector<std::uint8_t> output,
        carto::core::Revision source_revision,
        const carto::runtime_api::JobReceipt* replay_receipt = nullptr,
        std::string expected_request_id = {},
        std::string output_kind = std::string(
            carto::runtime_api::kBotanicalGltfOutputKind))
        : output_(std::move(output)), source_revision_(source_revision),
          output_digest_(carto::assets::sha256(output_).hex()),
          output_kind_(replay_receipt == nullptr
              ? std::move(output_kind) : replay_receipt->output_kind),
          job_id_(replay_receipt == nullptr ? "job-fixture-7" : replay_receipt->job_id),
          queued_status_("queued"),
          completed_status_(replay_receipt == nullptr ? "succeeded" : replay_receipt->status),
          source_artifact_id_(replay_receipt == nullptr
              ? "artifact-input-7" : replay_receipt->source_artifact_id),
          output_artifact_id_(replay_receipt == nullptr
              ? "artifact-output-7" : replay_receipt->output_artifact_id),
          expected_source_digest_(replay_receipt == nullptr
              ? std::string{} : replay_receipt->source_digest.hex()),
          expected_request_id_(std::move(expected_request_id)) {}

    std::vector<carto::runtime_api::Request> requests;

    [[nodiscard]] carto::core::Result<carto::runtime_api::Response> send(
        const carto::runtime_api::Config&,
        const carto::runtime_api::Request& request) override {
        requests.push_back(request);
        carto::runtime_api::Response response;
        response.status = 200U;
        if (request.method == "POST" && request.path.ends_with("/artifacts")) {
            input_digest_ = carto::assets::sha256(request.body).hex();
            const auto request_id_header = std::find_if(
                request.headers.begin(), request.headers.end(), [](const auto& header) {
                    return header.first == "x-cartographer-request-id";
                });
            if (!expected_source_digest_.empty() &&
                (expected_source_digest_ != input_digest_ ||
                 request_id_header == request.headers.end() ||
                 request_id_header->second != expected_request_id_ + ".input")) {
                return carto::core::Result<carto::runtime_api::Response>::failure(
                    carto::core::Diagnostic(carto::core::ErrorCode::validation_failed,
                        "replayed service receipt does not bind the uploaded source request"));
            }
            response.body = runtime_bytes(
                "{\"artifact\":{\"artifact_id\":\"" + source_artifact_id_ +
                "\",\"sha256\":\"" +
                input_digest_ +
                "\",\"kind\":\"botanical-source-v1\"},\"size_bytes\":" +
                std::to_string(request.body.size()) + "}");
        } else if (request.method == "POST" && request.path.ends_with("/jobs")) {
            const std::string body = runtime_text(request.body);
            const std::string revision_binding = "\"source_revision\":\"" +
                std::to_string(source_revision_.value()) + "\"";
            const std::string request_id_binding = "\"request_id\":\"" +
                expected_request_id_ + "\"";
            if (input_digest_.empty() || body.find(input_digest_) == std::string::npos ||
                body.find(revision_binding) == std::string::npos ||
                (!expected_request_id_.empty() &&
                    body.find(request_id_binding) == std::string::npos) ||
                body.find("\"operation\":\"carto.botanical.generate.v1\"") ==
                    std::string::npos) {
                return carto::core::Result<carto::runtime_api::Response>::failure(
                    carto::core::Diagnostic(carto::core::ErrorCode::validation_failed,
                        "job request did not preserve the source contract"));
            }
            response.body = runtime_bytes(
                "{\"job\":{\"job_id\":\"" + job_id_ + "\",\"status\":\"" +
                queued_status_ + "\","
                "\"request\":{\"operation\":\"carto.botanical.generate.v1\","
                "\"source_revision\":\"" + std::to_string(source_revision_.value()) +
                "\",\"input\":{\"artifact_id\":\"" + source_artifact_id_ +
                "\",\"sha256\":\"" +
                input_digest_ + "\",\"kind\":\"botanical-source-v1\"}}}}");
        } else if (request.method == "GET" &&
            request.path.ends_with("/jobs/" + job_id_)) {
            response.body = runtime_bytes(
                "{\"job\":{\"job_id\":\"" + job_id_ + "\",\"status\":\"" +
                completed_status_ + "\","
                "\"request\":{\"operation\":\"carto.botanical.generate.v1\","
                "\"source_revision\":\"" + std::to_string(source_revision_.value()) +
                "\",\"input\":{\"artifact_id\":\"" + source_artifact_id_ +
                "\",\"sha256\":\"" +
                input_digest_ + "\",\"kind\":\"botanical-source-v1\"}},"
                "\"output\":{\"artifact_id\":\"" + output_artifact_id_ +
                "\",\"sha256\":\"" +
                output_digest_ + "\",\"kind\":\"" + output_kind_ + "\"}}}");
        } else if (request.method == "GET" &&
            request.path.ends_with("/artifacts/" + output_artifact_id_)) {
            response.body = output_;
            response.headers.emplace_back("x-cartographer-content-sha256", output_digest_);
            response.headers.emplace_back("x-cartographer-artifact-kind", output_kind_);
        } else {
            return carto::core::Result<carto::runtime_api::Response>::failure(
                carto::core::Diagnostic(carto::core::ErrorCode::not_found,
                    "unexpected local fixture route"));
        }
        return carto::core::Result<carto::runtime_api::Response>::success(
            std::move(response));
    }

private:
    std::vector<std::uint8_t> output_;
    carto::core::Revision source_revision_;
    std::string input_digest_;
    std::string output_digest_;
    std::string output_kind_;
    std::string job_id_;
    std::string queued_status_;
    std::string completed_status_;
    std::string source_artifact_id_;
    std::string output_artifact_id_;
    std::string expected_source_digest_;
    std::string expected_request_id_;
};

void preferences_validate_shell_invariants() {
    carto::ui::UiPreferences preferences;
    REQUIRE(preferences.validate());

    preferences.visible_panels.clear();
    REQUIRE(!preferences.validate());

    preferences = {};
    preferences.visible_panels.push_back(carto::ui::Panel::viewport);
    preferences.visible_panels.push_back(carto::ui::Panel::viewport);
    REQUIRE(!preferences.validate());

    preferences = {};
    preferences.visible_panels.erase(
        std::find(preferences.visible_panels.begin(), preferences.visible_panels.end(),
                  carto::ui::Panel::viewport));
    REQUIRE(!preferences.validate());
}

void ui_routes_authoritative_actions_and_operation_lineage() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {2.0, 2.0, 2.0}}));
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");
    REQUIRE(snapshot.operations.size() == 1U);
    REQUIRE(snapshot.operations.back().operation_id == 1U);
    REQUIRE(snapshot.operations.back().revision_before == carto::core::Revision{});
    REQUIRE(snapshot.operations.back().revision_after > snapshot.operations.back().revision_before);
    REQUIRE(snapshot.operations.back().category == carto::ui::OperationCategory::committed_command);
    REQUIRE(snapshot.operations.back().action == "Create Box");
    REQUIRE(snapshot.operations.back().document_changed);

    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::face}));
    REQUIRE(ui.snapshot().operations.size() == 1U);

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.operations.size() == 3U);
    REQUIRE(snapshot.operations[1].category == carto::ui::OperationCategory::undo);
    REQUIRE(snapshot.operations[2].category == carto::ui::OperationCategory::redo);

    const auto object = snapshot.objects.front().object.id;
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::vertex_mode));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));
    carto::editor::ToolArguments arguments;
    arguments.position = {-1.25, -1.0, -1.0};
    REQUIRE(ui.dispatch(carto::application::InvokeToolAction{
        "mesh.set-vertex-position", arguments}));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::repeat_last_tool));
    REQUIRE(ui.snapshot().operations.back().action == "Invoke Tool: mesh.set-vertex-position");
}

void ui_routes_mesh_cleanup_review_without_implicit_acceptance() {
    using namespace carto;
    application::ApplicationSession session;
    ui::UiController controller(session);
    REQUIRE(controller.dispatch(application::CreatePlaneAction{"Cleanup plane", 1.0, 1.0}));
    const auto before = controller.snapshot();
    const auto object = before.objects.front().object.id;
    const auto diagnostics = controller.inspect_mesh_cleanup_issues(object);
    REQUIRE(diagnostics && diagnostics.value().unreferenced_vertices.empty());
    REQUIRE(diagnostics.value().project_generation == before.project_generation);
    REQUIRE(diagnostics.value().project_revision == before.project_revision);
    REQUIRE(controller.snapshot().operations.size() == before.operations.size());
    application::MeshCleanupOptions options;
    options.merge_distance = 1.01;
    auto discovered = controller.review_mesh_cleanup_issues(object, options);
    REQUIRE(discovered && discovered.value().candidates().size() == 4U);
    REQUIRE(controller.snapshot().project_revision == before.project_revision);
    discovered.value().cancel();
    auto review = controller.review_mesh_cleanup(object, {{1U}, {2U}}, options);
    REQUIRE(review && review.value().receipt().merged_vertices.size() == 1U);
    REQUIRE(controller.snapshot().project_revision == before.project_revision);
    REQUIRE(controller.snapshot().operations.size() == before.operations.size());
    REQUIRE(!controller.commit_mesh_cleanup(review.value(), ""));
    REQUIRE(review.value().active());
    REQUIRE(controller.snapshot().project_revision == before.project_revision);
    const auto committed = controller.commit_mesh_cleanup(review.value(), "Remove duplicate test seam");
    REQUIRE(committed && !review.value().active());
    const auto after = controller.snapshot();
    REQUIRE(after.operations.size() == before.operations.size() + 1U);
    REQUIRE(committed.value().source == application::OperationSource::human);
    REQUIRE(after.operations.back().category == ui::OperationCategory::committed_command);
    REQUIRE(after.operations.back().action.find("Remove duplicate test seam") != std::string::npos);
    REQUIRE(!controller.commit_mesh_cleanup(review.value(), "Replay"));
    REQUIRE(controller.snapshot().operations.size() == after.operations.size());
    REQUIRE(controller.handle_shortcut(ui::Shortcut::undo));
    REQUIRE(controller.handle_shortcut(ui::Shortcut::redo));
}

void ui_routes_connection_graph_cleanup_without_condition_authoring() {
    using namespace carto;
    application::ApplicationSession session;
    ui::UiController controller(session);
    REQUIRE(controller.dispatch(application::CreateBoxAction{
        "Connection host", {1.0, 1.0, 1.0}}));
    auto snapshot = controller.snapshot();
    REQUIRE(!snapshot.condition.present);
    REQUIRE(snapshot.objects.size() == 1U);

    connection::ConnectionPort port;
    port.owner = snapshot.objects.front().object.id;
    port.authoring.kind = world::AuthoringBindingKind::scene_object;
    port.authoring.identity = port.owner.value;
    port.local_frame.frame_identity = "service-port";
    port.local_frame.coordinate_convention = "carto.core/v1";
    port.compatibility_tags = {
        connection::compatibility::utility::water_25mm};
    const auto add_digest = session.connection_port_graph_digest();
    REQUIRE(add_digest);
    application::AddConnectionPortAction add;
    add.expected_project_generation = snapshot.project_generation;
    add.expected_project_revision = snapshot.project_revision;
    add.expected_graph_digest = add_digest.value();
    add.port = std::move(port);
    REQUIRE(controller.add_connection_port(std::move(add)));
    snapshot = controller.snapshot();
    REQUIRE(!snapshot.condition.present);
    REQUIRE(session.connection_port_graph().find_port(connection::PortId{1U}) != nullptr);

    const std::size_t history_before_remove = snapshot.operations.size();
    const auto remove_digest = session.connection_port_graph_digest();
    REQUIRE(remove_digest);
    application::RemoveConnectionPortAction remove;
    remove.expected_project_generation = snapshot.project_generation;
    remove.expected_project_revision = snapshot.project_revision;
    remove.expected_graph_digest = remove_digest.value();
    remove.port_id = connection::PortId{1U};
    REQUIRE(controller.remove_connection_port(std::move(remove)));
    snapshot = controller.snapshot();
    REQUIRE(!snapshot.condition.present);
    REQUIRE(session.connection_port_graph().empty());
    REQUIRE(snapshot.operations.size() == history_before_remove + 1U);
    REQUIRE(snapshot.operations.back().action == "Remove Connection Port");
    REQUIRE(controller.handle_shortcut(ui::Shortcut::undo));
    REQUIRE(session.connection_port_graph().find_port(connection::PortId{1U}) != nullptr);
    REQUIRE(controller.handle_shortcut(ui::Shortcut::redo));
    REQUIRE(session.connection_port_graph().empty());
}

void ui_routes_spatial_fields_through_the_human_command_boundary() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const carto::application::CreateSpatialFieldAction create{
        "field.ui.moisture", carto::spatial_field::FieldSemantic::moisture,
        {0.0, 0.0}, 1.0, 4U, 4U, 0.0, 1.0, 0.0};
    REQUIRE(!ui.submit_ai_proposal(create));
    REQUIRE(session.snapshot().spatial_fields.empty());
    REQUIRE(ui.dispatch(create));
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.spatial_fields.size() == 1U);
    REQUIRE(snapshot.spatial_fields.front().semantic ==
        carto::spatial_field::FieldSemantic::moisture);
    REQUIRE(snapshot.operations.back().category ==
        carto::ui::OperationCategory::committed_command);
    REQUIRE(snapshot.operations.back().action == "Create Spatial Field");

    carto::spatial_field::FieldBrush brush;
    brush.expected_revision = snapshot.spatial_fields.front().source_revision;
    brush.center_uv = {1.5, 1.5};
    brush.radius = 2.0;
    brush.value = 0.6;
    brush.strength = 1.0;
    REQUIRE(ui.dispatch(carto::application::PaintSpatialFieldAction{
        "field.ui.moisture", brush}));
    snapshot = ui.snapshot();
    const auto painted_revision = snapshot.spatial_fields.front().source_revision;
    if (painted_revision != carto::core::Revision{2U}) {
        throw TestFailure("spatial field paint expected source revision 2, found " +
            std::to_string(painted_revision.value()));
    }
    REQUIRE(snapshot.operations.back().action == "Paint Spatial Field");
}

void ui_distinguishes_unsaved_from_saved_state() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);

    auto snapshot = ui.snapshot();
    REQUIRE(!snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");

    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Unsaved"}));
    snapshot = ui.snapshot();
    REQUIRE(!snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");

    REQUIRE(!ui.dispatch(carto::application::SaveProjectAction{
        temp.path() / "missing-parent" / "failed.carto"}));
    snapshot = ui.snapshot();
    REQUIRE(!snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::unsaved);
    REQUIRE(snapshot.project_status_text == "Not saved");

    const auto path = temp.path() / "saved.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.project_path.has_value());
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::saved);
    REQUIRE(snapshot.project_status_text == "Saved");

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Modified", {1.0, 1.0, 1.0}}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.project_path.has_value());
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::modified);
    REQUIRE(snapshot.project_status_text == "Modified");
    REQUIRE(!ui.dispatch(carto::application::SaveProjectAction{
        temp.path() / "missing-parent" / "failed-again.carto"}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.project_path.has_value());
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.project_status == carto::ui::ProjectStatus::modified);
    REQUIRE(snapshot.project_status_text == "Modified");
}

void ui_rejects_unadmitted_ai_proposals() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const auto before = session.snapshot();

    const auto rejected = ui.submit_ai_proposal(
        carto::application::CreateBoxAction{"AI Box", {1.0, 1.0, 1.0}});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().objects.empty());
    REQUIRE(ui.snapshot().operations.empty());
    REQUIRE(!ui.snapshot().ui_problems.empty());

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Human Box", {1.0, 1.0, 1.0}}));
    REQUIRE(session.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().operations.size() == 1U);
}

void ui_rejects_raw_ai_material_mutations_but_admits_human_material_authoring() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const auto before = ui.snapshot();
    carto::render::StandardMaterial definition;
    const auto rejected = ui.submit_ai_proposal(
        carto::application::CreateMaterialAction{
            before.project_revision, "AI material", definition});
    REQUIRE(!rejected);
    REQUIRE(session.snapshot().project_revision == before.project_revision);
    REQUIRE(session.snapshot().materials.empty());

    const auto created = ui.dispatch(carto::application::CreateMaterialAction{
        before.project_revision, "Human material", definition});
    REQUIRE(created);
    const auto after = ui.snapshot();
    REQUIRE(after.materials.size() == 1U);
    REQUIRE(after.materials.front().name == "Human material");
    REQUIRE(after.operations.size() == 1U);
    REQUIRE(after.operations.back().action == "Create Material");
    REQUIRE(after.operations.back().revision_after > after.operations.back().revision_before);

    auto edited = after.materials.front().definition;
    edited.roughness = 0.37;
    const auto updated = ui.dispatch(carto::application::UpdateMaterialAction{
        after.project_revision, after.materials.front().id,
        after.materials.front().revision, edited});
    REQUIRE(updated);
    const auto after_update = ui.snapshot();
    REQUIRE(after_update.materials.front().definition.roughness == 0.37);
    REQUIRE(after_update.operations.size() == 2U);
    REQUIRE(after_update.operations.back().action == "Update Material");

    auto mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(mesh);
    const auto faces = mesh.value().faces_sorted();
    carto::attributes::AttributeSet attributes({
        {carto::attributes::AttributeDomain::face, faces.size()}});
    std::vector<carto::attributes::AttributeValue> regions(
        faces.size(), std::uint32_t{3U});
    REQUIRE(attributes.add_layer(carto::attributes::AttributeLayer{
        {"cartographer.material_slot", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32}, std::move(regions)}));
    REQUIRE(mesh.value().set_attribute_payload(std::move(attributes), {}));
    REQUIRE(ui.dispatch(carto::application::CreateMeshObjectAction{
        "Material target", std::move(mesh.value())}));
    const auto target = ui.snapshot();
    const auto mesh_asset = target.objects.front().object.mesh_asset;
    REQUIRE(mesh_asset.has_value());
    const carto::project::MaterialRegionKey region{*mesh_asset, 3U};

    const auto ai_assignment = ui.submit_ai_proposal(
        carto::application::SetMaterialRegionAssignmentAction{
            target.project_revision, region, after_update.materials.front().id,
            std::nullopt});
    REQUIRE(!ai_assignment);
    REQUIRE(ui.snapshot().material_assignments.empty());
    const auto human_assignment = ui.dispatch(
        carto::application::SetMaterialRegionAssignmentAction{
            target.project_revision, region, after_update.materials.front().id,
            std::nullopt});
    REQUIRE(human_assignment);
    const auto assigned = ui.snapshot();
    REQUIRE(assigned.material_assignments.size() == 1U);
    REQUIRE(assigned.material_assignments.front().key == region);
    REQUIRE(assigned.operations.back().action == "Set Material Region Assignment");

    carto::project::MaterialGraph graph;
    graph.identity = "materials/ui/galvanized-steel";
    graph.revision = carto::core::Revision{1U};
    carto::project::MaterialGraphNode color;
    color.id = 1U;
    color.kind = carto::project::MaterialGraphNodeKind::constant_color;
    color.value_type = carto::project::MaterialGraphValueType::color;
    color.constants = {0.4, 0.42, 0.44, 1.0};
    carto::project::MaterialGraphNode alpha;
    alpha.id = 2U;
    alpha.kind = carto::project::MaterialGraphNodeKind::parameter_scalar;
    alpha.value_type = carto::project::MaterialGraphValueType::scalar;
    alpha.parameter_name = "opacity";
    carto::project::MaterialGraphNode roughness;
    roughness.id = 3U;
    roughness.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    roughness.value_type = carto::project::MaterialGraphValueType::scalar;
    roughness.constants[0] = 0.6;
    carto::project::MaterialGraphNode metallic;
    metallic.id = 4U;
    metallic.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    metallic.value_type = carto::project::MaterialGraphValueType::scalar;
    metallic.constants[0] = 0.85;
    graph.nodes = {color, alpha, roughness, metallic};
    graph.outputs.base_color = color.id;
    graph.outputs.alpha = alpha.id;
    graph.outputs.roughness = roughness.id;
    graph.outputs.metallic = metallic.id;
    const auto ai_graph = ui.submit_ai_proposal(
        carto::application::SetMaterialGraphAction{
            assigned.project_revision, std::nullopt, std::nullopt, graph});
    REQUIRE(!ai_graph);
    REQUIRE(ui.snapshot().material_graphs.empty());
    const auto created_graph = ui.dispatch(carto::application::SetMaterialGraphAction{
        assigned.project_revision, std::nullopt, std::nullopt, graph});
    REQUIRE(created_graph);
    auto graph_snapshot = ui.snapshot();
    REQUIRE(graph_snapshot.material_graphs.size() == 1U);
    REQUIRE(graph_snapshot.material_graphs.front().identity == graph.identity);
    REQUIRE(graph_snapshot.material_graphs.front().node_count == 4U);
    REQUIRE(graph_snapshot.operations.back().action == "Set Material Graph");
    auto editable_graph = ui.material_graph_source(graph.identity);
    REQUIRE(editable_graph);
    const auto graph_digest = editable_graph.value().canonical_digest();
    REQUIRE(graph_digest);
    auto changed_graph = editable_graph.value();
    changed_graph.nodes.front().constants[0] = 0.55;
    changed_graph.revision = changed_graph.revision.next();
    REQUIRE(ui.dispatch(carto::application::SetMaterialGraphAction{
        graph_snapshot.project_revision, editable_graph.value().revision,
        graph_digest.value(), changed_graph}));
    graph_snapshot = ui.snapshot();
    REQUIRE(graph_snapshot.material_graphs.front().source_revision ==
        carto::core::Revision{2U});
    REQUIRE(graph_snapshot.operations.back().action == "Set Material Graph");
    editable_graph = ui.material_graph_source(graph.identity);
    REQUIRE(editable_graph && editable_graph.value().nodes.front().constants[0] == 0.55);

    const auto topology_digest = editable_graph.value().canonical_digest();
    REQUIRE(topology_digest);
    auto topology = editable_graph.value();
    carto::project::MaterialGraphNode color_add;
    color_add.id = 5U;
    color_add.kind = carto::project::MaterialGraphNodeKind::add;
    color_add.value_type = carto::project::MaterialGraphValueType::color;
    color_add.inputs = {1U, 1U};
    topology.nodes.push_back(color_add);
    topology.outputs.base_color = color_add.id;
    topology.revision = topology.revision.next();
    REQUIRE(ui.dispatch(carto::application::SetMaterialGraphAction{
        graph_snapshot.project_revision, editable_graph.value().revision,
        topology_digest.value(), topology}));
    graph_snapshot = ui.snapshot();
    REQUIRE(graph_snapshot.material_graphs.front().source_revision ==
        carto::core::Revision{3U});
    REQUIRE(graph_snapshot.material_graphs.front().node_count == 5U);
    editable_graph = ui.material_graph_source(graph.identity);
    REQUIRE(editable_graph && editable_graph.value().outputs.base_color == color_add.id);
    REQUIRE(graph_snapshot.operations.back().action == "Set Material Graph");

    const auto binding_digest = editable_graph.value().canonical_digest();
    REQUIRE(binding_digest && !graph_snapshot.materials.empty());
    const carto::application::SetMaterialGraphBindingAction graph_binding{
        graph_snapshot.project_revision, graph_snapshot.materials.front().id,
        graph_snapshot.materials.front().revision, graph.identity,
        editable_graph.value().revision, binding_digest.value()};
    REQUIRE(!ui.submit_ai_proposal(graph_binding));
    REQUIRE(ui.snapshot().material_graph_bindings.empty());
    REQUIRE(ui.dispatch(graph_binding));
    graph_snapshot = ui.snapshot();
    REQUIRE(graph_snapshot.material_graph_bindings.size() == 1U);
    REQUIRE(graph_snapshot.material_graph_bindings.front().source_current);
    REQUIRE(graph_snapshot.material_graph_bindings.front().graph_identity == graph.identity);
    REQUIRE(graph_snapshot.operations.back().action == "Bind Material Graph");

    carto::project::MaterialGraphEvaluationInputs persistent_inputs;
    carto::project::MaterialGraphValue opacity_value;
    opacity_value.type = carto::project::MaterialGraphValueType::scalar;
    opacity_value.components[0] = 0.42;
    persistent_inputs.parameters.emplace("opacity", opacity_value);
    const carto::application::SetMaterialGraphInputsAction set_inputs{
        graph_snapshot.project_revision, graph_snapshot.materials.front().id,
        graph_snapshot.materials.front().revision, graph.identity,
        editable_graph.value().revision, binding_digest.value(), persistent_inputs};
    REQUIRE(!ui.submit_ai_proposal(set_inputs));
    REQUIRE(ui.snapshot().material_graph_bindings.front().inputs.parameters.empty());
    REQUIRE(ui.dispatch(set_inputs));
    graph_snapshot = ui.snapshot();
    REQUIRE(graph_snapshot.material_graph_bindings.front().inputs == persistent_inputs);
    REQUIRE(graph_snapshot.materials.front().revision == carto::core::Revision{4U});
    REQUIRE(graph_snapshot.operations.back().action == "Set Material Graph Inputs");

    editable_graph = ui.material_graph_source(graph.identity);
    REQUIRE(editable_graph);
    const auto before_detached_add_digest = editable_graph.value().canonical_digest();
    REQUIRE(before_detached_add_digest);
    auto with_detached_node = editable_graph.value();
    carto::project::MaterialGraphNode detached_node;
    detached_node.id = 6U;
    detached_node.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    detached_node.value_type = carto::project::MaterialGraphValueType::scalar;
    detached_node.constants[0] = 0.3;
    with_detached_node.nodes.push_back(detached_node);
    with_detached_node.revision = with_detached_node.revision.next();
    REQUIRE(ui.dispatch(carto::application::SetMaterialGraphAction{
        graph_snapshot.project_revision, editable_graph.value().revision,
        before_detached_add_digest.value(), with_detached_node}));
    graph_snapshot = ui.snapshot();
    editable_graph = ui.material_graph_source(graph.identity);
    REQUIRE(editable_graph && !graph_snapshot.material_graph_bindings.front().source_current);
    const auto before_delete_digest = editable_graph.value().canonical_digest();
    REQUIRE(before_delete_digest);
    auto deleted_node_graph = editable_graph.value();
    deleted_node_graph.nodes.erase(std::remove_if(
        deleted_node_graph.nodes.begin(), deleted_node_graph.nodes.end(),
        [detached_node](const auto& node) { return node.id == detached_node.id; }),
        deleted_node_graph.nodes.end());
    deleted_node_graph.retired_node_ids.push_back(detached_node.id);
    deleted_node_graph.revision = deleted_node_graph.revision.next();
    REQUIRE(ui.dispatch(carto::application::SetMaterialGraphAction{
        graph_snapshot.project_revision, editable_graph.value().revision,
        before_delete_digest.value(), deleted_node_graph}));
    editable_graph = ui.material_graph_source(graph.identity);
    REQUIRE(editable_graph && editable_graph.value().retired_node_ids ==
        std::vector<std::uint64_t>{detached_node.id});
}

void ui_imports_and_binds_private_pathless_material_texture_sources() {
    TempDirectory temp;
    const auto project_root = temp.path() / "p";
    std::filesystem::create_directories(project_root);
    const auto project_path = project_root / "texture-workflow.carto";
    const auto source_path = temp.path() / "workstation-private-donor-identity.tga";
    const std::string private_tga_image_id = "PRIVATE-DONOR-IDENTITY";
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[0U] = static_cast<std::uint8_t>(private_tga_image_id.size());
    tga[2U] = 2U;
    tga[12U] = 2U;
    tga[14U] = 1U;
    tga[16U] = 24U;
    tga.insert(tga.end(), private_tga_image_id.begin(), private_tga_image_id.end());
    tga.insert(tga.end(), {0U, 0U, 255U, 0U, 255U, 0U});
    {
        std::ofstream source(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(source));
        source.write(reinterpret_cast<const char*>(tga.data()),
                     static_cast<std::streamsize>(tga.size()));
        REQUIRE(static_cast<bool>(source));
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const auto unsaved_revision = ui.snapshot().project_revision;
    REQUIRE(!ui.inspect_source_blob_store(
        ui.snapshot().project_generation, unsaved_revision));
    REQUIRE(!ui.import_material_texture(source_path));
    REQUIRE(ui.snapshot().project_revision == unsaved_revision);
    REQUIRE(ui.snapshot().texture_sources.empty());

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto malformed_path = temp.path() / "malformed.tga";
    {
        std::ofstream malformed(malformed_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(malformed));
        malformed << "not a texture";
    }
    const auto before_malformed = ui.snapshot();
    REQUIRE(!ui.import_material_texture(malformed_path));
    REQUIRE(ui.snapshot().project_revision == before_malformed.project_revision);
    REQUIRE(ui.snapshot().texture_sources.empty());
    REQUIRE(!std::filesystem::exists(project_root / ".carto-assets"));

    const std::string missing_blob_bytes = "absent texture blob";
    const auto missing_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(missing_blob_bytes.data()),
        missing_blob_bytes.size()));
    const auto before_missing_blob = ui.snapshot();
    const auto missing_blob = ui.dispatch(
        carto::application::ImportMaterialTextureSourceAction{
            before_missing_blob.project_revision, missing_digest,
            carto::assets::TextureSourceFormat::tga, 2U, 1U});
    REQUIRE(!missing_blob);
    REQUIRE(ui.snapshot().project_revision == before_missing_blob.project_revision);
    REQUIRE(ui.snapshot().texture_sources.empty());

    const auto tga_digest = carto::assets::sha256(tga);
    carto::assets::BlobStore blob_store(project_root / ".carto-assets" / "source-blobs");
    const auto staged_blob = blob_store.put(
        std::span<const std::uint8_t>(tga), "image/x-tga");
    REQUIRE(staged_blob && staged_blob.value().digest == tga_digest);
    const auto before_mismatched_metadata = ui.snapshot();
    const auto mismatched_metadata = ui.dispatch(
        carto::application::ImportMaterialTextureSourceAction{
            before_mismatched_metadata.project_revision, tga_digest,
            carto::assets::TextureSourceFormat::png, 2U, 1U});
    REQUIRE(!mismatched_metadata);
    REQUIRE(ui.snapshot().project_revision == before_mismatched_metadata.project_revision);
    REQUIRE(ui.snapshot().texture_sources.empty());

    const auto source_revision_before_import = ui.snapshot().project_revision;
    const auto imported = ui.import_material_texture(source_path);
    REQUIRE(imported);
    REQUIRE(imported.value().action == "Import Material Texture Source");
    REQUIRE(imported.value().source == carto::application::OperationSource::human);
    REQUIRE(imported.value().document_changed);
    const auto imported_snapshot = ui.snapshot();
    REQUIRE(imported_snapshot.operations.back().action == "Import Material Texture Source");
    REQUIRE(imported_snapshot.project_revision > source_revision_before_import);
    REQUIRE(imported_snapshot.texture_sources.size() == 1U);
    const auto texture = imported_snapshot.texture_sources.front();
    REQUIRE(texture.id != 0U);
    REQUIRE(texture.format == carto::assets::TextureSourceFormat::tga);
    REQUIRE(texture.width == 2U && texture.height == 1U);
    REQUIRE(texture.content_digest == tga_digest);
    REQUIRE(texture.relative_locator.starts_with(
        ".carto-assets/source-blobs/sha256/"));
    REQUIRE(texture.name.find("workstation-private-donor-identity") == std::string::npos);
    REQUIRE(texture.provenance == "cartographer.native_texture_import.v1");

    REQUIRE((project_root / texture.relative_locator).lexically_normal() ==
        blob_store.path_for(texture.content_digest).lexically_normal());
    const auto stored = blob_store.read(texture.content_digest);
    REQUIRE(stored);
    REQUIRE(stored.value() == tga);
    const auto unreferenced_blob = blob_store.put(
        "unreferenced texture workflow bytes", "application/octet-stream");
    REQUIRE(unreferenced_blob);
    const auto blob_inventory = ui.inspect_source_blob_store(
        imported_snapshot.project_generation, imported_snapshot.project_revision);
    REQUIRE(blob_inventory);
    REQUIRE(std::find(blob_inventory.value().orphan_candidates.begin(),
        blob_inventory.value().orphan_candidates.end(), unreferenced_blob.value().digest) !=
            blob_inventory.value().orphan_candidates.end());
    REQUIRE(std::find(blob_inventory.value().orphan_candidates.begin(),
        blob_inventory.value().orphan_candidates.end(), texture.content_digest) ==
            blob_inventory.value().orphan_candidates.end());
    const auto after_blob_inventory = ui.snapshot();
    REQUIRE(after_blob_inventory.project_revision == imported_snapshot.project_revision &&
        after_blob_inventory.undo_count == imported_snapshot.undo_count);
    const auto stale_blob_inventory = ui.inspect_source_blob_store(
        imported_snapshot.project_generation + 1U, imported_snapshot.project_revision);
    REQUIRE(!stale_blob_inventory && stale_blob_inventory.error().code ==
        carto::core::ErrorCode::stale_data);
    const auto stale_revision_inventory = ui.inspect_source_blob_store(
        imported_snapshot.project_generation, imported_snapshot.project_revision.next());
    REQUIRE(!stale_revision_inventory && stale_revision_inventory.error().code ==
        carto::core::ErrorCode::stale_data);

    const auto stale_action = ui.dispatch(
        carto::application::ImportMaterialTextureSourceAction{
            source_revision_before_import, texture.content_digest,
            texture.format, texture.width, texture.height});
    REQUIRE(!stale_action);
    REQUIRE(stale_action.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().texture_sources.size() == 1U);
    const auto duplicate_action = ui.dispatch(
        carto::application::ImportMaterialTextureSourceAction{
            imported_snapshot.project_revision, texture.content_digest,
            texture.format, texture.width, texture.height});
    REQUIRE(!duplicate_action);
    REQUIRE(ui.snapshot().project_revision == imported_snapshot.project_revision);
    REQUIRE(ui.snapshot().texture_sources.size() == 1U);
    const auto ai_attempt = ui.submit_ai_proposal(
        carto::application::ImportMaterialTextureSourceAction{
            imported_snapshot.project_revision, texture.content_digest,
            texture.format, texture.width, texture.height});
    REQUIRE(!ai_attempt);
    REQUIRE(ui.snapshot().project_revision == imported_snapshot.project_revision);
    REQUIRE(ui.snapshot().texture_sources.size() == 1U);

    const auto created_material = ui.dispatch(carto::application::CreateMaterialAction{
        imported_snapshot.project_revision, "Pathless texture material", {}});
    REQUIRE(created_material);
    const auto material_snapshot = ui.snapshot();
    REQUIRE(material_snapshot.materials.size() == 1U);
    auto textured_definition = material_snapshot.materials.front().definition;
    textured_definition.base_color.asset = texture.id;
    textured_definition.base_color.interpretation =
        carto::render::TextureInterpretation::srgb;
    const auto bound_material = ui.dispatch(carto::application::UpdateMaterialAction{
        material_snapshot.project_revision, material_snapshot.materials.front().id,
        material_snapshot.materials.front().revision, textured_definition});
    REQUIRE(bound_material);
    REQUIRE(ui.snapshot().materials.front().definition.base_color.asset == texture.id);

    const auto undo_material_edit = ui.handle_shortcut(carto::ui::Shortcut::undo);
    if (!undo_material_edit) throw TestFailure(
        "material-texture unbind undo failed: " + undo_material_edit.error().message);
    REQUIRE(ui.snapshot().materials.front().definition.base_color.asset == 0U);
    const auto undo_material_create = ui.handle_shortcut(carto::ui::Shortcut::undo);
    if (!undo_material_create) throw TestFailure(
        "material creation undo failed: " + undo_material_create.error().message);
    REQUIRE(ui.snapshot().materials.empty());
    const auto undo_texture_import = ui.handle_shortcut(carto::ui::Shortcut::undo);
    if (!undo_texture_import) throw TestFailure(
        "texture import undo failed: " + undo_texture_import.error().message);
    REQUIRE(ui.snapshot().texture_sources.empty());
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(ui.snapshot().texture_sources.size() == 1U);
    REQUIRE(ui.snapshot().texture_sources.front().id == texture.id);
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(ui.snapshot().materials.size() == 1U);
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(ui.snapshot().materials.front().definition.base_color.asset == texture.id);

    carto::project::MaterialGraph texture_graph;
    texture_graph.identity = "materials/texture-source-graph";
    texture_graph.revision = carto::core::Revision{1U};
    carto::project::MaterialGraphNode u;
    u.id = 1U;
    u.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    u.value_type = carto::project::MaterialGraphValueType::scalar;
    u.constants[0U] = 0.25;
    carto::project::MaterialGraphNode v = u;
    v.id = 2U;
    v.constants[0U] = 0.75;
    carto::project::MaterialGraphNode sampled;
    sampled.id = 3U;
    sampled.kind = carto::project::MaterialGraphNodeKind::texture_sample;
    sampled.value_type = carto::project::MaterialGraphValueType::color;
    sampled.texture_asset = texture.id;
    sampled.uv_set = 1U;
    sampled.inputs = {u.id, v.id};
    carto::project::MaterialGraphNode graph_alpha;
    graph_alpha.id = 4U;
    graph_alpha.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    graph_alpha.value_type = carto::project::MaterialGraphValueType::scalar;
    graph_alpha.constants[0U] = 1.0;
    carto::project::MaterialGraphNode graph_roughness;
    graph_roughness.id = 5U;
    graph_roughness.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    graph_roughness.value_type = carto::project::MaterialGraphValueType::scalar;
    graph_roughness.constants[0U] = 0.5;
    carto::project::MaterialGraphNode graph_metallic;
    graph_metallic.id = 6U;
    graph_metallic.kind = carto::project::MaterialGraphNodeKind::constant_scalar;
    graph_metallic.value_type = carto::project::MaterialGraphValueType::scalar;
    graph_metallic.constants[0U] = 0.0;
    texture_graph.nodes = {u, v, sampled, graph_alpha, graph_roughness, graph_metallic};
    texture_graph.outputs.base_color = sampled.id;
    texture_graph.outputs.alpha = graph_alpha.id;
    texture_graph.outputs.roughness = graph_roughness.id;
    texture_graph.outputs.metallic = graph_metallic.id;

    const auto before_texture_graph = ui.snapshot();
    auto foreign_texture_graph = texture_graph;
    foreign_texture_graph.nodes.at(2U).texture_asset =
        texture.id == 1U ? 2U : 1U;
    REQUIRE(!ui.dispatch(carto::application::SetMaterialGraphAction{
        before_texture_graph.project_revision, std::nullopt, std::nullopt,
        foreign_texture_graph}));
    REQUIRE(ui.snapshot().project_revision == before_texture_graph.project_revision);
    const auto created_texture_graph = ui.dispatch(
        carto::application::SetMaterialGraphAction{
            before_texture_graph.project_revision, std::nullopt, std::nullopt,
            texture_graph});
    REQUIRE(created_texture_graph && created_texture_graph.value().document_changed);
    auto saved_texture_graph = ui.material_graph_source(texture_graph.identity);
    REQUIRE((saved_texture_graph && saved_texture_graph.value().nodes.at(2U).texture_asset ==
        texture.id && saved_texture_graph.value().nodes.at(2U).inputs ==
        std::vector<std::uint64_t>{u.id, v.id}));

    const auto recovery = carto::application::ApplicationSession::inspect_recovery(project_path);
    REQUIRE(recovery);
    REQUIRE(!recovery.value().journal_entries.empty());
    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries);
    REQUIRE(std::any_of(journal_entries.value().begin(),
        journal_entries.value().end(), [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return entry.event_type == "cartographer.application_action" &&
                payload.find("operation=Import Material Texture Source") != std::string::npos;
        }));

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto serialized_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(serialized_project);
    const std::string serialized = serialized_project.value().serialize();
    REQUIRE(serialized.find(source_path.string()) == std::string::npos);
    REQUIRE(serialized.find(source_path.filename().string()) == std::string::npos);
    REQUIRE(serialized.find(private_tga_image_id) == std::string::npos);
    REQUIRE(serialized_project.value().material_catalog().textures().size() == 1U);
    REQUIRE(serialized_project.value().material_catalog().materials().begin()->second
        .definition.base_color.asset == texture.id);
    const auto& serialized_texture_graph = serialized_project.value()
        .material_catalog().material_graphs().at(texture_graph.identity);
    REQUIRE(serialized_texture_graph.nodes.at(2U).texture_asset == texture.id &&
        serialized_texture_graph.nodes.at(2U).uv_set == 1U);

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    const auto reopened = reopened_ui.snapshot();
    REQUIRE(reopened.texture_sources.size() == 1U);
    REQUIRE(reopened.texture_sources.front().content_digest == texture.content_digest);
    REQUIRE(reopened.materials.front().definition.base_color.asset == texture.id);
    const auto reopened_texture_graph = reopened_ui.material_graph_source(
        texture_graph.identity);
    REQUIRE(reopened_texture_graph &&
        reopened_texture_graph.value().nodes.at(2U).texture_asset == texture.id &&
        reopened_texture_graph.value().nodes.at(2U).uv_set == 1U);
    const auto reopened_blob = blob_store.read(texture.content_digest);
    REQUIRE(reopened_blob && reopened_blob.value() == tga);
}

void ui_reimports_material_texture_source_without_retargeting_bindings() {
    TempDirectory temp;
    const auto project_root = temp.path() / "saved-project";
    std::filesystem::create_directories(project_root);
    const auto project_path = project_root / "texture-reimport.carto";
    const auto first_path = temp.path() / "first-workstation-texture.tga";
    const auto other_path = temp.path() / "other-registered-texture.tga";
    const auto replacement_path = temp.path() / "replacement-workstation-texture.tga";
    const auto make_tga = [](std::uint8_t marker) {
        std::vector<std::uint8_t> bytes(18U, 0U);
        bytes[2U] = 2U;
        bytes[12U] = 2U;
        bytes[14U] = 1U;
        bytes[16U] = 24U;
        bytes.insert(bytes.end(), {marker, 0U, 255U, 0U, 255U, marker});
        return bytes;
    };
    const auto first_bytes = make_tga(0U);
    const auto other_bytes = make_tga(64U);
    const auto replacement_bytes = make_tga(192U);
    const auto write_file = [](const std::filesystem::path& path,
                               const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(output));
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        REQUIRE(static_cast<bool>(output));
    };
    write_file(first_path, first_bytes);
    write_file(other_path, other_bytes);
    write_file(replacement_path, replacement_bytes);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    REQUIRE(ui.import_material_texture(first_path));
    const auto first_snapshot = ui.snapshot();
    REQUIRE(first_snapshot.texture_sources.size() == 1U);
    const auto first_texture = first_snapshot.texture_sources.front();
    REQUIRE(ui.import_material_texture(other_path));
    REQUIRE(ui.snapshot().texture_sources.size() == 2U);

    const auto before_material = ui.snapshot();
    const auto created_material = ui.dispatch(carto::application::CreateMaterialAction{
        before_material.project_revision, "Stable texture binding", {}});
    REQUIRE(created_material);
    auto definition = ui.snapshot().materials.front().definition;
    definition.base_color.asset = first_texture.id;
    definition.base_color.interpretation =
        carto::render::TextureInterpretation::srgb;
    const auto before_binding = ui.snapshot();
    REQUIRE(ui.dispatch(carto::application::UpdateMaterialAction{
        before_binding.project_revision, before_binding.materials.front().id,
        before_binding.materials.front().revision, definition}));

    const auto before_reimport = ui.snapshot();
    const std::string absent_bytes = "unwritten replacement texture bytes";
    const auto absent_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(absent_bytes.data()), absent_bytes.size()));
    const auto replacement_digest = carto::assets::sha256(replacement_bytes);
    carto::assets::BlobStore blob_store(project_root / ".carto-assets" / "source-blobs");
    const auto staged_replacement = blob_store.put(
        std::span<const std::uint8_t>(replacement_bytes), "image/x-tga");
    REQUIRE(staged_replacement && staged_replacement.value().digest == replacement_digest);
    const auto missing_blob = ui.dispatch(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation, before_reimport.project_revision,
            first_texture.id, first_texture.revision, first_texture.content_digest,
            absent_digest, carto::assets::TextureSourceFormat::tga, 2U, 1U});
    REQUIRE(!missing_blob);
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);
    REQUIRE(ui.snapshot().undo_count == before_reimport.undo_count);
    const auto stale_source = ui.dispatch(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation, before_reimport.project_revision,
            first_texture.id, first_texture.revision.next(), first_texture.content_digest,
            absent_digest, carto::assets::TextureSourceFormat::tga, 2U, 1U});
    REQUIRE(!stale_source && stale_source.error().code == carto::core::ErrorCode::stale_data);
    const auto stale_generation = ui.dispatch(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation + 1U, before_reimport.project_revision,
            first_texture.id, first_texture.revision, first_texture.content_digest,
            absent_digest, carto::assets::TextureSourceFormat::tga, 2U, 1U});
    REQUIRE(!stale_generation &&
        stale_generation.error().code == carto::core::ErrorCode::stale_data);
    const auto stale_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(absent_bytes.data()), absent_bytes.size() - 1U));
    const auto wrong_source = ui.dispatch(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation, before_reimport.project_revision,
            first_texture.id, first_texture.revision, stale_digest,
            replacement_digest, carto::assets::TextureSourceFormat::tga, 2U, 1U});
    REQUIRE(!wrong_source && wrong_source.error().code == carto::core::ErrorCode::stale_data);
    const auto wrong_identity = ui.dispatch(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation, before_reimport.project_revision,
            first_texture.id + 99U, first_texture.revision, first_texture.content_digest,
            replacement_digest, carto::assets::TextureSourceFormat::tga, 2U, 1U});
    REQUIRE(!wrong_identity &&
        wrong_identity.error().code == carto::core::ErrorCode::not_found);
    const auto wrong_dimensions = ui.dispatch(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation, before_reimport.project_revision,
            first_texture.id, first_texture.revision, first_texture.content_digest,
            replacement_digest, carto::assets::TextureSourceFormat::tga, 3U, 1U});
    REQUIRE(!wrong_dimensions &&
        wrong_dimensions.error().code == carto::core::ErrorCode::stale_data);
    const auto malformed_path = temp.path() / "malformed-replacement.tga";
    {
        std::ofstream malformed(malformed_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(malformed));
        malformed << "not a texture";
    }
    REQUIRE(!ui.reimport_material_texture(first_texture.id, malformed_path));
    REQUIRE(!ui.reimport_material_texture(first_texture.id, first_path));
    REQUIRE(!ui.reimport_material_texture(first_texture.id, other_path));
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);
    REQUIRE(!ui.submit_ai_proposal(
        carto::application::ReimportMaterialTextureSourceAction{
            before_reimport.project_generation, before_reimport.project_revision,
            first_texture.id, first_texture.revision, first_texture.content_digest,
            absent_digest, carto::assets::TextureSourceFormat::tga, 2U, 1U}));
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);

    const auto reimported = ui.reimport_material_texture(
        first_texture.id, replacement_path);
    REQUIRE(reimported);
    REQUIRE(reimported.value().action == "Reimport Material Texture Source");
    REQUIRE(reimported.value().source == carto::application::OperationSource::human);
    REQUIRE(reimported.value().document_changed);
    const auto after_reimport = ui.snapshot();
    REQUIRE(after_reimport.operations.back().action == "Reimport Material Texture Source");
    const auto updated_texture = std::find_if(after_reimport.texture_sources.begin(),
        after_reimport.texture_sources.end(), [&first_texture](const auto& source_record) {
            return source_record.id == first_texture.id;
        });
    REQUIRE(updated_texture != after_reimport.texture_sources.end());
    REQUIRE(updated_texture->revision == first_texture.revision.next());
    REQUIRE(updated_texture->content_digest == replacement_digest);
    REQUIRE(updated_texture->content_digest != first_texture.content_digest);
    REQUIRE(updated_texture->name == first_texture.name);
    REQUIRE(updated_texture->provenance == first_texture.provenance);
    REQUIRE(updated_texture->relative_locator != first_texture.relative_locator);
    REQUIRE(after_reimport.materials.front().definition.base_color.asset == first_texture.id);

    const auto old_blob = blob_store.read(first_texture.content_digest);
    const auto current_blob = blob_store.read(updated_texture->content_digest);
    REQUIRE(old_blob && old_blob.value() == first_bytes);
    REQUIRE(current_blob && current_blob.value() == replacement_bytes);
    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries);
    REQUIRE(std::any_of(journal_entries.value().begin(), journal_entries.value().end(),
        [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return entry.event_type == "cartographer.application_action" &&
                payload.find("operation=Reimport Material Texture Source") !=
                    std::string::npos;
        }));

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.snapshot().texture_sources.front().content_digest == first_texture.content_digest);
    REQUIRE(ui.snapshot().materials.front().definition.base_color.asset == first_texture.id);
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(ui.snapshot().texture_sources.front().content_digest == updated_texture->content_digest);
    REQUIRE(ui.snapshot().materials.front().definition.base_color.asset == first_texture.id);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    const auto reopened = reopened_ui.snapshot();
    REQUIRE(reopened.texture_sources.front().id == first_texture.id);
    REQUIRE(reopened.texture_sources.front().revision == first_texture.revision.next());
    REQUIRE(reopened.texture_sources.front().content_digest == updated_texture->content_digest);
    REQUIRE(reopened.materials.front().definition.base_color.asset == first_texture.id);
    const auto reopened_blob = blob_store.read(reopened.texture_sources.front().content_digest);
    REQUIRE(reopened_blob && reopened_blob.value() == replacement_bytes);
}

void ui_imports_reference_images_as_project_bound_revisioned_planes() {
    TempDirectory temp;
    const auto project_root = temp.path() / "saved project";
    std::filesystem::create_directories(project_root);
    const auto project_path = project_root / "reference-image-workflow.carto";
    const auto first_path = temp.path() / "workstation-private-reference-image.tga";
    const auto second_path = temp.path() / "second-reference-image.tga";
    const std::string private_image_id = "PRIVATE-REFERENCE-IMAGE";
    std::vector<std::uint8_t> first_tga(18U, 0U);
    first_tga[0U] = static_cast<std::uint8_t>(private_image_id.size());
    first_tga[2U] = 2U;
    first_tga[12U] = 2U;
    first_tga[14U] = 1U;
    first_tga[16U] = 24U;
    first_tga.insert(first_tga.end(), private_image_id.begin(), private_image_id.end());
    first_tga.insert(first_tga.end(), {0U, 0U, 255U, 0U, 255U, 0U});
    {
        std::ofstream source(first_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(source));
        source.write(reinterpret_cast<const char*>(first_tga.data()),
                     static_cast<std::streamsize>(first_tga.size()));
        REQUIRE(static_cast<bool>(source));
    }
    auto second_tga = first_tga;
    second_tga.back() = 127U;
    {
        std::ofstream source(second_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(source));
        source.write(reinterpret_cast<const char*>(second_tga.data()),
                     static_cast<std::streamsize>(second_tga.size()));
        REQUIRE(static_cast<bool>(source));
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const auto unsaved = ui.snapshot();
    REQUIRE(!ui.import_reference_image(first_path));
    REQUIRE(ui.snapshot().project_revision == unsaved.project_revision);
    REQUIRE(ui.snapshot().source_assets.empty());
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().empty());
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));

    const auto malformed_path = temp.path() / "malformed-reference.png";
    {
        std::ofstream malformed(malformed_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(malformed));
        malformed << "not an image";
    }
    const auto before_malformed = ui.snapshot();
    REQUIRE(!ui.import_reference_image(malformed_path));
    REQUIRE(ui.snapshot().project_revision == before_malformed.project_revision);
    REQUIRE(ui.snapshot().source_assets.empty());
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().empty());
    REQUIRE(!std::filesystem::exists(project_root / ".carto-assets"));

    const auto imported = ui.import_reference_image(first_path);
    REQUIRE(imported);
    REQUIRE(imported.value().source == carto::application::OperationSource::human);
    REQUIRE(imported.value().document_changed);
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.source_assets.size() == 1U);
    REQUIRE(snapshot.presentation_catalog.reference_planes().size() == 1U);
    const auto first_digest = carto::assets::sha256(first_tga);
    const std::string first_identity = "reference.image." + first_digest.hex();
    const auto decode_revision = snapshot.project_revision;
    const auto exact_reference_source =
        ui.read_reference_image_source_bytes(first_identity);
    REQUIRE(exact_reference_source);
    REQUIRE(exact_reference_source.value() == first_tga);
    REQUIRE(!ui.read_reference_image_source_bytes("reference.image.missing"));
    REQUIRE(ui.snapshot().project_revision == decode_revision);
    const auto decoded_reference = ui.decode_reference_image_tga(first_identity);
    REQUIRE(decoded_reference);
    REQUIRE(decoded_reference.value().admission.stage ==
        carto::assets::TextureSourceStage::decoded);
    REQUIRE(decoded_reference.value().admission.source.digest == first_digest);
    REQUIRE(decoded_reference.value().admission.width == 2U);
    REQUIRE(decoded_reference.value().admission.height == 1U);
    REQUIRE(decoded_reference.value().pixels == std::vector<std::uint8_t>({
        255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U}));
    REQUIRE(!ui.decode_reference_image_tga("reference.image.missing"));
    REQUIRE(ui.snapshot().project_revision == decode_revision);
    const auto& first_plane = snapshot.presentation_catalog.reference_planes().at(first_identity);
    REQUIRE(first_plane.source_digest == first_digest);
    REQUIRE(first_plane.image_width_pixels == 2U);
    REQUIRE(first_plane.image_height_pixels == 1U);
    REQUIRE(first_plane.view == carto::project::ReferencePlaneView::perspective);
    REQUIRE(first_plane.visible);
    REQUIRE(!first_plane.locked);
    REQUIRE(first_plane.width_meters == 1.0);
    REQUIRE(snapshot.source_assets.front().identity == first_plane.source_asset_identity);
    REQUIRE(snapshot.source_assets.front().kind == carto::assets::SourceAssetKind::image);
    REQUIRE(snapshot.source_assets.front().source_generation == first_plane.source_generation);
    REQUIRE(snapshot.source_assets.front().source_digest == first_plane.source_digest);
    REQUIRE(snapshot.source_assets.front().relative_source_path == first_plane.source_locator);
    REQUIRE(first_plane.source_locator.find(first_path.filename().string()) == std::string::npos);
    carto::assets::BlobStore reference_blob_store(
        project_root / ".carto-assets" / "source-blobs");
    const auto unrelated_blob = reference_blob_store.put(
        "unreferenced reference workflow bytes", "application/octet-stream");
    REQUIRE(unrelated_blob);
    const auto reference_blob_inventory = ui.inspect_source_blob_store(
        snapshot.project_generation, snapshot.project_revision);
    REQUIRE(reference_blob_inventory);
    REQUIRE(std::find(reference_blob_inventory.value().orphan_candidates.begin(),
        reference_blob_inventory.value().orphan_candidates.end(), unrelated_blob.value().digest) !=
            reference_blob_inventory.value().orphan_candidates.end());
    REQUIRE(std::find(reference_blob_inventory.value().orphan_candidates.begin(),
        reference_blob_inventory.value().orphan_candidates.end(), first_plane.source_digest) ==
            reference_blob_inventory.value().orphan_candidates.end());

    const auto duplicate_revision = snapshot.project_revision;
    REQUIRE(!ui.import_reference_image(first_path));
    REQUIRE(ui.snapshot().project_revision == duplicate_revision);
    REQUIRE(ui.snapshot().source_assets.size() == 1U);
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().size() == 1U);

    REQUIRE(ui.import_reference_image(second_path));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.source_assets.size() == 2U);
    REQUIRE(snapshot.presentation_catalog.reference_planes().size() == 2U);
    REQUIRE(std::any_of(snapshot.source_assets.begin(), snapshot.source_assets.end(),
        [&first_digest](const auto& source_asset) {
            return source_asset.source_digest == first_digest;
        }));

    const auto second_digest = carto::assets::sha256(second_tga);
    const std::string second_identity = "reference.image." + second_digest.hex();
    const auto calibration_base = ui.snapshot();
    auto calibration_edit = calibration_base.presentation_catalog.reference_planes().at(
        second_identity);
    calibration_edit.calibration = carto::project::ReferenceImageCalibration{
        {0.0, 0.0}, {1.0, 0.0}, 0.75};
    calibration_edit.width_meters = 1.5;
    REQUIRE(ui.update_reference_image_plane(
        calibration_base.project_generation, calibration_base.project_revision,
        calibration_edit));
    snapshot = ui.snapshot();
    const auto& calibrated_plane =
        snapshot.presentation_catalog.reference_planes().at(second_identity);
    REQUIRE(calibrated_plane.calibration.has_value());
    REQUIRE(calibrated_plane.width_meters == 1.5);
    const auto invalid_calibration_base = snapshot;
    auto invalid_calibration = calibrated_plane;
    invalid_calibration.calibration->second_pixel =
        invalid_calibration.calibration->first_pixel;
    REQUIRE(!ui.update_reference_image_plane(
        invalid_calibration_base.project_generation,
        invalid_calibration_base.project_revision, invalid_calibration));
    REQUIRE(ui.snapshot().project_revision == invalid_calibration_base.project_revision);

    const auto edit_base = ui.snapshot();
    auto placement_edit = edit_base.presentation_catalog.reference_planes().at(first_identity);
    placement_edit.origin = {2.0, 3.0, 4.0};
    placement_edit.width_meters = 2.5;
    placement_edit.opacity = 0.65;
    placement_edit.visible = false;
    placement_edit.locked = true;
    const auto edited = ui.update_reference_image_plane(
        edit_base.project_generation, edit_base.project_revision, placement_edit);
    REQUIRE(edited);
    REQUIRE(edited.value().source == carto::application::OperationSource::human);
    const auto after_edit = ui.snapshot();
    const auto& updated_plane =
        after_edit.presentation_catalog.reference_planes().at(first_identity);
    REQUIRE(updated_plane.revision.value() == placement_edit.revision.value() + 1U);
    REQUIRE(same_vec3(updated_plane.origin, placement_edit.origin));
    REQUIRE(updated_plane.width_meters == placement_edit.width_meters);
    REQUIRE(updated_plane.opacity == placement_edit.opacity);
    REQUIRE(!updated_plane.visible);
    REQUIRE(updated_plane.locked);
    REQUIRE(updated_plane.source_digest == first_digest);
    REQUIRE(updated_plane.source_locator == placement_edit.source_locator);

    auto stale_edit = placement_edit;
    stale_edit.opacity = 0.4;
    const auto stale_edit_result = ui.update_reference_image_plane(
        edit_base.project_generation, edit_base.project_revision, stale_edit);
    REQUIRE(!stale_edit_result);
    REQUIRE(ui.snapshot().project_revision == after_edit.project_revision);
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().at(first_identity).opacity ==
        placement_edit.opacity);

    auto locked_placement_edit = updated_plane;
    locked_placement_edit.origin.x += 1.0;
    const auto locked_edit_result = ui.update_reference_image_plane(
        after_edit.project_generation, after_edit.project_revision,
        locked_placement_edit);
    REQUIRE(!locked_edit_result);
    REQUIRE(ui.snapshot().project_revision == after_edit.project_revision);
    REQUIRE(same_vec3(
        ui.snapshot().presentation_catalog.reference_planes().at(first_identity).origin,
        placement_edit.origin));

    auto source_rebind = updated_plane;
    source_rebind.source_locator = "References/rebound.png";
    const auto source_rebind_result = ui.update_reference_image_plane(
        after_edit.project_generation, after_edit.project_revision, source_rebind);
    REQUIRE(!source_rebind_result);
    REQUIRE(ui.snapshot().project_revision == after_edit.project_revision);

    auto locked_display_edit = updated_plane;
    locked_display_edit.opacity = 0.5;
    locked_display_edit.visible = true;
    REQUIRE(ui.update_reference_image_plane(
        after_edit.project_generation, after_edit.project_revision,
        locked_display_edit));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.presentation_catalog.reference_planes().at(first_identity).opacity == 0.5);
    REQUIRE(snapshot.presentation_catalog.reference_planes().at(first_identity).visible);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto saved = carto::project::ProjectDocument::load(project_path);
    REQUIRE(saved);
    const std::string serialized = saved.value().serialize();
    REQUIRE(serialized.find(first_path.string()) == std::string::npos);
    REQUIRE(serialized.find(first_path.filename().string()) == std::string::npos);
    REQUIRE(serialized.find(private_image_id) == std::string::npos);
    REQUIRE(saved.value().presentation_catalog().reference_planes().size() == 2U);
    REQUIRE(saved.value().source_asset_registry().records().size() == 2U);
    REQUIRE(same_vec3(
        saved.value().presentation_catalog().reference_planes().at(first_identity).origin,
        placement_edit.origin));
    REQUIRE(saved.value().presentation_catalog().reference_planes().at(first_identity).opacity ==
        0.5);
    REQUIRE(saved.value().presentation_catalog().reference_planes().at(second_identity)
        .calibration.has_value());
    REQUIRE(saved.value().presentation_catalog().reference_planes().at(second_identity)
        .width_meters == 1.5);
    const auto bytes = carto::project::read_reference_image_source_bytes(
        project_root, saved.value().presentation_catalog().reference_planes().at(first_identity),
        saved.value().source_asset_registry());
    REQUIRE(bytes && bytes.value() == first_tga);

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    REQUIRE(reopened_ui.snapshot().source_assets.size() == 2U);
    REQUIRE(reopened_ui.snapshot().presentation_catalog.reference_planes().size() == 2U);
}

void ui_edits_reference_image_view_sets_atomically() {
    TempDirectory temp;
    const auto project_path = temp.path() / "reference-views.carto";
    const auto source_path = temp.path() / "reference.tga";
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[2U] = 2U;
    tga[12U] = 2U;
    tga[14U] = 1U;
    tga[16U] = 24U;
    tga.insert(tga.end(), {0U, 0U, 255U, 0U, 255U, 0U});
    {
        std::ofstream output(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(output));
        output.write(reinterpret_cast<const char*>(tga.data()),
            static_cast<std::streamsize>(tga.size()));
        REQUIRE(static_cast<bool>(output));
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    REQUIRE(ui.import_reference_image(source_path));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "View camera anchor", {1.0, 1.0, 1.0}}));
    auto snapshot = ui.snapshot();
    const auto camera_object = snapshot.objects.front().object.id;
    const double quarter_turn = std::acos(-1.0) * 0.25;
    const carto::core::Transform camera_transform{
        {4.0, 2.0, -6.0},
        {0.0, std::sin(quarter_turn), 0.0, std::cos(quarter_turn)},
        {1.0, 1.0, 1.0}};
    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        camera_object, camera_transform, snapshot.project_revision}));
    snapshot = ui.snapshot();
    const auto source_plane = snapshot.presentation_catalog.reference_planes().begin()->second;

    constexpr std::array<const char*, 3U> kPlaneNames{"front", "side", "top"};
    constexpr std::array<carto::project::ReferencePlaneView, 3U> kViews{
        carto::project::ReferencePlaneView::front,
        carto::project::ReferencePlaneView::side,
        carto::project::ReferencePlaneView::top};
    constexpr std::array<carto::project::ReferencePlaneView, 3U> kPerspectiveViews{
        carto::project::ReferencePlaneView::perspective,
        carto::project::ReferencePlaneView::perspective,
        carto::project::ReferencePlaneView::perspective};
    constexpr std::array<carto::project::ReferencePlaneView, 3U> kCameraPinnedViews{
        carto::project::ReferencePlaneView::camera_pinned,
        carto::project::ReferencePlaneView::camera_pinned,
        carto::project::ReferencePlaneView::camera_pinned};
    constexpr std::string_view kInitialSet = "reference.orthos.initial";
    std::array<std::string, 3U> plane_identities;
    auto catalog = snapshot.presentation_catalog;
    for (std::size_t index = 0U; index < kPlaneNames.size(); ++index) {
        auto plane = source_plane;
        plane.identity = "reference.image." + std::string(kPlaneNames[index]);
        plane.view = kViews[index];
        plane.view_set_identity = std::string(kInitialSet);
        plane.pinned_camera.reset();
        plane.locked = false;
        plane.revision = carto::core::Revision{1U};
        plane_identities[index] = plane.identity;
        REQUIRE(catalog.insert_reference_plane(std::move(plane)));
    }
    carto::project::AuthoredCamera camera;
    camera.object = camera_object;
    camera.identity = "reference.review-camera";
    camera.bookmark_identity = "reference-review";
    camera.reference_image_identity = plane_identities.front();
    camera.review_purpose = carto::project::CameraReviewPurpose::silhouette_review;
    camera.vertical_fov_radians = 0.82;
    camera.near_plane = 0.05;
    camera.far_plane = 800.0;
    camera.revision = carto::core::Revision{1U};
    REQUIRE(catalog.insert_camera(camera));
    const auto seeded_views = ui.dispatch(carto::application::SetPresentationCatalogAction{
        snapshot.project_generation, snapshot.project_revision, std::move(catalog)});
    if (!seeded_views) std::cerr << seeded_views.error().message << '\n';
    REQUIRE(seeded_views);

    const auto view_edits = [&](const std::array<carto::project::ReferencePlaneView, 3U>& views,
                                std::string view_set,
                                std::optional<carto::scene::ObjectId> pinned_camera) {
        std::vector<carto::ui::ReferenceImagePlaneViewEdit> edits;
        for (std::size_t index = 0U; index < kViews.size(); ++index) {
            edits.push_back({plane_identities[index], views[index], view_set, pinned_camera});
        }
        return edits;
    };
    snapshot = ui.snapshot();
    const auto renamed = ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision,
        view_edits(kViews, "reference.orthos.renamed", std::nullopt));
    REQUIRE(renamed);
    auto after_rename = ui.snapshot();
    REQUIRE(after_rename.project_revision.value() == snapshot.project_revision.value() + 1U);
    for (const auto& identity : plane_identities) {
        const auto& plane = after_rename.presentation_catalog.reference_planes().at(identity);
        REQUIRE(plane.view_set_identity == "reference.orthos.renamed");
        REQUIRE(plane.revision.value() == 2U);
    }
    const auto& front_basis = after_rename.presentation_catalog.reference_planes().at(
        plane_identities[0]);
    const auto& side_basis = after_rename.presentation_catalog.reference_planes().at(
        plane_identities[1]);
    const auto& top_basis = after_rename.presentation_catalog.reference_planes().at(
        plane_identities[2]);
    REQUIRE(front_basis.normal.x == 0.0 && front_basis.normal.y == 0.0 &&
            front_basis.normal.z == 1.0 && front_basis.up.x == 0.0 &&
            front_basis.up.y == 1.0 && front_basis.up.z == 0.0);
    REQUIRE(side_basis.normal.x == 1.0 && side_basis.normal.y == 0.0 &&
            side_basis.normal.z == 0.0 && side_basis.up.x == 0.0 &&
            side_basis.up.y == 1.0 && side_basis.up.z == 0.0);
    REQUIRE(top_basis.normal.x == 0.0 && top_basis.normal.y == -1.0 &&
            top_basis.normal.z == 0.0 && top_basis.up.x == 0.0 &&
            top_basis.up.y == 0.0 && top_basis.up.z == 1.0);

    const auto incomplete_base = after_rename;
    REQUIRE(!ui.update_reference_image_plane_views(
        incomplete_base.project_generation, incomplete_base.project_revision,
        {{plane_identities[0], carto::project::ReferencePlaneView::perspective,
          {}, std::nullopt}}));
    REQUIRE(ui.snapshot().project_revision == incomplete_base.project_revision);
    auto duplicate_roles = std::vector<carto::ui::ReferenceImagePlaneViewEdit>{
        {plane_identities[0], carto::project::ReferencePlaneView::front,
         "reference.orthos.renamed", std::nullopt},
        {plane_identities[1], carto::project::ReferencePlaneView::front,
         "reference.orthos.renamed", std::nullopt},
        {plane_identities[2], carto::project::ReferencePlaneView::top,
         "reference.orthos.renamed", std::nullopt}};
    REQUIRE(!ui.update_reference_image_plane_views(
        incomplete_base.project_generation, incomplete_base.project_revision,
        std::move(duplicate_roles)));
    REQUIRE(ui.snapshot().project_revision == incomplete_base.project_revision);
    REQUIRE(!ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision,
        view_edits(kViews, "reference.orthos.stale", std::nullopt)));
    REQUIRE(ui.snapshot().project_revision == incomplete_base.project_revision);

    auto front = after_rename.presentation_catalog.reference_planes().at(
        plane_identities[0]);
    front.locked = true;
    REQUIRE(ui.update_reference_image_plane(
        after_rename.project_generation, after_rename.project_revision, front));
    auto locked_snapshot = ui.snapshot();
    REQUIRE(!ui.update_reference_image_plane_views(
        locked_snapshot.project_generation, locked_snapshot.project_revision,
        view_edits(kViews, "reference.orthos.locked", std::nullopt)));
    REQUIRE(ui.snapshot().project_revision == locked_snapshot.project_revision);
    front = ui.snapshot().presentation_catalog.reference_planes().at(plane_identities[0]);
    front.locked = false;
    REQUIRE(ui.update_reference_image_plane(
        locked_snapshot.project_generation, locked_snapshot.project_revision, front));

    snapshot = ui.snapshot();
    REQUIRE(ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision,
        view_edits(kPerspectiveViews, {}, std::nullopt)));
    snapshot = ui.snapshot();
    std::map<std::string, carto::project::ReferenceImagePlane> world_poses_before_pin;
    for (const auto& identity : plane_identities) {
        world_poses_before_pin.emplace(identity,
            snapshot.presentation_catalog.reference_planes().at(identity));
    }
    const auto invalid_camera_base = snapshot;
    REQUIRE(!ui.update_reference_image_plane_views(
        invalid_camera_base.project_generation, invalid_camera_base.project_revision,
        view_edits(kCameraPinnedViews, {}, carto::scene::ObjectId{999'999U})));
    REQUIRE(ui.snapshot().project_revision == invalid_camera_base.project_revision);
    REQUIRE(ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision,
        view_edits(kCameraPinnedViews, {}, camera.object)));
    snapshot = ui.snapshot();
    for (const auto& identity : plane_identities) {
        const auto& plane = snapshot.presentation_catalog.reference_planes().at(identity);
        REQUIRE(plane.view == carto::project::ReferencePlaneView::camera_pinned);
        REQUIRE(plane.pinned_camera == camera.object);
        REQUIRE(plane.view_set_identity.empty());
        REQUIRE(plane.camera_local_placement);
    }
    auto legacy_pin = snapshot.presentation_catalog.reference_planes().at(
        plane_identities.front());
    const auto& legacy_world_pose = world_poses_before_pin.at(plane_identities.front());
    legacy_pin.origin = legacy_world_pose.origin;
    legacy_pin.normal = legacy_world_pose.normal;
    legacy_pin.up = legacy_world_pose.up;
    legacy_pin.camera_local_placement = false;
    REQUIRE(ui.update_reference_image_plane(
        snapshot.project_generation, snapshot.project_revision, legacy_pin));
    snapshot = ui.snapshot();
    REQUIRE(!snapshot.presentation_catalog.reference_planes().at(
        plane_identities.front()).camera_local_placement);
    REQUIRE(ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision,
        view_edits(kCameraPinnedViews, {}, camera.object)));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.presentation_catalog.reference_planes().at(
        plane_identities.front()).camera_local_placement);
    REQUIRE(ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision,
        view_edits(kPerspectiveViews, {}, std::nullopt)));
    snapshot = ui.snapshot();
    for (const auto& identity : plane_identities) {
        const auto& expected = world_poses_before_pin.at(identity);
        const auto& actual = snapshot.presentation_catalog.reference_planes().at(identity);
        REQUIRE((actual.origin - expected.origin).length() < 1.0e-9);
        REQUIRE((actual.normal - expected.normal).length() < 1.0e-9);
        REQUIRE((actual.up - expected.up).length() < 1.0e-9);
    }
    std::vector<carto::ui::ReferenceImagePlaneViewEdit> created_set;
    for (std::size_t index = 0U; index < kViews.size(); ++index) {
        created_set.push_back({plane_identities[index], kViews[index],
            "reference.orthos.created", std::nullopt});
    }
    REQUIRE(ui.update_reference_image_plane_views(
        snapshot.project_generation, snapshot.project_revision, std::move(created_set)));
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().at(plane_identities[0])
        .view_set_identity == "reference.orthos.created");
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().at(plane_identities[0])
        .view == carto::project::ReferencePlaneView::perspective);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().presentation_catalog.reference_planes().at(plane_identities[0])
        .view_set_identity == "reference.orthos.created");
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto saved = carto::project::ProjectDocument::load(project_path);
    REQUIRE(saved);
    for (std::size_t index = 0U; index < kViews.size(); ++index) {
        const auto& plane = saved.value().presentation_catalog().reference_planes().at(
            plane_identities[index]);
        REQUIRE(plane.view == kViews[index]);
        REQUIRE(plane.view_set_identity == "reference.orthos.created");
    }
}

void ui_routes_edge_selection_without_authoring_mutation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Box", {1.0, 1.0, 1.0}}));
    const auto before = ui.snapshot();
    REQUIRE(before.objects.size() == 1U);
    const auto object = before.objects.front().object.id;
    const auto instances = before.viewport.scene.instances();
    REQUIRE(instances.size() == 1U);
    REQUIRE(instances.front().mesh->triangle_edges.front().front().has_value());
    const auto edge = *instances.front().mesh->triangle_edges.front().front();
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
    REQUIRE(ui.dispatch(carto::application::SelectEdgeAction{object, edge}));
    const auto after = ui.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.objects.size() == before.objects.size());
    REQUIRE(after.selection.mode == carto::editor::SelectionMode::edge);
    REQUIRE(after.selection.edges.size() == 1U);
    REQUIRE(after.selection.edges.front() == edge);
    REQUIRE(after.ui_problems.empty());
}

void ui_routes_revision_bound_sweep_authoring_and_refuses_stale_inputs() {
    TempDirectory temp;
    const auto project_path = temp.path() / "sweep-ui.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Sweep UI"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Port owner", {1.0, 1.0, 1.0}}));
    const auto port_owner = ui.snapshot().objects.front().object.id;

    carto::connection::ConnectionPort endpoint_port;
    endpoint_port.id = carto::connection::PortId{7001U};
    endpoint_port.owner = port_owner;
    endpoint_port.authoring.kind = carto::world::AuthoringBindingKind::scene_object;
    endpoint_port.authoring.identity = port_owner.value;
    endpoint_port.local_frame.frame_identity = "ui-sweep-end";
    endpoint_port.local_frame.coordinate_convention = "carto.core/v1";
    endpoint_port.local_frame.transform_to_parent.translation = {3.0, 0.0, 0.0};
    endpoint_port.local_frame.transform_to_parent.rotation =
        {0.0, 0.7071067811865475, 0.0, 0.7071067811865476};
    carto::connection::ConnectionPortGraph endpoint_graph;
    REQUIRE(endpoint_graph.insert_port(endpoint_port));
    const auto graph_base = ui.snapshot();
    const auto empty_graph_digest = session.connection_port_graph_digest();
    REQUIRE(empty_graph_digest);
    REQUIRE(ui.dispatch(carto::application::ReplaceConnectionPortGraphAction{
        graph_base.project_generation, graph_base.project_revision,
        empty_graph_digest.value(), endpoint_graph}));

    carto::geometry::CurvePath centerline;
    centerline.source_identity = "ui.sweep.centerline";
    centerline.source_revision = carto::core::Revision{1U};
    centerline.segments.push_back({
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0}, {3.0, 0.0, 0.0}});
    carto::project::RoadSource road;
    road.source_identity = "ui.sweep.path-owner";
    road.source_revision = carto::core::Revision{1U};
    road.centerline_identity = centerline.source_identity;
    road.centerline_revision = centerline.source_revision;
    road.width_meters = 4.0;
    REQUIRE(ui.dispatch(carto::application::SetRoadAuthoringAction{
        ui.snapshot().project_revision, centerline, road}));

    carto::geometry::SweepSource source;
    source.source_identity = "ui.sweep.pipe";
    source.centerline_identity = centerline.source_identity;
    source.centerline_revision = centerline.source_revision;
    source.reference_up = {0.0, 1.0, 0.0};
    source.profile = {{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}};
    source.samples_per_segment = 4U;
    const auto next_revision = session.next_sweep_source_revision(source.source_identity);
    REQUIRE(next_revision && next_revision.value() == carto::core::Revision{1U});
    source.source_revision = next_revision.value();
    REQUIRE(ui.dispatch(carto::application::SetSweepSourceAction{
        ui.snapshot().project_revision, source}));

    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.sweep_sources.size() == 1U);
    REQUIRE(snapshot.sweep_sources.front().source.source_identity == source.source_identity);
    REQUIRE(snapshot.sweep_sources.front().source.centerline_identity == centerline.source_identity);
    REQUIRE(snapshot.sweep_sources.front().source.centerline_revision == centerline.source_revision);

    carto::condition::SweepEndpointSpatialPolicy spatial_policy;
    spatial_policy.port_outward_axis_local = {0.0, 0.0, 1.0};
    spatial_policy.maximum_position_error = 0.001;
    spatial_policy.minimum_axis_alignment = 0.99;
    const carto::condition::SweepEndpointPortBinding endpoint_binding{
        source.source_identity, source.source_revision,
        carto::condition::SweepEndpointSide::end, endpoint_port.id};
    const auto before_spatial_review = ui.snapshot();
    const auto spatial_review = ui.inspect_sweep_endpoint_spatial_fit(
        endpoint_binding, spatial_policy,
        snapshot.project_generation, snapshot.project_revision);
    REQUIRE(spatial_review && spatial_review.value().validate() &&
        spatial_review.value().aligned && spatial_review.value().position_error == 0.0);
    const auto after_spatial_review = ui.snapshot();
    REQUIRE(after_spatial_review.project_revision == before_spatial_review.project_revision &&
        after_spatial_review.undo_count == before_spatial_review.undo_count &&
        after_spatial_review.dirty == before_spatial_review.dirty);
    const auto stale_spatial_review = ui.inspect_sweep_endpoint_spatial_fit(
        endpoint_binding, spatial_policy,
        graph_base.project_generation, graph_base.project_revision);
    REQUIRE(!stale_spatial_review &&
        stale_spatial_review.error().code == carto::core::ErrorCode::stale_data);

    const auto before_preview = session.snapshot();
    const auto preview = session.sweep_source_preview(source.source_identity);
    REQUIRE(preview && preview.value().mesh.validate());
    REQUIRE(preview.value().source_identity == source.source_identity);
    REQUIRE(preview.value().source_revision == source.source_revision);
    const auto after_preview = session.snapshot();
    REQUIRE(after_preview.project_revision == before_preview.project_revision);
    REQUIRE(after_preview.undo_count == before_preview.undo_count);
    REQUIRE(after_preview.dirty == before_preview.dirty);

    auto invalid_recipe = source;
    invalid_recipe.source_revision = session.next_sweep_source_revision(
        source.source_identity).value();
    invalid_recipe.profile.clear();
    const auto before_invalid = session.snapshot();
    REQUIRE(!ui.dispatch(carto::application::SetSweepSourceAction{
        snapshot.project_revision, invalid_recipe}));
    const auto after_invalid = session.snapshot();
    REQUIRE(after_invalid.project_revision == before_invalid.project_revision);
    REQUIRE(after_invalid.undo_count == before_invalid.undo_count);
    REQUIRE(after_invalid.sweep_sources.size() == before_invalid.sweep_sources.size());

    auto stale_binding = source;
    stale_binding.source_revision = session.next_sweep_source_revision(
        source.source_identity).value();
    stale_binding.centerline_revision = carto::core::Revision{2U};
    const auto before_stale = session.snapshot();
    REQUIRE(!ui.dispatch(carto::application::SetSweepSourceAction{
        before_stale.project_revision, stale_binding}));
    const auto after_stale = session.snapshot();
    REQUIRE(after_stale.project_revision == before_stale.project_revision);
    REQUIRE(after_stale.undo_count == before_stale.undo_count);
    REQUIRE(after_stale.sweep_sources.front().digest ==
        before_stale.sweep_sources.front().digest);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().sweep_sources.empty());
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    snapshot = ui.snapshot();
    REQUIRE(snapshot.sweep_sources.size() == 1U);
    REQUIRE(snapshot.sweep_sources.front().source.centerline_revision == centerline.source_revision);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened_ui(reopened_session);
    REQUIRE(reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    const auto reopened = reopened_ui.snapshot();
    REQUIRE(reopened.sweep_sources.size() == 1U);
    REQUIRE(reopened.sweep_sources.front().source.source_identity == source.source_identity);
    REQUIRE(reopened.sweep_sources.front().source.centerline_identity == centerline.source_identity);
    REQUIRE(reopened.sweep_sources.front().source.centerline_revision == centerline.source_revision);

    auto hollow = reopened.sweep_sources.front().source;
    hollow.source_revision = reopened_session.next_sweep_source_revision(hollow.source_identity).value();
    hollow.inner_profile_scale = 0.5;
    const carto::journal::Journal history(std::filesystem::path(project_path.string() + ".journal"));
    const auto before_hollow = history.read_all();
    REQUIRE(before_hollow);
    const auto authored_hollow = reopened_ui.dispatch(carto::application::SetSweepSourceAction{
        reopened.project_revision, hollow});
    REQUIRE(authored_hollow && authored_hollow.value().document_changed &&
        authored_hollow.value().source == carto::application::OperationSource::human);
    const auto after_hollow = reopened_ui.snapshot();
    REQUIRE(after_hollow.sweep_sources.front().source.inner_profile_scale == 0.5);
    const auto hollow_events = history.read_all();
    REQUIRE(hollow_events && hollow_events.value().size() == before_hollow.value().size() + 1U);
    const std::string hollow_payload(hollow_events.value().back().payload.begin(),
        hollow_events.value().back().payload.end());
    REQUIRE(hollow_payload.find("CARTOGRAPHER_SWEEP_SOURCE 2") != std::string::npos &&
        hollow_payload.find("INNER_SCALE 0.5") != std::string::npos && history.verify());
    const auto hollow_preview = reopened_session.sweep_source_preview(hollow.source_identity);
    REQUIRE(hollow_preview && hollow_preview.value().mesh.vertex_count() == 40U &&
        hollow_preview.value().mesh.face_count() == 40U);
    REQUIRE(reopened_ui.snapshot().project_revision == after_hollow.project_revision &&
        history.read_all().value().back().entry_hash == hollow_events.value().back().entry_hash);

    auto invalid_hollow = hollow;
    invalid_hollow.source_revision = reopened_session.next_sweep_source_revision(hollow.source_identity).value();
    invalid_hollow.inner_profile_scale = 1.0;
    REQUIRE(!reopened_ui.dispatch(carto::application::SetSweepSourceAction{
        after_hollow.project_revision, invalid_hollow}));
    REQUIRE(reopened_ui.snapshot().project_revision == after_hollow.project_revision &&
        reopened_ui.snapshot().undo_count == after_hollow.undo_count &&
        history.read_all().value().back().entry_hash == hollow_events.value().back().entry_hash);
    auto valid_unapproved_hollow = invalid_hollow;
    valid_unapproved_hollow.inner_profile_scale = 0.4;
    REQUIRE(valid_unapproved_hollow.validate());
    REQUIRE(!reopened_ui.submit_ai_proposal(carto::application::SetSweepSourceAction{
        after_hollow.project_revision, valid_unapproved_hollow}));
    const auto stale_hollow = reopened_ui.dispatch(carto::application::SetSweepSourceAction{
        reopened.project_revision, valid_unapproved_hollow});
    REQUIRE(!stale_hollow && stale_hollow.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(reopened_ui.snapshot().project_revision == after_hollow.project_revision &&
        reopened_ui.snapshot().undo_count == after_hollow.undo_count &&
        history.read_all().value().back().entry_hash == hollow_events.value().back().entry_hash);
    REQUIRE(reopened_ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(reopened_ui.snapshot().sweep_sources.front().source.inner_profile_scale == 0.0);
    REQUIRE(reopened_ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(reopened_ui.snapshot().sweep_sources.front().source.inner_profile_scale == 0.5);
    REQUIRE(history.read_all().value().size() == hollow_events.value().size() + 2U && history.verify());
    REQUIRE(reopened_ui.dispatch(carto::application::SaveProjectAction{project_path}));
    carto::application::ApplicationSession hollow_reopened_session;
    carto::ui::UiController hollow_reopened_ui(hollow_reopened_session);
    REQUIRE(hollow_reopened_ui.dispatch(carto::application::OpenProjectAction{project_path}));
    REQUIRE(hollow_reopened_ui.snapshot().sweep_sources.front().source.inner_profile_scale == 0.5);
    const auto persisted_preview = hollow_reopened_session.sweep_source_preview(hollow.source_identity);
    REQUIRE(persisted_preview && persisted_preview.value().mesh.vertex_count() == 40U &&
        persisted_preview.value().mesh.face_count() == 40U);
}

void ui_preserves_required_viewport_and_supports_mode_theme_density() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(!ui.set_panel_visible(carto::ui::Panel::viewport, false));
    REQUIRE(ui.set_density(carto::ui::Density::compact));
    REQUIRE(ui.set_theme(carto::ui::Theme::dark));
    const auto dark_snapshot = ui.snapshot();
    REQUIRE(dark_snapshot.theme == carto::ui::Theme::dark);
    REQUIRE(dark_snapshot.colors.canvas_0.red < dark_snapshot.colors.text_primary.red);
    REQUIRE(ui.set_theme(carto::ui::Theme::light));
    const auto light_snapshot = ui.snapshot();
    REQUIRE(light_snapshot.theme == carto::ui::Theme::light);
    REQUIRE(light_snapshot.colors.canvas_0.red > light_snapshot.colors.text_primary.red);
    REQUIRE(dark_snapshot.colors.canvas_0.red != light_snapshot.colors.canvas_0.red);
    REQUIRE(ui.set_theme(carto::ui::Theme::high_contrast));
    REQUIRE(ui.set_operator_mode(carto::ui::OperatorMode::ai_first));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));
    const auto snapshot = ui.snapshot();
    REQUIRE(snapshot.density == carto::ui::Density::compact);
    REQUIRE(snapshot.theme == carto::ui::Theme::high_contrast);
    REQUIRE(snapshot.operator_mode == carto::ui::OperatorMode::ai_first);
    REQUIRE(snapshot.workspace_kind == carto::ui::Workspace::ai);
    REQUIRE(snapshot.ai_available);
    REQUIRE(snapshot.ai_auto_approve);
    REQUIRE(snapshot.ai_status.find("Native planner") != std::string::npos);
    REQUIRE(std::find(snapshot.visible_panels.begin(), snapshot.visible_panels.end(),
                      carto::ui::Panel::viewport) != snapshot.visible_panels.end());
}

void cartographer_brand_palette_is_stable() {
    const auto dark = carto::ui::color_tokens(carto::ui::Theme::dark);
    REQUIRE(dark.canvas_0.red == 0.031F);
    REQUIRE(dark.canvas_0.green == 0.059F);
    REQUIRE(dark.canvas_0.blue == 0.078F);
    REQUIRE(dark.accent_primary.red == 0.145F);
    REQUIRE(dark.accent_primary.green == 0.741F);
    REQUIRE(dark.accent_primary.blue == 0.914F);
    REQUIRE(dark.accent_secondary.red == 0.831F);
    REQUIRE(dark.accent_secondary.green == 0.651F);
    REQUIRE(dark.accent_secondary.blue == 0.471F);

    const auto light = carto::ui::color_tokens(carto::ui::Theme::light);
    REQUIRE(light.canvas_0.red == 0.941F);
    REQUIRE(light.canvas_0.green == 0.953F);
    REQUIRE(light.canvas_0.blue == 0.965F);
    REQUIRE(light.accent_primary.blue > light.accent_primary.red);
    REQUIRE(light.accent_secondary.red > light.accent_secondary.blue);
}

void ui_persists_preferences_and_resets_corrupt_state() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.set_density(carto::ui::Density::touch));
    REQUIRE(ui.set_theme(carto::ui::Theme::light));
    REQUIRE(ui.set_bottom_panel(carto::ui::BottomPanel::problems));
    REQUIRE(ui.set_ai_auto_approve(false));
    const auto path = temp.path() / "workspace.prefs";
    REQUIRE(ui.save_preferences(path));

    carto::application::ApplicationSession reopened_session;
    carto::ui::UiController reopened(reopened_session);
    REQUIRE(reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().density == carto::ui::Density::touch);
    REQUIRE(reopened.snapshot().theme == carto::ui::Theme::light);
    REQUIRE(reopened.snapshot().bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(!reopened.snapshot().ai_auto_approve);
    REQUIRE(reopened.set_panel_visible(carto::ui::Panel::scene, false));
    auto reopened_application_snapshot = reopened_session.snapshot();
    REQUIRE(std::find(reopened_application_snapshot.workspace.visible_panes.begin(),
                      reopened_application_snapshot.workspace.visible_panes.end(),
                      carto::application::Pane::outliner) ==
            reopened_application_snapshot.workspace.visible_panes.end());

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "header=CARTOGRAPHER_UI_PREFS_V1\n"
                  "operator_mode=person-first\n"
                  "workspace=model\n"
                  "density=standard\n"
                  "theme=dark\n"
                  "bottom_panel=operations\n"
                  "panels=scene,viewport,inspector,operations,problems\n";
    }
    REQUIRE(reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().ai_auto_approve);

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "header=CARTOGRAPHER_UI_PREFS_V1\nworkspace=unknown\n";
    }
    REQUIRE(!reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().workspace_kind == carto::ui::Workspace::model);
    REQUIRE(reopened.snapshot().density == carto::ui::Density::standard);
    reopened_application_snapshot = reopened_session.snapshot();
    REQUIRE(std::find(reopened_application_snapshot.workspace.visible_panes.begin(),
                      reopened_application_snapshot.workspace.visible_panes.end(),
                      carto::application::Pane::outliner) !=
            reopened_application_snapshot.workspace.visible_panes.end());
    REQUIRE(!reopened.snapshot().ui_problems.empty());

    {
        std::ofstream oversized(path, std::ios::binary | std::ios::trunc);
        oversized << std::string(17U * 1024U, 'x');
    }
    REQUIRE(!reopened.load_preferences(path));
    REQUIRE(reopened.snapshot().workspace_kind == carto::ui::Workspace::model);
    REQUIRE(reopened.snapshot().density == carto::ui::Density::standard);
    REQUIRE(reopened.snapshot().theme == carto::ui::Theme::dark);
    REQUIRE(!reopened.snapshot().ui_problems.empty());
}

void workbench_preferences_round_trip_and_fail_closed() {
    TempDirectory temp;
    const auto path = temp.path() / "workbench.prefs";
    carto::ui::WorkbenchPreferences preferences;
    preferences.ledger_collapsed = true;
    preferences.flow_use_counts[2] = 7U;
    preferences.last_flow_index = 2;
    auto& scene = preferences.instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene));
    scene.visible = true;
    scene.positioned = true;
    scene.x = 142;
    scene.y = 156;
    scene.width = 286;
    scene.height = 330;
    auto& surface_pane = preferences.panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::work_surface));
    surface_pane.visible = true;
    surface_pane.positioned = true;
    surface_pane.x = 248;
    surface_pane.y = 192;
    surface_pane.width = 1140;
    surface_pane.height = 486;
    REQUIRE(carto::ui::save_workbench_preferences(path, preferences));

    const auto loaded = carto::ui::load_workbench_preferences(path);
    REQUIRE(loaded);
    REQUIRE(loaded.value().ledger_collapsed);
    REQUIRE(loaded.value().flow_use_counts[2] == 7U);
    REQUIRE(loaded.value().last_flow_index == 2);
    REQUIRE(loaded.value().instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).visible);
    REQUIRE(loaded.value().instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).x == 142);
    REQUIRE(loaded.value().instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).width == 286);
    REQUIRE(loaded.value().panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::work_surface)).visible);
    REQUIRE(loaded.value().panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::work_surface)).x == 248);
    REQUIRE(loaded.value().panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::work_surface)).height == 486);

    auto measured_but_unpositioned = preferences;
    auto& measured_scene = measured_but_unpositioned.instruments.at(
        static_cast<std::size_t>(carto::ui::WorkbenchInstrument::scene));
    measured_scene.positioned = false;
    measured_scene.x = 0;
    measured_scene.y = 0;
    measured_scene.width = 286;
    measured_scene.height = 330;
    REQUIRE(!carto::ui::save_workbench_preferences(path, measured_but_unpositioned));
    REQUIRE(carto::ui::record_workbench_instrument_geometry(
        measured_scene, -23.0, 41.0, 286.0, 330.0));
    REQUIRE(measured_scene.positioned);
    REQUIRE(measured_scene.x == -23);
    REQUIRE(measured_scene.y == 41);
    REQUIRE(carto::ui::save_workbench_preferences(path, measured_but_unpositioned));

    carto::ui::WorkbenchInstrumentLayout invalid_measurement;
    invalid_measurement.visible = true;
    invalid_measurement.x = 7;
    invalid_measurement.y = -9;
    invalid_measurement.width = 240;
    invalid_measurement.height = 180;
    const auto remains_unchanged = [&invalid_measurement] {
        return invalid_measurement.visible && !invalid_measurement.positioned &&
            invalid_measurement.x == 7 && invalid_measurement.y == -9 &&
            invalid_measurement.width == 240 && invalid_measurement.height == 180;
    };
    const double not_a_number = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    REQUIRE(!carto::ui::record_workbench_instrument_geometry(
        invalid_measurement, not_a_number, 1.0, 240.0, 180.0));
    REQUIRE(remains_unchanged());
    REQUIRE(!carto::ui::record_workbench_instrument_geometry(
        invalid_measurement, 1.0, infinity, 240.0, 180.0));
    REQUIRE(remains_unchanged());
    REQUIRE(!carto::ui::record_workbench_instrument_geometry(
        invalid_measurement, 1.0, 1.0, not_a_number, 180.0));
    REQUIRE(remains_unchanged());
    REQUIRE(!carto::ui::record_workbench_instrument_geometry(
        invalid_measurement, 1.0, 1.0, 240.0, infinity));
    REQUIRE(remains_unchanged());

    carto::ui::WorkbenchInstrumentLayout bounded_measurement;
    REQUIRE(carto::ui::record_workbench_instrument_geometry(
        bounded_measurement, -1.0e300, 1.0e300, 1.0, 1.0e300));
    REQUIRE(bounded_measurement.positioned);
    REQUIRE(bounded_measurement.x == -65536);
    REQUIRE(bounded_measurement.y == 65536);
    REQUIRE(bounded_measurement.width == 120);
    REQUIRE(bounded_measurement.height == 4096);

    const auto before_failed_save = [&path] {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }();
    auto invalid_preferences = preferences;
    invalid_preferences.instruments.at(static_cast<std::size_t>(
        carto::ui::WorkbenchInstrument::scene)).width = 20;
    REQUIRE(!carto::ui::save_workbench_preferences(path, invalid_preferences));
    std::ifstream after_failed_save_stream(path, std::ios::binary);
    const std::string after_failed_save(
        std::istreambuf_iterator<char>(after_failed_save_stream), {});
    REQUIRE(after_failed_save == before_failed_save);

    auto invalid_memory = preferences;
    invalid_memory.last_flow_index = 4;
    REQUIRE(!carto::ui::save_workbench_preferences(path, invalid_memory));
    std::ifstream after_invalid_memory_stream(path, std::ios::binary);
    const std::string after_invalid_memory(
        std::istreambuf_iterator<char>(after_invalid_memory_stream), {});
    REQUIRE(after_invalid_memory == before_failed_save);

    auto invalid_pane = preferences;
    invalid_pane.panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::work_surface)).height = 20;
    REQUIRE(!carto::ui::save_workbench_preferences(path, invalid_pane));
    std::ifstream after_invalid_pane_stream(path, std::ios::binary);
    const std::string after_invalid_pane(
        std::istreambuf_iterator<char>(after_invalid_pane_stream), {});
    REQUIRE(after_invalid_pane == before_failed_save);

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "header=CARTOGRAPHER_WORKBENCH_PREFS_V1\n"
                  "ledger_collapsed=0\n";
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            legacy << carto::ui::workbench_instrument_name(instrument)
                   << "=0,0,0,0,0,0\n";
        }
    }
    const auto legacy_loaded = carto::ui::load_workbench_preferences(path);
    REQUIRE(legacy_loaded);
    REQUIRE(legacy_loaded.value().last_flow_index == -1);
    REQUIRE(std::all_of(legacy_loaded.value().flow_use_counts.begin(),
                        legacy_loaded.value().flow_use_counts.end(),
                        [](std::uint32_t uses) { return uses == 0U; }));

    {
        std::ofstream previous(path, std::ios::binary | std::ios::trunc);
        previous << "header=CARTOGRAPHER_WORKBENCH_PREFS_V2\n"
                    "ledger_collapsed=0\n"
                    "workflow_last_flow=-1\n"
                    "workflow_flow_counts=0,0,0,0\n";
        for (std::size_t index = 0U; index < carto::ui::kWorkbenchInstrumentCount; ++index) {
            const auto instrument = static_cast<carto::ui::WorkbenchInstrument>(index);
            previous << carto::ui::workbench_instrument_name(instrument)
                     << "=0,0,0,0,0,0\n";
        }
    }
    const auto previous_loaded = carto::ui::load_workbench_preferences(path);
    REQUIRE(previous_loaded);
    REQUIRE(previous_loaded.value().panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::navigation)).visible);
    REQUIRE(previous_loaded.value().panes.at(static_cast<std::size_t>(
        carto::ui::WorkbenchPane::work_surface)).visible);

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "header=CARTOGRAPHER_WORKBENCH_PREFS_V1\n"
                   "ledger_collapsed=0\nscene=1,1,142,156,286\n";
    }
    REQUIRE(!carto::ui::load_workbench_preferences(path));
}

void workbench_snap_selects_nearest_target_with_bounded_tolerance() {
    const std::array<carto::core::Vec2d, 4U> candidates{{
        {48.0, 100.0}, {10.0, 104.0},
        {std::numeric_limits<double>::infinity(), 0.0}, {10.0, 104.0}}};
    const auto snapped = carto::ui::nearest_workbench_snap_target(
        {0.0, 100.0}, candidates, 24.0);
    REQUIRE(snapped.has_value());
    REQUIRE(snapped->x == 10.0 && snapped->y == 104.0);

    const auto outside_radius = carto::ui::nearest_workbench_snap_target(
        {0.0, 0.0}, candidates, 8.0);
    REQUIRE(!outside_radius.has_value());
    REQUIRE(!carto::ui::nearest_workbench_snap_target(
        {0.0, 0.0}, candidates, -1.0).has_value());
    const std::array<carto::core::Vec2d, 1U> forgiving_candidate{{{68.0, 0.0}}};
    const auto forgiving_snap = carto::ui::nearest_workbench_snap_target(
        {0.0, 0.0}, forgiving_candidate,
        carto::ui::kWorkbenchSnapDistancePixels);
    REQUIRE(forgiving_snap.has_value());
    REQUIRE(forgiving_snap->x == 68.0);
    const std::array<carto::core::Vec2d, 1U> snap_boundary{{{128.0, 0.0}}};
    REQUIRE(carto::ui::nearest_workbench_snap_target(
        {0.0, 0.0}, snap_boundary,
        carto::ui::kWorkbenchSnapDistancePixels).has_value());
    const std::array<carto::core::Vec2d, 1U> outside_snap_boundary{{{128.01, 0.0}}};
    REQUIRE(!carto::ui::nearest_workbench_snap_target(
        {0.0, 0.0}, outside_snap_boundary,
        carto::ui::kWorkbenchSnapDistancePixels).has_value());
    const std::array<carto::core::Vec2d, 2U> corner_candidates{{
        {48.0, 48.0}, {40.0, 40.0}}};
    const auto corner_snap = carto::ui::nearest_workbench_snap_target(
        {0.0, 0.0}, corner_candidates, 48.0);
    REQUIRE(corner_snap.has_value());
    REQUIRE(corner_snap->x == 40.0 && corner_snap->y == 40.0);

    const carto::core::Vec2d placement_target{400.0, 300.0};
    const auto placement_origin = carto::ui::workbench_placement_window_position(
        placement_target, {260.0, 180.0}, {24.0, 14.0},
        {100.0, 50.0}, {800.0, 600.0});
    REQUIRE(placement_origin.x == 376.0 && placement_origin.y == 286.0);
    const std::array<carto::core::Vec2d, 1U> placement_snap_target{{placement_target}};
    const auto placed_snap = carto::ui::nearest_workbench_snap_target(
        placement_origin, placement_snap_target,
        carto::ui::kWorkbenchSnapDistancePixels,
        {112.0, 62.0}, {628.0, 458.0});
    REQUIRE(placed_snap.has_value());
    REQUIRE(placed_snap->x == placement_target.x &&
        placed_snap->y == placement_target.y);

    const auto clamped_to_work_surface = carto::ui::clamp_workbench_instrument_position(
        {40.0, 40.0}, {250.0, 180.0}, {200.0, 100.0}, {600.0, 400.0}, 12.0);
    REQUIRE(clamped_to_work_surface.x == 212.0);
    REQUIRE(clamped_to_work_surface.y == 112.0);
    const auto clamped_to_far_edge = carto::ui::clamp_workbench_instrument_position(
        {900.0, 700.0}, {250.0, 180.0}, {200.0, 100.0}, {600.0, 400.0}, 12.0);
    REQUIRE(clamped_to_far_edge.x == 538.0);
    REQUIRE(clamped_to_far_edge.y == 308.0);
    const auto oversized_instrument = carto::ui::clamp_workbench_instrument_position(
        {0.0, 0.0}, {900.0, 700.0}, {200.0, 100.0}, {600.0, 400.0}, 12.0);
    REQUIRE(oversized_instrument.x == 212.0);
    REQUIRE(oversized_instrument.y == 112.0);

    const std::array<carto::core::Vec2d, 2U> bounded_candidates{{
        {99.0, 100.0}, {130.0, 100.0}}};
    const auto bounded_snap = carto::ui::nearest_workbench_snap_target(
        {100.0, 100.0}, bounded_candidates, 32.0,
        {100.0, 100.0}, {500.0, 400.0});
    REQUIRE(bounded_snap.has_value());
    REQUIRE(bounded_snap->x == 130.0 && bounded_snap->y == 100.0);
    REQUIRE(!carto::ui::nearest_workbench_snap_target(
        {100.0, 100.0}, std::span<const carto::core::Vec2d>(bounded_candidates).first(1U), 32.0,
        {100.0, 100.0}, {500.0, 400.0}).has_value());
    REQUIRE(!carto::ui::nearest_workbench_snap_target(
        {100.0, 100.0}, bounded_candidates, 32.0,
        {500.0, 400.0}, {100.0, 100.0}).has_value());

    const std::array<carto::core::Vec2d, 2U> equidistant_snap_targets{{
        {190.0, 200.0}, {210.0, 200.0}}};
    const std::array<carto::core::Vec2d, 2U> reversed_equidistant_snap_targets{{
        equidistant_snap_targets[1U], equidistant_snap_targets[0U]}};
    const auto stable_tie = carto::ui::nearest_workbench_snap_target(
        {200.0, 200.0}, equidistant_snap_targets, 20.0);
    const auto reversed_stable_tie = carto::ui::nearest_workbench_snap_target(
        {200.0, 200.0}, reversed_equidistant_snap_targets, 20.0);
    REQUIRE(stable_tie.has_value() && reversed_stable_tie.has_value());
    REQUIRE(stable_tie->x == 190.0 && stable_tie->y == 200.0);
    REQUIRE(reversed_stable_tie->x == stable_tie->x &&
        reversed_stable_tie->y == stable_tie->y);
    const auto bounded_stable_tie = carto::ui::nearest_workbench_snap_target(
        {200.0, 200.0}, equidistant_snap_targets, 20.0,
        {100.0, 100.0}, {300.0, 300.0});
    const auto reversed_bounded_stable_tie = carto::ui::nearest_workbench_snap_target(
        {200.0, 200.0}, reversed_equidistant_snap_targets, 20.0,
        {100.0, 100.0}, {300.0, 300.0});
    REQUIRE(bounded_stable_tie.has_value() && reversed_bounded_stable_tie.has_value());
    REQUIRE(bounded_stable_tie->x == 190.0 && bounded_stable_tie->y == 200.0);
    REQUIRE(reversed_bounded_stable_tie->x == bounded_stable_tie->x &&
        reversed_bounded_stable_tie->y == bounded_stable_tie->y);

    const std::array<carto::ui::WorkbenchSnapWindow, 1U> offset_panel{{
        {{200.0, 100.0}, {220.0, 140.0}}}};
    const auto side_dock = carto::ui::workbench_instrument_snap_position(
        {480.0, 500.0}, {200.0, 160.0}, {100.0, 50.0}, {1000.0, 800.0},
        offset_panel, 10.0);
    REQUIRE(side_dock.has_value());
    REQUIRE(side_dock->x == 430.0 && side_dock->y == 500.0);
    REQUIRE(!carto::ui::workbench_instrument_snap_position(
        {600.0, 500.0}, {200.0, 160.0}, {100.0, 50.0}, {1000.0, 800.0},
        offset_panel, 10.0).has_value());
    REQUIRE(!carto::ui::workbench_instrument_snap_position(
        {480.0, 500.0}, {0.0, 160.0}, {100.0, 50.0}, {1000.0, 800.0},
        offset_panel, 10.0).has_value());

    REQUIRE(!carto::ui::workbench_drag_threshold_crossed(
        {0.0, 0.0}, {3.0, 4.0}, 6.0));
    REQUIRE(carto::ui::workbench_drag_threshold_crossed(
        {0.0, 0.0}, {6.0, 0.0}, 6.0));
    REQUIRE(!carto::ui::workbench_drag_threshold_crossed(
        {0.0, 0.0}, {12.0, 0.0}, -1.0));
    const auto moved = carto::ui::workbench_dragged_position(
        {300.0, 180.0}, {410.0, 250.0}, {470.0, 215.0});
    REQUIRE(moved.x == 360.0 && moved.y == 145.0);
    const auto invalid_pointer = carto::ui::workbench_dragged_position(
        {300.0, 180.0}, {410.0, 250.0},
        {std::numeric_limits<double>::quiet_NaN(), 215.0});
    REQUIRE(invalid_pointer.x == 300.0 && invalid_pointer.y == 180.0);

    const auto fitted_size = carto::ui::fit_workbench_instrument_size(
        {340.0, 700.0}, {800.0, 500.0});
    REQUIRE(fitted_size.x == 340.0);
    REQUIRE(fitted_size.y == 340.0);
    const auto minimum_size = carto::ui::fit_workbench_instrument_size(
        {10.0, 10.0}, {80.0, 40.0});
    REQUIRE(minimum_size.x == 120.0);
    REQUIRE(minimum_size.y == carto::ui::kWorkbenchInstrumentMinimumHeight);

    REQUIRE(carto::ui::workbench_instrument_drag_grip_width(286.0) == 286.0);
    REQUIRE(carto::ui::workbench_instrument_drag_grip_width(0.0) == 1.0);
    REQUIRE(carto::ui::workbench_instrument_drag_grip_width(-1.0) == 1.0);
    REQUIRE(carto::ui::workbench_instrument_drag_grip_width(
        std::numeric_limits<double>::quiet_NaN()) == 1.0);
    REQUIRE(carto::ui::workbench_drag_grip_contains(
        {12.0, 19.0}, {12.0, 19.0}, {80.0, 44.0}));
    REQUIRE(carto::ui::workbench_drag_grip_contains(
        {80.0, 44.0}, {12.0, 19.0}, {80.0, 44.0}));
    REQUIRE(!carto::ui::workbench_drag_grip_contains(
        {81.0, 44.0}, {12.0, 19.0}, {80.0, 44.0}));
    REQUIRE(!carto::ui::workbench_drag_grip_contains(
        {std::numeric_limits<double>::quiet_NaN(), 20.0},
        {12.0, 19.0}, {80.0, 44.0}));
    REQUIRE(!carto::ui::workbench_drag_grip_contains(
        {20.0, 20.0}, {80.0, 44.0}, {12.0, 19.0}));
    REQUIRE(carto::ui::workbench_instrument_drag_region_contains(
        {120.0, 12.0}, {0.0, 40.0}, {200.0, 64.0},
        {100.0, 0.0}, {286.0, 28.0}, true));
    REQUIRE(carto::ui::workbench_instrument_drag_region_contains(
        {120.0, 52.0}, {0.0, 40.0}, {200.0, 64.0},
        {100.0, 0.0}, {286.0, 28.0}, true));
    REQUIRE(!carto::ui::workbench_instrument_drag_region_contains(
        {280.0, 12.0}, {0.0, 40.0}, {200.0, 64.0},
        {100.0, 0.0}, {260.0, 28.0}, true));
    REQUIRE(!carto::ui::workbench_instrument_drag_region_contains(
        {120.0, 12.0}, {0.0, 40.0}, {200.0, 64.0},
        {100.0, 0.0}, {286.0, 28.0}, false));
}

void ui_observation_queries_use_current_viewport_without_mutation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{
        "Rural sightline blocker", {1.0, 1.0, 1.0}}));
    const auto before = ui.snapshot();
    REQUIRE(before.objects.size() == 1U);
    carto::editor::PointSightlineQuery query{
        {-2.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, before.project_revision,
        std::nullopt, std::nullopt};

    const auto result = ui.evaluate_point_sightline(query);
    REQUIRE(result);
    REQUIRE(!result.value().visible);
    REQUIRE(result.value().blocker_object == before.objects.front().object.id);
    REQUIRE(result.value().source_revision == before.viewport.project_revision);
    const auto after = ui.snapshot();
    REQUIRE(after.project_revision == before.project_revision);
    REQUIRE(after.operations.size() == before.operations.size());

    carto::editor::RouteVisibilityQuery route_query{
        {{-2.0, 0.0, 0.0}, {1.0, 0.0, 0.0}}, {},
        {5.0, 0.0, 0.0}, before.project_revision, std::nullopt};
    const auto route = ui.evaluate_route_visibility(route_query);
    REQUIRE(route);
    REQUIRE(route.value().samples.size() == 2U);
    REQUIRE(!route.value().samples.front().visible);
    REQUIRE(route.value().samples.back().visible);
    REQUIRE(route.value().source_revision == before.viewport.project_revision);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.snapshot().operations.size() == before.operations.size());

    carto::editor::LandmarkViewshedQuery viewshed_query{
        {-2.0, 0.0, 0.0}, std::nullopt, 2U, 2U,
        {{5.0, 0.0, -0.2}, {5.0, 0.0, 0.2},
         {5.0, 3.0, -0.2}, {5.0, 3.0, 0.2}},
        before.project_revision};
    const auto viewshed = ui.evaluate_landmark_viewshed(viewshed_query);
    REQUIRE(viewshed);
    REQUIRE(viewshed.value().cells.size() == 4U);
    REQUIRE(viewshed.value().visible_cells == 2U);
    REQUIRE(viewshed.value().source_revision == before.viewport.project_revision);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.snapshot().operations.size() == before.operations.size());

    carto::editor::AuthoringObservationQuery observation_query;
    observation_query.observer = {-2.0, 0.0, 0.0};
    observation_query.forward = {1.0, 0.0, 0.0};
    observation_query.up = {0.0, 1.0, 0.0};
    observation_query.source_revision = before.project_revision;
    observation_query.targets = {{1U, before.objects.front().object.id,
        {0.0, 0.0, 0.0}, carto::editor::ObservationTargetKind::landmark}};
    const auto observation = ui.evaluate_authoring_observation(observation_query);
    REQUIRE(observation);
    REQUIRE(observation.value().visible_targets == 1U);
    REQUIRE(observation.value().targets.front().target_identity == 1U);
    REQUIRE(observation.value().source_revision == before.viewport.project_revision);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.snapshot().operations.size() == before.operations.size());

    query.source_revision = query.source_revision.next();
    const auto stale = ui.evaluate_point_sightline(query);
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);

    route_query.source_revision = route_query.source_revision.next();
    const auto stale_route = ui.evaluate_route_visibility(route_query);
    REQUIRE(!stale_route);
    REQUIRE(stale_route.error().code == carto::core::ErrorCode::stale_data);
    viewshed_query.source_revision = viewshed_query.source_revision.next();
    const auto stale_viewshed = ui.evaluate_landmark_viewshed(viewshed_query);
    REQUIRE(!stale_viewshed);
    REQUIRE(stale_viewshed.error().code == carto::core::ErrorCode::stale_data);
    observation_query.source_revision = observation_query.source_revision.next();
    const auto stale_observation = ui.evaluate_authoring_observation(observation_query);
    REQUIRE(!stale_observation);
    REQUIRE(stale_observation.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
}

void workbench_click_place_and_viewport_pan_fallbacks_are_explicit() {
    using carto::ui::NativePointerButton;
    REQUIRE(carto::ui::workbench_instrument_should_settle(
        false, true, false, true));
    REQUIRE(carto::ui::workbench_instrument_should_settle(
        true, false, false, false));
    REQUIRE(carto::ui::workbench_instrument_should_settle(
        false, false, true, true));
    REQUIRE(!carto::ui::workbench_instrument_should_settle(
        false, false, false, true));
    REQUIRE(!carto::ui::workbench_instrument_should_settle(
        false, true, false, false));

    REQUIRE(carto::ui::native_pointer_requests_pan(
        NativePointerButton::primary, false, false, true));
    REQUIRE(carto::ui::native_pointer_requests_pan(
        NativePointerButton::primary, false, true, false));
    REQUIRE(carto::ui::native_pointer_requests_pan(
        NativePointerButton::primary, true, false, false));
    REQUIRE(carto::ui::native_pointer_requests_pan(
        NativePointerButton::middle, false, false, false));
    REQUIRE(carto::ui::native_pointer_requests_pan(
        NativePointerButton::secondary, false, false, false));
    REQUIRE(!carto::ui::native_pointer_requests_pan(
        NativePointerButton::primary, false, false, false));
    REQUIRE(carto::ui::native_pointer_requests_pan(
        NativePointerButton::secondary, true, true, true));

    REQUIRE(carto::ui::work_surface_pointer_requests_pan(
        NativePointerButton::primary, false, false, false, true));
    REQUIRE(!carto::ui::work_surface_pointer_requests_pan(
        NativePointerButton::primary, false, false, false, false));
    REQUIRE(carto::ui::work_surface_pointer_requests_pan(
        NativePointerButton::primary, false, true, false, false));
    REQUIRE(carto::ui::work_surface_pointer_requests_pan(
        NativePointerButton::middle, false, false, false, false));
    REQUIRE(carto::ui::work_surface_pointer_requests_pan(
        NativePointerButton::secondary, false, false, false, false));
    REQUIRE(carto::ui::work_surface_pointer_requests_pan(
        NativePointerButton::primary, true, false, false, false));

    REQUIRE(carto::ui::work_surface_camera_pan_allowed(true, true, true));
    REQUIRE(!carto::ui::work_surface_camera_pan_allowed(false, true, true));
    REQUIRE(!carto::ui::work_surface_camera_pan_allowed(true, false, true));
    REQUIRE(!carto::ui::work_surface_camera_pan_allowed(true, true, false));

    REQUIRE(carto::ui::work_surface_camera_hotkey_allowed(
        true, true, false, false, true));
    REQUIRE(!carto::ui::work_surface_camera_hotkey_allowed(
        false, true, false, false, true));
    REQUIRE(!carto::ui::work_surface_camera_hotkey_allowed(
        true, false, false, false, true));
    REQUIRE(!carto::ui::work_surface_camera_hotkey_allowed(
        true, true, true, false, true));
    REQUIRE(!carto::ui::work_surface_camera_hotkey_allowed(
        true, true, false, true, true));
    REQUIRE(!carto::ui::work_surface_camera_hotkey_allowed(
        true, true, false, false, false));

    const auto camera_left = carto::ui::work_surface_camera_keyboard_pan_delta(
        true, false, false, false, {1000.0, 500.0});
    REQUIRE(std::abs(camera_left.x + 160.0) < 1e-12 && camera_left.y == 0.0);
    const auto camera_up = carto::ui::work_surface_camera_keyboard_pan_delta(
        false, false, true, false, {1000.0, 500.0});
    REQUIRE(camera_up.x == 0.0 && std::abs(camera_up.y + 80.0) < 1e-12);
    const auto opposite_keys = carto::ui::work_surface_camera_keyboard_pan_delta(
        true, true, true, true, {1000.0, 500.0});
    REQUIRE(opposite_keys.x == 0.0 && opposite_keys.y == 0.0);
    const auto diagonal_keys = carto::ui::work_surface_camera_keyboard_pan_delta(
        false, true, true, false, {1000.0, 500.0});
    REQUIRE(std::abs(std::hypot(diagonal_keys.x / 1000.0,
                               diagonal_keys.y / 500.0) - 0.16) < 1e-12);
    REQUIRE(carto::ui::work_surface_camera_keyboard_pan_delta(
                true, false, false, false, {0.0, 500.0}).x == 0.0);
    REQUIRE(carto::ui::work_surface_camera_keyboard_pan_delta(
                true, false, false, false,
                {std::numeric_limits<double>::infinity(), 500.0}).x == 0.0);

    REQUIRE(carto::ui::work_surface_pointer_is_unclaimed(true, false));
    REQUIRE(!carto::ui::work_surface_pointer_is_unclaimed(true, true));
    REQUIRE(carto::ui::work_surface_pointer_is_unclaimed(false, true));
    REQUIRE(carto::ui::work_surface_pointer_is_unclaimed(false, false));
}

void workbench_drag_uses_the_topmost_visible_instrument() {
    using carto::ui::WorkbenchInstrument;
    using carto::ui::WorkbenchInstrumentDragTarget;
    const std::array<WorkbenchInstrumentDragTarget, 3U> targets{{
        {WorkbenchInstrument::scene, 1, true,
            {10.0, 10.0}, {90.0, 50.0}, {110.0, 10.0}, {190.0, 25.0}, true},
        {WorkbenchInstrument::tools, 4, true,
            {10.0, 10.0}, {90.0, 50.0}, {}, {}, false},
        {WorkbenchInstrument::extrude, 8, false,
            {10.0, 10.0}, {90.0, 50.0}, {}, {}, false},
    }};
    const auto target = carto::ui::topmost_workbench_drag_target(
        {50.0, 30.0}, targets);
    REQUIRE(target == WorkbenchInstrument::tools);
    REQUIRE(carto::ui::topmost_workbench_drag_target(
        {160.0, 20.0}, targets) == WorkbenchInstrument::scene);
    REQUIRE(carto::ui::topmost_workbench_drag_grip_target(
        {50.0, 30.0}, targets) == WorkbenchInstrument::tools);
    REQUIRE(!carto::ui::topmost_workbench_drag_grip_target(
        {160.0, 20.0}, targets).has_value());
    REQUIRE(!carto::ui::topmost_workbench_drag_target(
        {195.0, 20.0}, targets).has_value());
    REQUIRE(!carto::ui::topmost_workbench_drag_target(
        {150.0, 30.0}, targets).has_value());
    REQUIRE(!carto::ui::topmost_workbench_drag_target(
        {std::numeric_limits<double>::quiet_NaN(), 30.0}, targets).has_value());

    // A live panel moved away from its persisted location must still shield
    // its surface from viewport selection and camera gestures.
    REQUIRE(carto::ui::workbench_instrument_window_contains(
        {280.0, 300.0}, {220.0, 100.0}, {520.0, 420.0}, true));
    REQUIRE(!carto::ui::workbench_instrument_window_contains(
        {280.0, 300.0}, {20.0, 10.0}, {180.0, 90.0}, true));
    REQUIRE(!carto::ui::workbench_instrument_window_contains(
        {280.0, 300.0}, {220.0, 100.0}, {520.0, 420.0}, false));
    REQUIRE(!carto::ui::workbench_instrument_window_contains(
        {std::numeric_limits<double>::quiet_NaN(), 300.0},
        {220.0, 100.0}, {520.0, 420.0}, true));

    // Exercise the native gesture math end-to-end: choose the topmost hit,
    // apply one pointer delta to the captured window origin, then settle to
    // the nearest admissible snap target.
    const auto dragged = carto::ui::workbench_dragged_position(
        {100.0, 150.0}, {50.0, 30.0}, {110.0, 90.0});
    REQUIRE(dragged.x == 160.0 && dragged.y == 210.0);
    const std::array<carto::core::Vec2d, 2U> snap_targets{{
        {168.0, 216.0}, {250.0, 300.0}}};
    const auto settled = carto::ui::nearest_workbench_snap_target(
        dragged, snap_targets, 16.0, {0.0, 0.0}, {500.0, 400.0});
    REQUIRE(settled.has_value());
    REQUIRE(settled->x == 168.0 && settled->y == 216.0);
}

void workbench_native_release_uses_drop_position_not_stale_imgui_pointer() {
    const carto::core::Vec2d rack_press{84.0, 210.0};
    const carto::core::Vec2d surface_release{612.0, 404.0};
    const auto native_drop = carto::ui::workbench_release_position(
        true, surface_release, rack_press);
    REQUIRE(native_drop.x == surface_release.x);
    REQUIRE(native_drop.y == surface_release.y);

    const auto ordinary_release = carto::ui::workbench_release_position(
        false, surface_release, rack_press);
    REQUIRE(ordinary_release.x == rack_press.x);
    REQUIRE(ordinary_release.y == rack_press.y);
    const auto invalid_fallback = carto::ui::workbench_release_position(
        false, surface_release,
        {std::numeric_limits<double>::quiet_NaN(), 0.0});
    REQUIRE(invalid_fallback.x == 0.0 && invalid_fallback.y == 0.0);
}

void ui_connects_surface_fidelity_preview_without_authoring_mutation() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);

    REQUIRE(!ui.compile_surface_preview(6U));
    REQUIRE(ui.snapshot().surface_fidelity.status ==
            carto::ui::SurfaceFidelityStatus::unavailable);

    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Surface box", {2.0, 2.0, 2.0}}));
    const auto before = ui.snapshot();
    REQUIRE(before.objects.size() == 1U);
    const auto object = before.objects.front().object.id;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    const auto selected = ui.snapshot();
    const auto revision = selected.project_revision;
    const auto bounded = ui.compile_surface_preview(
        carto::ui::kMaxSurfacePreviewTriangles + 1U,
        carto::surface::RepresentationMode::fidelity_closure);
    REQUIRE(!bounded);
    REQUIRE(bounded.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(ui.snapshot().surface_fidelity.status ==
            carto::ui::SurfaceFidelityStatus::unavailable);
    REQUIRE(ui.snapshot().operations.size() == 1U);
    REQUIRE(ui.compile_surface_preview(
        6U, carto::surface::RepresentationMode::fidelity_closure));

    auto preview = ui.snapshot().surface_fidelity;
    REQUIRE(preview.status == carto::ui::SurfaceFidelityStatus::ready);
    REQUIRE(preview.admitted);
    REQUIRE(preview.object.has_value() && *preview.object == object);
    REQUIRE(preview.source_triangles == 12U);
    REQUIRE(preview.target_triangles == 6U);
    REQUIRE(preview.mode == carto::surface::RepresentationMode::fidelity_closure);
    REQUIRE(preview.compiled != nullptr);
    REQUIRE(preview.compiled->geometry.triangle_count() == 6U);
    REQUIRE(!preview.domain_digest.empty());
    REQUIRE(!preview.representation_digest.empty());
    REQUIRE(preview.validate());
    REQUIRE(ui.snapshot().project_revision == revision);
    REQUIRE(ui.snapshot().operations.size() == 1U);

    REQUIRE(ui.dispatch(carto::application::SetObjectTransformAction{
        object, carto::core::Transform{{1.0, 0.0, 0.0}, carto::core::Quaternion::identity(),
                                       {1.0, 1.0, 1.0}}, std::nullopt}));
    preview = ui.snapshot().surface_fidelity;
    REQUIRE(preview.status == carto::ui::SurfaceFidelityStatus::stale);
    REQUIRE(!preview.admitted);

    REQUIRE(ui.clear_surface_preview());
    REQUIRE(ui.snapshot().surface_fidelity.status == carto::ui::SurfaceFidelityStatus::idle);
}

void ui_imports_mesh_through_provider_and_authoring_boundaries() {
    TempDirectory temp;
    const auto project_path = temp.path() / "wired-import.carto";
    const auto obj_path = temp.path() / "triangle.obj";
    const auto ply_path = temp.path() / "triangle.ply";
    const auto stl_path = temp.path() / "triangle.stl";
    {
        std::ofstream obj(obj_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(obj));
        obj << "v 0 0 0\n"
               "v 1 0 0\n"
               "v 0 1 0\n"
               "f 1 2 3\n";
    }
    {
        std::ofstream ply(ply_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(ply));
        ply << "ply\n"
               "format ascii 1.0\n"
               "element vertex 3\n"
               "property float x\n"
               "property float y\n"
               "property float z\n"
               "element face 1\n"
               "property list uchar int vertex_indices\n"
               "end_header\n"
               "0 0 0\n"
               "1 0 0\n"
               "0 1 0\n"
               "3 0 1 2\n";
    }
    {
        std::ofstream stl(stl_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(stl));
        stl << "solid triangle\n"
               "facet normal 0 0 1\n"
               "outer loop\n"
               "vertex 0 0 0\n"
               "vertex 1 0 0\n"
               "vertex 0 1 0\n"
               "endloop\n"
               "endfacet\n"
               "endsolid triangle\n";
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto revision_before = ui.snapshot().project_revision;

    auto unbacked_mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(unbacked_mesh);
    const std::string missing_source_bytes = "missing source snapshot";
    carto::assets::SourceImportRecord unbacked_record;
    unbacked_record.identity = "mesh.source.unbacked";
    unbacked_record.relative_source_path =
        ".carto-assets/source-blobs/sha256/missing-source";
    unbacked_record.source_generation = carto::core::Revision{1U};
    unbacked_record.kind = carto::assets::SourceAssetKind::mesh;
    unbacked_record.source_digest = carto::assets::sha256(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(missing_source_bytes.data()),
            missing_source_bytes.size()));
    unbacked_record.importer_identity = "carto.import.obj";
    unbacked_record.artifact_identity = "pending.mesh";
    unbacked_record.artifact_digest =
        carto::application::canonical_mesh_geometry_digest(unbacked_mesh.value());
    unbacked_record.domain_digests = {{
        carto::assets::SourceAssetDomain::geometry, unbacked_record.artifact_digest}};
    const auto fabricated_source = ui.dispatch(
        carto::application::CreateMeshObjectAction{
            "Unbacked", std::move(unbacked_mesh.value()), unbacked_record});
    REQUIRE(!fabricated_source);
    REQUIRE(ui.snapshot().project_revision == revision_before);
    REQUIRE(ui.snapshot().objects.empty());
    REQUIRE(ui.snapshot().source_assets.empty());

    const auto imported = ui.import_mesh(obj_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported);
    REQUIRE(imported.value().object_name == "triangle");
    REQUIRE(imported.value().vertices == 3U);
    REQUIRE(imported.value().faces == 1U);
    REQUIRE(imported.value().triangles == 1U);
    REQUIRE(imported.value().source_linked);
    REQUIRE(!imported.value().source_identity.empty());
    REQUIRE(imported.value().relative_source_path.starts_with(
        ".carto-assets/source-blobs/sha256/"));
    REQUIRE(imported.value().dispatch.action == "Import Source Mesh");
    REQUIRE(imported.value().dispatch.document_changed);
    REQUIRE(imported.value().dispatch.source == carto::application::OperationSource::human);

    const auto snapshot = ui.snapshot();
    REQUIRE(snapshot.project_revision > revision_before);
    REQUIRE(snapshot.dirty);
    REQUIRE(snapshot.objects.size() == 1U);
    REQUIRE(snapshot.objects.front().object.name == "triangle");
    REQUIRE(snapshot.objects.front().object.mesh_asset.has_value());
    REQUIRE(snapshot.viewport.scene.instances().size() == 1U);
    REQUIRE(snapshot.source_assets.size() == 1U);
    REQUIRE(snapshot.source_assets.front().identity == imported.value().source_identity);
    REQUIRE(snapshot.source_assets.front().source_generation == carto::core::Revision{1U});
    REQUIRE(snapshot.source_assets.front().artifact_identity ==
        "mesh.asset." + std::to_string(*snapshot.objects.front().object.mesh_asset));
    REQUIRE(snapshot.source_assets.front().bound_mesh_asset ==
        snapshot.objects.front().object.mesh_asset);
    REQUIRE(snapshot.source_assets.front().relative_source_path ==
        imported.value().relative_source_path);
    REQUIRE(!snapshot.operations.empty());
    REQUIRE(snapshot.operations.back().action == "Import Source Mesh");

    const auto recovery = carto::application::ApplicationSession::inspect_recovery(project_path);
    REQUIRE(recovery);
    REQUIRE(recovery.value().state == carto::application::RecoveryInspectionState::pending);
    REQUIRE(!recovery.value().journal_entries.empty());
    REQUIRE(recovery.value().journal_entries.back().event_type ==
            "cartographer.application_action");
    carto::journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries);
    REQUIRE(!journal_entries.value().empty());
    const std::string journal_payload(
        journal_entries.value().back().payload.begin(),
        journal_entries.value().back().payload.end());
    REQUIRE(journal_payload.find("operation=Import Source Mesh") != std::string::npos);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto serialized_project = carto::project::ProjectDocument::load(project_path);
    REQUIRE(serialized_project);
    REQUIRE(serialized_project.value().source_asset_registry().records().size() == 1U);
    REQUIRE(serialized_project.value().serialize().find(obj_path.string()) == std::string::npos);

    const auto unchanged_revision = ui.snapshot().project_revision;
    const auto unchanged_import = ui.import_mesh(obj_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(unchanged_import);
    REQUIRE(unchanged_import.value().source_linked);
    REQUIRE(!unchanged_import.value().dispatch.document_changed);
    REQUIRE(ui.snapshot().project_revision == unchanged_revision);
    REQUIRE(ui.snapshot().objects.size() == 1U);

    const auto metadata_base = ui.snapshot();
    const auto metadata_review = ui.preview_source_asset_metadata(
        metadata_base.source_assets.front().identity,
        metadata_base.source_assets.front().source_generation,
        metadata_base.project_generation, metadata_base.project_revision,
        {"canopy", "exterior"}, {});
    REQUIRE(metadata_review && ui.apply_source_reimport_plan(metadata_review.value()));
    {
        std::ofstream obj(obj_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(obj));
        obj << "v 0 0 0\n"
               "v 1.5 0 0\n"
               "v 0 1 0\n"
               "f 1 2 3\n";
    }
    const auto reimport_snapshot = ui.snapshot();
    const auto reimport_review = ui.preview_source_mesh_reimport(
        metadata_base.source_assets.front().identity, obj_path,
        carto::ui::MeshImportFormat::obj,
        reimport_snapshot.project_generation, reimport_snapshot.project_revision);
    REQUIRE(reimport_review);
    const auto reimported_with_metadata = ui.apply_source_mesh_reimport(
        reimport_review.value(), obj_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(reimported_with_metadata &&
        reimported_with_metadata.value().dispatch.document_changed);
    const auto snapshot_after_reimport = ui.snapshot();
    const auto source_after_reimport = std::find_if(snapshot_after_reimport.source_assets.begin(),
        snapshot_after_reimport.source_assets.end(), [&metadata_base](const auto& source_asset) {
            return source_asset.identity == metadata_base.source_assets.front().identity;
        });
    REQUIRE(source_after_reimport != snapshot_after_reimport.source_assets.end());
    REQUIRE((source_after_reimport->authored_tags ==
        std::vector<std::string>{"canopy", "exterior"}));

    const auto imported_ply = ui.import_mesh(ply_path, carto::ui::MeshImportFormat::ply);
    REQUIRE(imported_ply);
    REQUIRE(imported_ply.value().vertices == 3U);
    REQUIRE(imported_ply.value().faces == 1U);
    REQUIRE(imported_ply.value().triangles == 1U);
    const auto imported_stl = ui.import_mesh(stl_path, carto::ui::MeshImportFormat::stl);
    REQUIRE(imported_stl);
    REQUIRE(imported_stl.value().vertices == 3U);
    REQUIRE(imported_stl.value().faces == 1U);
    REQUIRE(imported_stl.value().triangles == 1U);
    REQUIRE(ui.snapshot().objects.size() == 3U);
    REQUIRE(ui.snapshot().source_assets.size() == 3U);

    const auto broken_obj_path = temp.path() / "broken.obj";
    {
        std::ofstream broken(broken_obj_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(broken));
        broken << "not an OBJ vertex";
    }
    const std::string broken_bytes = "not an OBJ vertex";
    const auto broken_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(broken_bytes.data()), broken_bytes.size()));
    carto::assets::BlobStore source_store(
        project_path.parent_path() / ".carto-assets" / "source-blobs");
    const auto stable_revision = ui.snapshot().project_revision;
    const auto stable_operations = ui.snapshot().operations.size();
    const auto bad_mesh = ui.import_mesh(
        broken_obj_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(!bad_mesh);
    REQUIRE(!std::filesystem::exists(source_store.path_for(broken_digest)));
    REQUIRE(std::filesystem::is_empty(
        project_path.parent_path() / ".carto-assets" / "import-staging"));
    REQUIRE(ui.snapshot().project_revision == stable_revision);
    REQUIRE(ui.snapshot().objects.size() == 3U);

    const auto missing = ui.import_mesh(
        temp.path() / "missing.obj", carto::ui::MeshImportFormat::obj);
    REQUIRE(!missing);
    REQUIRE(ui.snapshot().project_revision == stable_revision);
    REQUIRE(ui.snapshot().objects.size() == 3U);
    REQUIRE(ui.snapshot().operations.size() == stable_operations);
    REQUIRE(!ui.snapshot().ui_problems.empty());

    const auto unsupported = ui.import_mesh(
        obj_path, static_cast<carto::ui::MeshImportFormat>(99));
    REQUIRE(!unsupported);
    REQUIRE(unsupported.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(ui.snapshot().project_revision == stable_revision);

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::save));
    REQUIRE(!ui.snapshot().dirty);
}

void ui_promotes_private_botanical_output_with_bound_human_receipts() {
    TempDirectory temp;
    const auto project_path = temp.path() / "botanical-promotion.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::ImportConditionFixtureAction{}));
    auto fixture_authoring = session.condition_authoring();
    REQUIRE(fixture_authoring);
    auto current_authoring_document = fixture_authoring.value();
    current_authoring_document.project_revision =
        current_authoring_document.project_revision.next();
    REQUIRE(ui.dispatch(carto::application::ImportConditionAuthoringAction{
        current_authoring_document.serialize()}));

    carto::botanical::Source source;
    source.identity = "pine-promotion-fixture";
    const auto current_authoring = session.condition_authoring();
    REQUIRE(current_authoring);
    source.revision = current_authoring.value().project_revision;
    const std::string source_snapshot = source.serialize();
    const auto* source_data = reinterpret_cast<const std::uint8_t*>(source_snapshot.data());
    const auto source_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        source_data, source_snapshot.size()));
    const auto generated_payload = botanical_gltf_fixture(
        source.identity, source.revision.value());
    auto transport = std::make_shared<BotanicalPromotionTransport>(
        generated_payload, source.revision);
    carto::runtime_api::Client runtime_client(
        carto::runtime_api::Config{
            "https://hub.example.test", "fixture-function-key",
            "fixture-bearer-token", "fixture-organization"},
        transport);
    REQUIRE(transport->requests.empty());

    const auto submitted = runtime_client.submit_botanical_source(
        source.identity, source.revision, source_snapshot,
        "explicit native authoring test", "request-pine-7");
    REQUIRE(submitted && submitted.value().status == "queued");
    REQUIRE(transport->requests.size() == 2U);
    REQUIRE(transport->requests.front().path ==
        "/api/cartographer/runtime/v1/artifacts");
    REQUIRE(runtime_text(transport->requests.front().body) == source_snapshot);
    REQUIRE(runtime_text(transport->requests[1U].body).find(
        "explicit native authoring test") != std::string::npos);
    REQUIRE(runtime_text(transport->requests[1U].body).find(
        "fixture-function-key") == std::string::npos);
    REQUIRE(runtime_text(transport->requests[1U].body).find(
        "fixture-bearer-token") == std::string::npos);

    const auto completed = runtime_client.get_job(submitted.value());
    REQUIRE(completed && completed.value().status == "succeeded");
    REQUIRE(completed.value().source_revision == source.revision);
    REQUIRE(completed.value().source_digest == source_digest);
    const auto downloaded = runtime_client.download_output(completed.value());
    REQUIRE(downloaded && downloaded.value() == generated_payload);
    REQUIRE(transport->requests.size() == 4U);
    const auto payload = downloaded.value();
    const auto job = completed.value();
    REQUIRE(job.output_digest == carto::assets::sha256(payload));

    const auto before_unsaved = ui.snapshot();
    const auto unsaved = ui.import_generated_botanical_gltf(
        payload, source, job, "fixture-organization", "Pine candidate");
    REQUIRE(!unsaved);
    const auto after_unsaved = ui.snapshot();
    REQUIRE(after_unsaved.project_revision == before_unsaved.project_revision);
    REQUIRE(after_unsaved.objects.size() == before_unsaved.objects.size());
    REQUIRE(after_unsaved.operations.size() == before_unsaved.operations.size());

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto before = ui.snapshot();
    carto::assets::SourceImportRecord unreceipted;
    unreceipted.identity = "botanical.generated.unreceipted";
    unreceipted.relative_source_path = ".carto-assets/source-blobs/unreceipted.gltf";
    unreceipted.source_generation = carto::core::Revision{1U};
    unreceipted.kind = carto::assets::SourceAssetKind::mesh;
    unreceipted.source_digest = job.output_digest;
    unreceipted.importer_identity = "carto.import.generated-gltf";
    unreceipted.importer_version = 1U;
    unreceipted.artifact_identity = "mesh.unreceipted";
    unreceipted.artifact_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>("unproven geometry"), 17U));
    const auto unproven = ui.dispatch(carto::application::CreateMeshObjectAction{
        "Unproven botanical", carto::geometry::EditableMesh{}, unreceipted});
    REQUIRE(!unproven);
    REQUIRE(unproven.error().code == carto::core::ErrorCode::unsupported);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.snapshot().objects.empty());

    auto cross_org_job = job;
    cross_org_job.organization_scope = "other-organization";
    const auto before_cross_org = ui.snapshot();
    const auto cross_org = ui.import_generated_botanical_gltf(
        payload, source, cross_org_job, "fixture-organization", "Cross Org Pine");
    REQUIRE(!cross_org);
    REQUIRE(cross_org.error().code == carto::core::ErrorCode::validation_failed);
    const auto after_cross_org = ui.snapshot();
    REQUIRE(after_cross_org.project_revision == before_cross_org.project_revision);
    REQUIRE(after_cross_org.objects.size() == before_cross_org.objects.size());
    REQUIRE(after_cross_org.source_assets.size() == before_cross_org.source_assets.size());
    REQUIRE(after_cross_org.operations.size() == before_cross_org.operations.size());

    const auto missing_scope = ui.import_generated_botanical_gltf(
        payload, source, job, {}, "No Scope Pine");
    REQUIRE(!missing_scope);
    REQUIRE(missing_scope.error().code == carto::core::ErrorCode::validation_failed);
    REQUIRE(ui.snapshot().project_revision == before_cross_org.project_revision);

    auto stale_job = job;
    stale_job.source_revision = carto::core::Revision{8U};
    REQUIRE(!ui.import_generated_botanical_gltf(payload, source, stale_job,
        "fixture-organization", "Stale Pine"));
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    REQUIRE(ui.snapshot().objects.empty());
    REQUIRE(ui.snapshot().source_assets.empty());

    for (const std::string_view terminal_status : {
        std::string_view{"cancelled"}, std::string_view{"failed"},
        std::string_view{"cancel_requested"}}) {
        auto non_successful_job = job;
        non_successful_job.status = terminal_status;
        const auto before_non_successful_promotion = ui.snapshot();
        const auto rejected = ui.import_generated_botanical_gltf(
            payload, source, non_successful_job, "fixture-organization", "Unpromoted Pine");
        REQUIRE(!rejected);
        const auto after_non_successful_promotion = ui.snapshot();
        REQUIRE(after_non_successful_promotion.project_revision ==
            before_non_successful_promotion.project_revision);
        REQUIRE(after_non_successful_promotion.operations.size() ==
            before_non_successful_promotion.operations.size());
        REQUIRE(after_non_successful_promotion.objects.size() ==
            before_non_successful_promotion.objects.size());
        REQUIRE(after_non_successful_promotion.source_assets.size() ==
            before_non_successful_promotion.source_assets.size());
    }

    const auto promoted = ui.import_generated_botanical_gltf(
        payload, source, job, "fixture-organization", "Pine candidate");
    if (!promoted) {
        throw TestFailure("generated botanical promotion failed: " +
            promoted.error().message);
    }
    REQUIRE(promoted.value().action == "Import Source Mesh");
    REQUIRE(promoted.value().document_changed);
    REQUIRE(promoted.value().source == carto::application::OperationSource::human);
    auto after = ui.snapshot();
    REQUIRE(after.objects.size() == 1U);
    REQUIRE(after.source_assets.size() == 1U);
    REQUIRE(after.operations.back().action == "Import Source Mesh");
    REQUIRE(after.operations.back().category == carto::ui::OperationCategory::committed_command);

    const auto source_record = session.snapshot().source_assets.front();
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    const auto& records = reopened.value().source_asset_registry().records();
    const auto record = records.find(source_record.identity);
    REQUIRE(record != records.end());
    REQUIRE(record->second.botanical_generation.has_value());
    REQUIRE(record->second.botanical_generation->source_snapshot == source_snapshot);
    REQUIRE(record->second.botanical_generation->input_artifact_digest == source_digest);
    REQUIRE(record->second.botanical_generation->output_artifact_digest == job.output_digest);
    REQUIRE(record->second.botanical_generation->job_identity_digest ==
        carto::assets::sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(job.job_id.data()), job.job_id.size())));
    const auto persisted_project = reopened.value().serialize();
    REQUIRE(persisted_project.find(job.job_id) == std::string::npos);
    REQUIRE(persisted_project.find(job.source_artifact_id) == std::string::npos);
    REQUIRE(persisted_project.find(job.output_artifact_id) == std::string::npos);
    REQUIRE(persisted_project.find("fixture-organization") == std::string::npos);
    REQUIRE(persisted_project.find("fixture-function-key") == std::string::npos);
    REQUIRE(persisted_project.find("fixture-bearer-token") == std::string::npos);
    REQUIRE(persisted_project.find("https://hub.example.test") == std::string::npos);

    carto::assets::BlobStore source_blobs(
        project_path.parent_path() / ".carto-assets" / "source-blobs");
    const auto stored = source_blobs.read(job.output_digest);
    REQUIRE(stored && stored.value() == payload);

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    after = ui.snapshot();
    REQUIRE(after.objects.empty());
    REQUIRE(after.source_assets.empty());
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    after = ui.snapshot();
    REQUIRE(after.objects.size() == 1U);
    REQUIRE(after.source_assets.size() == 1U);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto round_trip = carto::project::ProjectDocument::load(project_path);
    REQUIRE(round_trip && round_trip.value().source_asset_registry().records().at(
        source_record.identity).botanical_generation.has_value());

    auto advanced_authoring = session.condition_authoring();
    REQUIRE(advanced_authoring);
    auto next_authoring = advanced_authoring.value();
    next_authoring.project_revision = next_authoring.project_revision.next();
    REQUIRE(ui.dispatch(carto::application::ImportConditionAuthoringAction{
        next_authoring.serialize()}));
    const auto before_stale_promotion = ui.snapshot();
    const auto stale_promotion = ui.import_generated_botanical_gltf(
        payload, source, job, "fixture-organization", "Stale Pine");
    REQUIRE(!stale_promotion);
    REQUIRE(stale_promotion.error().code == carto::core::ErrorCode::stale_data);
    const auto after_stale_promotion = ui.snapshot();
    REQUIRE(after_stale_promotion.project_revision == before_stale_promotion.project_revision);
    REQUIRE(after_stale_promotion.operations.size() == before_stale_promotion.operations.size());
    REQUIRE(after_stale_promotion.objects.size() == before_stale_promotion.objects.size());
    REQUIRE(after_stale_promotion.source_assets.size() == before_stale_promotion.source_assets.size());
}

void ui_promotes_private_botanical_glb_with_bound_human_receipts() {
    TempDirectory temp;
    const auto project_path = temp.path() / "botanical-glb-promotion.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::ImportConditionFixtureAction{}));
    auto authoring = session.condition_authoring();
    REQUIRE(authoring);
    auto next_authoring = authoring.value();
    next_authoring.project_revision = next_authoring.project_revision.next();
    REQUIRE(ui.dispatch(carto::application::ImportConditionAuthoringAction{
        next_authoring.serialize()}));
    authoring = session.condition_authoring();
    REQUIRE(authoring);

    carto::botanical::Source source;
    source.identity = "pine-glb-promotion-fixture";
    source.revision = authoring.value().project_revision;
    const std::string source_snapshot = source.serialize();
    const auto source_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(source_snapshot.data()),
        source_snapshot.size()));
    const auto payload = botanical_glb_fixture(source.identity, source.revision.value());
    const auto output_digest = carto::assets::sha256(payload);
    auto transport = std::make_shared<BotanicalPromotionTransport>(
        payload, source.revision, nullptr, "request-pine-glb-7",
        std::string(carto::runtime_api::kBotanicalGlbOutputKind));
    carto::runtime_api::Client runtime_client(
        carto::runtime_api::Config{
            "https://hub.example.test", "fixture-function-key",
            "fixture-bearer-token", "fixture-organization"},
        transport);

    const auto submitted = runtime_client.submit_botanical_source(
        source.identity, source.revision, source_snapshot,
        "Admit the bounded GLB fixture.", "request-pine-glb-7");
    REQUIRE(submitted && submitted.value().status == "queued");
    const auto completed = runtime_client.get_job(submitted.value());
    REQUIRE(completed && completed.value().status == "succeeded");
    REQUIRE(completed.value().output_kind == carto::runtime_api::kBotanicalGlbOutputKind);
    REQUIRE(completed.value().output_digest == output_digest);
    const auto downloaded = runtime_client.download_output(completed.value());
    REQUIRE(downloaded && downloaded.value() == payload);
    REQUIRE(transport->requests.size() == 4U);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto before = ui.snapshot();
    const auto promoted = ui.import_generated_botanical_gltf(
        downloaded.value(), source, completed.value(),
        "fixture-organization", "Private GLB Pine");
    REQUIRE(promoted && promoted.value().document_changed &&
            promoted.value().source == carto::application::OperationSource::human &&
            promoted.value().action == "Import Source Mesh");
    const auto after = ui.snapshot();
    REQUIRE(after.project_revision > before.project_revision &&
            after.objects.size() == 1U && after.source_assets.size() == 1U &&
            after.operations.back().action == "Import Source Mesh");

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    const auto& records = reopened.value().source_asset_registry().records();
    REQUIRE(records.size() == 1U);
    const auto& record = records.begin()->second;
    REQUIRE(record.importer_identity == "carto.import.generated-glb" &&
            record.source_digest == output_digest &&
            record.botanical_generation.has_value());
    REQUIRE(record.botanical_generation->source_snapshot == source_snapshot &&
            record.botanical_generation->input_artifact_digest == source_digest &&
            record.botanical_generation->output_artifact_digest == output_digest &&
            record.botanical_generation->job_identity_digest ==
                carto::assets::sha256(std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(completed.value().job_id.data()),
                    completed.value().job_id.size())));
    carto::assets::BlobStore source_blobs(
        project_path.parent_path() / ".carto-assets" / "source-blobs");
    const auto stored = source_blobs.read(output_digest);
    REQUIRE(stored && stored.value() == payload);
}

void ui_promotes_real_botanical_artifact_when_configured() {
    const char* artifact_path = std::getenv(
        "CARTOGRAPHER_GENERATED_BOTANICAL_UI_TEST_ARTIFACT");
    if (artifact_path == nullptr || *artifact_path == '\0') return;
    const char* receipt_path = std::getenv(
        "CARTOGRAPHER_GENERATED_BOTANICAL_UI_TEST_RECEIPT");
    REQUIRE(receipt_path != nullptr && *receipt_path != '\0');

    std::ifstream artifact_file(artifact_path, std::ios::binary);
    REQUIRE(static_cast<bool>(artifact_file));
    std::string artifact_text;
    artifact_text.assign(std::istreambuf_iterator<char>(artifact_file),
        std::istreambuf_iterator<char>());
    const std::vector<std::uint8_t> payload(artifact_text.begin(),
        artifact_text.end());
    REQUIRE(!payload.empty());

    std::ifstream receipt_file(receipt_path, std::ios::binary);
    REQUIRE(static_cast<bool>(receipt_file));
    const std::string receipt_text{
        std::istreambuf_iterator<char>(receipt_file), std::istreambuf_iterator<char>()};
    std::istringstream receipt_lines(receipt_text);
    std::map<std::string, std::string> receipt_fields;
    for (std::string line; std::getline(receipt_lines, line);) {
        const auto separator = line.find('=');
        REQUIRE(separator != std::string::npos && separator != 0U);
        REQUIRE(receipt_fields.emplace(line.substr(0U, separator),
            line.substr(separator + 1U)).second);
    }
    REQUIRE(receipt_fields.size() == 10U);

    TempDirectory temp;
    const auto project_path = temp.path() / "generated-botanical.carto";
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::ImportConditionFixtureAction{}));

    auto authoring = session.condition_authoring();
    REQUIRE(authoring);
    REQUIRE(authoring.value().project_revision.value() <= 7U);
    while (authoring.value().project_revision.value() < 7U) {
        auto next = authoring.value();
        next.project_revision = next.project_revision.next();
        REQUIRE(ui.dispatch(carto::application::ImportConditionAuthoringAction{
            next.serialize()}));
        authoring = session.condition_authoring();
        REQUIRE(authoring);
    }

    carto::botanical::Source source;
    source.identity = "botanical-fixture";
    source.revision = authoring.value().project_revision;
    const std::string source_snapshot = source.serialize();
    const auto source_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(source_snapshot.data()),
        source_snapshot.size()));
    const auto output_digest = carto::assets::sha256(std::span<const std::uint8_t>(
        payload.data(), payload.size()));

    REQUIRE(receipt_fields.at("status") == "succeeded");
    REQUIRE(receipt_fields.at("request_id") == "native-botanical-job-001");
    REQUIRE(receipt_fields.at("source_identity") == source.identity);
    REQUIRE(receipt_fields.at("source_revision") ==
        std::to_string(source.revision.value()));
    REQUIRE(receipt_fields.at("source_digest") == source_digest.hex());
    REQUIRE(receipt_fields.at("output_digest") == output_digest.hex());
    REQUIRE(receipt_fields.at("output_kind") == "gltf-v1");

    carto::runtime_api::JobReceipt job;
    job.job_id = receipt_fields.at("job_id");
    job.status = receipt_fields.at("status");
    job.source_identity = source.identity;
    job.source_revision = source.revision;
    job.source_artifact_id = receipt_fields.at("source_artifact_id");
    job.source_digest = source_digest;
    job.output_artifact_id = receipt_fields.at("output_artifact_id");
    job.output_digest = output_digest;
    job.output_kind = receipt_fields.at("output_kind");
    job.organization_scope = "hidden-canopy";

    constexpr std::string_view replay_justification =
        "Generate this reviewed botanical fixture for import acceptance.";
    auto transport = std::make_shared<BotanicalPromotionTransport>(
        payload, source.revision, &job, receipt_fields.at("request_id"));
    carto::runtime_api::Client runtime_client(carto::runtime_api::Config{
        "https://hub.example.test", "fixture-function-key",
        "user-token-000000000000000000000", "hidden-canopy"}, transport);
    REQUIRE(transport->requests.empty());
    const auto submitted = runtime_client.submit_botanical_source(source.identity,
        source.revision, source_snapshot, replay_justification,
        receipt_fields.at("request_id"));
    REQUIRE(submitted && submitted.value().job_id == job.job_id &&
        submitted.value().status == "queued");
    REQUIRE(submitted.value().source_artifact_id == job.source_artifact_id);
    REQUIRE(transport->requests.size() == 2U);
    REQUIRE(runtime_text(transport->requests[1U].body).find(replay_justification) !=
        std::string::npos);
    const auto completed = runtime_client.get_job(submitted.value());
    REQUIRE(completed && completed.value().status == "succeeded");
    REQUIRE(completed.value().output_digest == job.output_digest);
    const auto downloaded = runtime_client.download_output(completed.value());
    REQUIRE(downloaded && downloaded.value() == payload);
    REQUIRE(transport->requests.size() == 4U);
    job = completed.value();

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto before = ui.snapshot();
    const auto promoted = ui.import_generated_botanical_gltf(
        payload, source, job, "hidden-canopy", "Generated conifer");
    if (!promoted) {
        throw TestFailure("real botanical artifact promotion failed: " +
            promoted.error().message);
    }
    REQUIRE(promoted.value().document_changed);
    REQUIRE(promoted.value().source == carto::application::OperationSource::human);
    const auto after = ui.snapshot();
    REQUIRE(after.project_revision > before.project_revision);
    REQUIRE(after.objects.size() == 1U);
    REQUIRE(after.source_assets.size() == 1U);

    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{}));
    const auto reopened = carto::project::ProjectDocument::load(project_path);
    REQUIRE(reopened);
    const auto& records = reopened.value().source_asset_registry().records();
    const auto record = records.find(after.source_assets.front().identity);
    REQUIRE(record != records.end());
    REQUIRE(record->second.botanical_generation.has_value());
    REQUIRE(record->second.botanical_generation->source_snapshot == source_snapshot);
    REQUIRE(record->second.botanical_generation->input_artifact_digest == source_digest);
    REQUIRE(record->second.botanical_generation->output_artifact_digest == job.output_digest);
    const auto serialized_project = reopened.value().serialize();
    REQUIRE(serialized_project.find(job.job_id) == std::string::npos);
    REQUIRE(serialized_project.find(job.source_artifact_id) == std::string::npos);
    REQUIRE(serialized_project.find(job.output_artifact_id) == std::string::npos);
    REQUIRE(serialized_project.find("fixture-function-key") == std::string::npos);
    REQUIRE(serialized_project.find("user-token-000000000000000000000") ==
        std::string::npos);
    REQUIRE(serialized_project.find("https://hub.example.test") == std::string::npos);
    REQUIRE(serialized_project.find("hidden-canopy") == std::string::npos);

    const auto mesh_asset = after.objects.front().object.mesh_asset;
    REQUIRE(mesh_asset.has_value());
    const auto imported_mesh = reopened.value().meshes().find(*mesh_asset);
    REQUIRE(imported_mesh != reopened.value().meshes().end());
    const auto topology = imported_mesh->second.topology();
    REQUIRE(topology);
    const auto* uv_layer = imported_mesh->second.attributes().find("cartographer.uv0");
    const auto* material_layer = imported_mesh->second.attributes().find("gltf.material_index");
    REQUIRE(uv_layer != nullptr && material_layer != nullptr);
    REQUIRE(uv_layer->values.size() == topology.value().corners.size());
    REQUIRE(material_layer->values.size() == imported_mesh->second.face_count());
    const auto* active_uv = imported_mesh->second.uv_sets().active_for_render();
    REQUIRE(active_uv != nullptr && active_uv->layer_id == "cartographer.uv0");
    for (const auto& value : uv_layer->values) {
        const auto* uv = std::get_if<carto::core::Vec2d>(&value);
        REQUIRE(uv != nullptr && uv->x >= 0.0 && uv->x <= 1.0 &&
            uv->y >= 0.0 && uv->y <= 1.0);
    }
    bool has_stem = false;
    bool has_foliage = false;
    for (const auto& value : material_layer->values) {
        const auto* material = std::get_if<std::uint32_t>(&value);
        REQUIRE(material != nullptr && *material <= 1U);
        has_stem = has_stem || *material == 0U;
        has_foliage = has_foliage || *material == 1U;
    }
    REQUIRE(has_stem && has_foliage);

    carto::assets::BlobStore source_blobs(
        project_path.parent_path() / ".carto-assets" / "source-blobs");
    const auto stored = source_blobs.read(job.output_digest);
    REQUIRE(stored && stored.value() == payload);
}

void ui_source_mesh_reimport_updates_in_place_and_keeps_monotonic_undo_lineage() {
    TempDirectory temp;
    const auto project_path = temp.path() / "source-reimport.carto";
    const auto source_path = temp.path() / "editable.obj";
    const auto write_triangle = [&source_path](double z) {
        std::ofstream source(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(source));
        source << "v 0 0 0\nv 1 0 0\nv 0 1 " << z << "\nf 1 2 3\n";
    };
    write_triangle(0.0);

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto first = ui.import_mesh(source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(first && first.value().source_linked);
    REQUIRE(ui.snapshot().objects.size() == 1U);
    const auto original_mesh_asset = ui.snapshot().objects.front().object.mesh_asset;
    REQUIRE(original_mesh_asset.has_value());
    const auto first_source = ui.snapshot().source_assets.front();
    REQUIRE(first_source.source_generation == carto::core::Revision{1U});

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.snapshot().objects.empty());
    REQUIRE(ui.snapshot().source_assets.empty());
    REQUIRE(ui.snapshot().source_asset_tombstone_count == 1U);
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().objects.front().object.mesh_asset == original_mesh_asset);
    const auto original_source = ui.snapshot().source_assets.front();
    REQUIRE(original_source.source_generation == carto::core::Revision{2U});
    REQUIRE(original_source.source_digest == first_source.source_digest);

    const auto before_reimport = ui.snapshot();
    const auto wrong_importer = ui.preview_source_mesh_reimport(
        first.value().source_identity, source_path,
        carto::ui::MeshImportFormat::ply,
        before_reimport.project_generation, before_reimport.project_revision);
    REQUIRE(!wrong_importer &&
        wrong_importer.error().code == carto::core::ErrorCode::unsupported);
    const auto stale_reimport = ui.preview_source_mesh_reimport(
        first.value().source_identity, source_path,
        carto::ui::MeshImportFormat::obj,
        before_reimport.project_generation + 1U, before_reimport.project_revision);
    REQUIRE(!stale_reimport &&
        stale_reimport.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);
    REQUIRE(ui.snapshot().source_assets.front().source_generation ==
        before_reimport.source_assets.front().source_generation);

    write_triangle(1.0);
    const auto before_review = session.snapshot();
    const auto unreviewed_reimport = ui.import_mesh(
        source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(!unreviewed_reimport &&
        unreviewed_reimport.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);
    const auto review = ui.preview_source_mesh_reimport(
        first.value().source_identity, source_path,
        carto::ui::MeshImportFormat::obj,
        before_reimport.project_generation, before_reimport.project_revision);
    REQUIRE(review);
    REQUIRE(review.value().current_vertex_count == std::optional<std::size_t>{3U});
    REQUIRE(review.value().current_face_count == std::optional<std::size_t>{1U});
    REQUIRE(review.value().candidate_vertex_count == 3U);
    REQUIRE(review.value().candidate_face_count == 1U);
    REQUIRE(review.value().candidate_triangle_count == 1U);
    REQUIRE(review.value().candidate_mesh_preview != nullptr);
    REQUIRE(review.value().candidate_mesh_preview->valid());
    REQUIRE(review.value().candidate_source_digest != review.value().source_digest);
    REQUIRE(review.value().candidate_artifact_digest != review.value().artifact_digest);
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);

    write_triangle(2.0);
    const auto changed_after_review = ui.apply_source_mesh_reimport(
        review.value(), source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(!changed_after_review &&
        changed_after_review.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == before_reimport.project_revision);
    REQUIRE(ui.snapshot().source_assets.front().source_generation ==
        before_reimport.source_assets.front().source_generation);

    REQUIRE(session.snapshot().undo_count == before_review.undo_count &&
        session.snapshot().redo_count == before_review.redo_count);
    write_triangle(1.0);
    const auto changed = ui.apply_source_mesh_reimport(
        review.value(), source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(changed && changed.value().dispatch.document_changed);
    REQUIRE(changed.value().source_identity == first.value().source_identity);
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().objects.front().object.mesh_asset == original_mesh_asset);
    const auto changed_source = ui.snapshot().source_assets.front();
    REQUIRE(changed_source.source_generation == carto::core::Revision{3U});
    REQUIRE(changed_source.source_digest != original_source.source_digest);
    REQUIRE(changed_source.artifact_digest != original_source.artifact_digest);
    REQUIRE(changed_source.importer_version == original_source.importer_version);
    REQUIRE(changed_source.options == original_source.options);
    const auto changed_revision = ui.snapshot().project_revision;

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(ui.snapshot().project_revision > changed_revision);
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().objects.front().object.mesh_asset == original_mesh_asset);
    const auto undone_source = ui.snapshot().source_assets.front();
    REQUIRE(undone_source.source_generation == carto::core::Revision{4U});
    REQUIRE(undone_source.source_digest == original_source.source_digest);
    REQUIRE(undone_source.artifact_digest == original_source.artifact_digest);

    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::redo));
    const auto redone_source = ui.snapshot().source_assets.front();
    REQUIRE(redone_source.source_generation == carto::core::Revision{5U});
    REQUIRE(redone_source.source_digest == changed_source.source_digest);
    REQUIRE(redone_source.artifact_digest == changed_source.artifact_digest);
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().objects.front().object.mesh_asset == original_mesh_asset);
}

void ui_unsaved_mesh_import_remains_available_but_is_explicitly_unlinked() {
    TempDirectory temp;
    const auto source_path = temp.path() / "triangle.obj";
    {
        std::ofstream source(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(source));
        source << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    const auto imported = ui.import_mesh(source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported);
    REQUIRE(!imported.value().source_linked);
    REQUIRE(imported.value().source_identity.empty());
    REQUIRE(std::any_of(imported.value().warnings.begin(), imported.value().warnings.end(),
        [](const std::string& warning) {
            return warning.find("save the project and import again") != std::string::npos;
        }));
    REQUIRE(ui.snapshot().objects.size() == 1U);
    REQUIRE(ui.snapshot().source_assets.empty());
}

void ui_rejects_tampered_source_blob_without_fallback_or_project_mutation() {
    TempDirectory temp;
    const auto project_path = temp.path() / "tamper-check.carto";
    const auto source_path = temp.path() / "triangle.obj";
    {
        std::ofstream source(source_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(source));
        source << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{project_path}));
    const auto imported = ui.import_mesh(source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(imported && imported.value().source_linked);
    const auto source_view = ui.snapshot().source_assets.front();
    const auto blob_path = project_path.parent_path() / source_view.relative_source_path;
    {
        std::ofstream corrupt(blob_path, std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(corrupt));
        corrupt << "tampered bytes";
    }
    const auto revision_before = ui.snapshot().project_revision;
    const auto objects_before = ui.snapshot().objects.size();
    const auto reimport = ui.import_mesh(source_path, carto::ui::MeshImportFormat::obj);
    REQUIRE(!reimport);
    REQUIRE(reimport.error().code == carto::core::ErrorCode::validation_failed);
    REQUIRE(ui.snapshot().project_revision == revision_before);
    REQUIRE(ui.snapshot().objects.size() == objects_before);
    REQUIRE(ui.snapshot().source_assets.front().source_digest == source_view.source_digest);
    REQUIRE(std::filesystem::is_empty(
        project_path.parent_path() / ".carto-assets" / "import-staging"));
}

void ui_routes_source_reimport_review_and_keeps_ai_out_of_mutation_path() {
    const auto digest = [](std::string_view value) {
        return carto::assets::sha256(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    };
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    carto::assets::SourceImportRecord source;
    source.identity = "asset.fixture";
    source.relative_source_path = "Assets/fixture.obj";
    source.source_generation = carto::core::Revision{1U};
    source.kind = carto::assets::SourceAssetKind::mesh;
    source.source_digest = digest("source-1");
    source.importer_identity = "carto.import.obj";
    source.importer_version = 1U;
    source.artifact_identity = "mesh.fixture";
    source.artifact_digest = digest("geometry-1");
    source.domain_digests = {{
        carto::assets::SourceAssetDomain::geometry, digest("geometry-1")}};

    const auto plan = ui.preview_source_reimport({source});
    REQUIRE(plan && plan.value().entries.size() == 1U);
    const auto imported = ui.apply_source_reimport_plan(plan.value());
    REQUIRE(imported && imported.value().document_changed);
    REQUIRE(ui.snapshot().source_assets.size() == 1U);

    auto changed = source;
    changed.source_generation = carto::core::Revision{2U};
    changed.source_digest = digest("source-2");
    changed.artifact_digest = digest("geometry-2");
    changed.domain_digests.front().digest = digest("geometry-2");
    const auto update_plan = ui.preview_source_reimport({changed});
    REQUIRE(update_plan && update_plan.value().entries.front().change ==
        carto::assets::SourceReimportChange::geometry_changed);
    const auto raw_ai = ui.submit_ai_proposal(
        carto::application::ApplySourceReimportPlanAction{update_plan.value()});
    REQUIRE(!raw_ai && raw_ai.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(ui.snapshot().source_assets.front().source_generation ==
        carto::core::Revision{1U});

    auto proposed_mesh = carto::geometry::make_box({1.0, 1.0, 1.0});
    REQUIRE(proposed_mesh);
    carto::assets::SourceImportRecord proposed_source;
    proposed_source.identity = "asset.ai_attempt";
    proposed_source.relative_source_path = ".carto-assets/source-blobs/sha256/ai";
    proposed_source.source_generation = carto::core::Revision{1U};
    proposed_source.kind = carto::assets::SourceAssetKind::mesh;
    proposed_source.source_digest = digest("ai-source");
    proposed_source.importer_identity = "carto.import.obj";
    proposed_source.artifact_identity = "pending.mesh";
    proposed_source.artifact_digest =
        carto::application::canonical_mesh_geometry_digest(proposed_mesh.value());
    proposed_source.domain_digests = {{
        carto::assets::SourceAssetDomain::geometry, proposed_source.artifact_digest}};
    const auto raw_ai_mesh = ui.submit_ai_proposal(
        carto::application::CreateMeshObjectAction{
            "AI Source Mesh", std::move(proposed_mesh.value()), proposed_source});
    REQUIRE(!raw_ai_mesh && raw_ai_mesh.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(ui.snapshot().source_assets.size() == 1U);
    REQUIRE(ui.snapshot().objects.empty());

    const auto metadata_base = ui.snapshot();
    const auto metadata_review = ui.preview_source_asset_metadata(source.identity,
        carto::core::Revision{1U}, metadata_base.project_generation,
        metadata_base.project_revision, {"canopy", "exterior"}, {});
    REQUIRE(metadata_review && metadata_review.value().entries.front().incoming.has_value());
    REQUIRE(ui.snapshot().project_revision == metadata_base.project_revision);
    const auto metadata_applied = ui.apply_source_reimport_plan(metadata_review.value());
    REQUIRE(metadata_applied && metadata_applied.value().document_changed);
    const auto metadata_after = ui.snapshot();
    REQUIRE(metadata_after.source_assets.front().source_generation ==
        carto::core::Revision{2U});
    REQUIRE((metadata_after.source_assets.front().authored_tags ==
        std::vector<std::string>{"canopy", "exterior"}));
    REQUIRE(!ui.apply_source_reimport_plan(metadata_review.value()));
    REQUIRE(metadata_after.project_revision == ui.snapshot().project_revision);
}

void ui_connects_local_market_boundary_and_fails_closed() {
    TempDirectory temp;
    const auto package = temp.path() / "package";
    const auto library = temp.path() / "library";
    std::filesystem::create_directories(package);
    {
        std::ofstream payload(package / "geometry.bin", std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(payload));
        payload << "cartographer-data-only-payload";
    }

    carto::market::MarketPackageManifest manifest;
    manifest.package_id = "sample.geometry";
    manifest.listing_id = "sample-listing";
    manifest.listing_version_id = "1.0.0";
    manifest.publisher_id = "local-publisher";
    manifest.license_id = "cc0-1.0";
    manifest.entrypoints.push_back({"asset", "geometry.bin"});
    const auto content_digest = carto::market::MarketPackageValidator::directory_payload_digest(package);
    REQUIRE(content_digest);
    manifest.content_digest = content_digest.value();
    {
        std::ofstream descriptor(package / "manifest.json", std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(descriptor));
        descriptor << manifest.serialize();
    }

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.snapshot().market.status == carto::ui::MarketStatus::unconfigured);
    REQUIRE(ui.configure_market_library(library));
    REQUIRE(ui.snapshot().market.status == carto::ui::MarketStatus::empty);
    REQUIRE(ui.snapshot().market.packages.empty());
    const auto refresh_command = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "market.refresh"; });
    REQUIRE(refresh_command != ui.snapshot().commands.end());
    REQUIRE(refresh_command->supported);

    const auto installed = ui.install_market_directory(package);
    REQUIRE(installed);
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.market.status == carto::ui::MarketStatus::ready);
    REQUIRE(snapshot.market.packages.size() == 1U);
    REQUIRE(snapshot.market.packages.front().package_id == "sample.geometry");
    REQUIRE(snapshot.market.packages.front().listing_version_id == "1.0.0");
    REQUIRE(!snapshot.market.packages.front().publication_receipt_bound);
    REQUIRE(snapshot.market.packages.front().package_digest.starts_with("sha256:"));

    const auto installed_payload = installed.value().install_root / "geometry.bin";
    {
        std::ofstream tampered(installed_payload, std::ios::binary | std::ios::app);
        REQUIRE(static_cast<bool>(tampered));
        tampered << "tampered";
    }
    const auto refresh = ui.refresh_market_library();
    REQUIRE(!refresh);
    REQUIRE(ui.snapshot().market.status == carto::ui::MarketStatus::error);
    REQUIRE(ui.snapshot().market.packages.empty());
}

void ui_failed_shortcuts_and_actions_remain_bounded() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(!ui.handle_shortcut(carto::ui::Shortcut::undo));
    REQUIRE(!ui.handle_shortcut(carto::ui::Shortcut::redo));
    REQUIRE(!ui.handle_shortcut(carto::ui::Shortcut::save));
    REQUIRE(!ui.dispatch(carto::application::SelectObjectAction{carto::scene::ObjectId{999U}}));
    REQUIRE(ui.snapshot().operations.empty());
    REQUIRE(!ui.snapshot().problems.empty());
}

void ui_command_palette_is_bounded_and_explicit_about_deferred_features() {
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::toggle_command_palette));
    REQUIRE(ui.snapshot().command_palette_open);
    REQUIRE(ui.set_command_palette_query("face"));
    REQUIRE(ui.snapshot().command_palette_query == "face");
    const std::string oversized(1025U, 'x');
    REQUIRE(!ui.set_command_palette_query(oversized));
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::close_overlay));
    REQUIRE(!ui.snapshot().command_palette_open);
    const auto edge = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "mode.edge"; });
    REQUIRE(edge != ui.snapshot().commands.end());
    REQUIRE(edge->supported);
    const auto repeat = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "edit.repeat-last"; });
    REQUIRE(repeat != ui.snapshot().commands.end());
    REQUIRE(repeat->shortcut == "Ctrl+R");
    const auto import_mesh = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "file.import-mesh"; });
    REQUIRE(import_mesh != ui.snapshot().commands.end());
    REQUIRE(import_mesh->supported);
    REQUIRE(import_mesh->shortcut == "Ctrl+I");
    const auto duplicate = std::find_if(
        ui.snapshot().commands.begin(), ui.snapshot().commands.end(),
        [](const auto& command) { return command.id == "edit.duplicate"; });
    REQUIRE(duplicate != ui.snapshot().commands.end());
    REQUIRE(duplicate->supported);
    REQUIRE(duplicate->shortcut == "Ctrl+D");
    REQUIRE(ui.handle_shortcut(carto::ui::Shortcut::edge_mode));
    REQUIRE(ui.snapshot().selection.mode == carto::editor::SelectionMode::edge);
    REQUIRE(ui.snapshot().project_revision == carto::core::Revision{});
}

void native_key_contract_maps_core_control_without_imgui() {
    using carto::ui::NativeKey;
    using carto::ui::NativeKeyEvent;
    using carto::ui::Shortcut;

    const auto save = carto::ui::shortcut_for_native_key({
        NativeKey::s, carto::ui::kNativeModifierControl, true, false});
    REQUIRE(save.has_value() && *save == Shortcut::save);
    const auto undo_repeat = carto::ui::shortcut_for_native_key({
        NativeKey::z, carto::ui::kNativeModifierControl, true, true});
    REQUIRE(undo_repeat.has_value() && *undo_repeat == Shortcut::undo);
    const auto repeat_last = carto::ui::shortcut_for_native_key({
        NativeKey::r, carto::ui::kNativeModifierControl, true, false});
    REQUIRE(repeat_last.has_value() && *repeat_last == Shortcut::repeat_last_tool);
    const auto face = carto::ui::shortcut_for_native_key({
        NativeKey::digit_4, 0U, true, false});
    REQUIRE(face.has_value() && *face == Shortcut::face_mode);
    const auto escape = carto::ui::shortcut_for_native_key({
        NativeKey::escape, 0U, true, false});
    REQUIRE(escape.has_value() && *escape == Shortcut::close_overlay);
    REQUIRE(!carto::ui::shortcut_for_native_key({
        NativeKey::s, 0U, true, false}).has_value());
    REQUIRE(!carto::ui::shortcut_for_native_key({
        NativeKey::digit_1, carto::ui::kNativeModifierShift, true, false}).has_value());
    REQUIRE(!carto::ui::shortcut_for_native_key(
        NativeKeyEvent{NativeKey::k, carto::ui::kNativeModifierControl, false, false}).has_value());

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.handle_shortcut(*face));
    REQUIRE(ui.snapshot().selection.mode == carto::editor::SelectionMode::face);

    using carto::ui::NativePointerButton;
    using carto::ui::NativePointerEvent;
    const auto replace = carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240, 0U, true});
    REQUIRE(replace.has_value() && *replace == carto::editor::SelectionOperation::replace);
    const auto add = carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240, carto::ui::kNativeModifierShift, true});
    REQUIRE(add.has_value() && *add == carto::editor::SelectionOperation::add);
    const auto toggle = carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240, carto::ui::kNativeModifierControl, true});
    REQUIRE(toggle.has_value() && *toggle == carto::editor::SelectionOperation::toggle);
    REQUIRE(!carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::secondary, 120, 240, 0U, true}).has_value());
    REQUIRE(!carto::ui::selection_operation_for_native_pointer({
        NativePointerButton::primary, 120, 240,
        static_cast<std::uint8_t>(carto::ui::kNativeModifierShift |
                                   carto::ui::kNativeModifierControl), true}).has_value());
    REQUIRE(!carto::ui::selection_operation_for_native_pointer(
        NativePointerEvent{NativePointerButton::primary, 120, 240, 0U, false}).has_value());

    using carto::ui::ProjectedEdgePickCandidate;
    using carto::ui::ProjectedEdgePickStatus;
    const std::array duplicate_edge_hits{
        ProjectedEdgePickCandidate{
            carto::scene::ObjectId{7U}, carto::geometry::EdgeId{12U},
            {0.0, 0.0}, {10.0, 0.0}},
        ProjectedEdgePickCandidate{
            carto::scene::ObjectId{7U}, carto::geometry::EdgeId{12U},
            {10.0, 0.0}, {0.0, 0.0}}};
    const auto duplicate_edge_pick = carto::ui::resolve_projected_edge_pick(
        duplicate_edge_hits, {5.0, 0.0});
    REQUIRE(duplicate_edge_pick.status == ProjectedEdgePickStatus::hit);
    REQUIRE(duplicate_edge_pick.object.value == 7U);
    REQUIRE(duplicate_edge_pick.edge.value == 12U);
    REQUIRE(duplicate_edge_pick.distance_pixels == 0.0);

    const std::array separated_edge_hits{
        duplicate_edge_hits.front(),
        ProjectedEdgePickCandidate{
            carto::scene::ObjectId{8U}, carto::geometry::EdgeId{13U},
            {0.0, 4.0}, {10.0, 4.0}}};
    const auto separated_edge_pick = carto::ui::resolve_projected_edge_pick(
        separated_edge_hits, {5.0, 0.0});
    REQUIRE(separated_edge_pick.status == ProjectedEdgePickStatus::hit);
    REQUIRE(separated_edge_pick.object.value == 7U);

    const std::array ambiguous_edge_hits{
        duplicate_edge_hits.front(),
        ProjectedEdgePickCandidate{
            carto::scene::ObjectId{8U}, carto::geometry::EdgeId{13U},
            {0.0, 0.5}, {10.0, 0.5}}};
    REQUIRE(carto::ui::resolve_projected_edge_pick(
        ambiguous_edge_hits, {5.0, 0.0}).status == ProjectedEdgePickStatus::ambiguous);

    const std::array boundary_edge_hit{
        ProjectedEdgePickCandidate{
            carto::scene::ObjectId{7U}, carto::geometry::EdgeId{12U},
            {0.0, 12.0}, {10.0, 12.0}}};
    REQUIRE(carto::ui::resolve_projected_edge_pick(
        boundary_edge_hit, {5.0, 0.0}).status == ProjectedEdgePickStatus::miss);
    REQUIRE(carto::ui::resolve_projected_edge_pick(
        duplicate_edge_hits,
        {std::numeric_limits<double>::quiet_NaN(), 0.0}).status ==
            ProjectedEdgePickStatus::invalid_input);
    REQUIRE(carto::ui::resolve_projected_edge_pick(
        duplicate_edge_hits, {5.0, 0.0}, 0.0).status ==
            ProjectedEdgePickStatus::invalid_input);
}

void workspace_registry_is_canonical_and_fail_closed() {
    const auto& registry = carto::ui::workspace_registry();
    REQUIRE(registry.size() == 11U);
    std::vector<carto::ui::Workspace> seen;
    for (const auto& definition : registry) {
        REQUIRE(definition.validate());
        const std::string availability = definition.availability_summary();
        REQUIRE(!availability.empty());
        if (definition.available) {
            REQUIRE(availability == "Available");
        } else {
            REQUIRE(availability == "Unavailable: " + definition.unavailable_reason);
        }
        REQUIRE(std::find(seen.begin(), seen.end(), definition.workspace) == seen.end());
        seen.push_back(definition.workspace);
        const auto resolved = carto::ui::workspace_definition(definition.workspace);
        REQUIRE(resolved);
        REQUIRE(resolved.value().id == definition.id);
        REQUIRE(resolved.value().label == definition.label);
    }
    REQUIRE(seen.size() == registry.size());

    const auto model = carto::ui::workspace_definition(carto::ui::Workspace::model);
    REQUIRE(model && model.value().available);
    REQUIRE(carto::ui::workspace_supports_scene_object_translation(
        carto::ui::Workspace::model));
    const auto ai = carto::ui::workspace_definition(carto::ui::Workspace::ai);
    REQUIRE(ai && ai.value().available);
    const auto sculpt = carto::ui::workspace_definition(carto::ui::Workspace::sculpt);
    REQUIRE(sculpt && sculpt.value().available);
    const auto uv = carto::ui::workspace_definition(carto::ui::Workspace::uv);
    REQUIRE(uv && uv.value().available);
    const auto engineering = carto::ui::workspace_definition(
        carto::ui::Workspace::engineering);
    REQUIRE(engineering && engineering.value().available);
    const auto condition = carto::ui::workspace_definition(
        carto::ui::Workspace::condition);
    REQUIRE(condition && condition.value().available);
    REQUIRE(carto::ui::workspace_supports_scene_object_translation(
        carto::ui::Workspace::condition));
    REQUIRE(!carto::ui::workspace_supports_scene_object_translation(
        carto::ui::Workspace::engineering));
    REQUIRE(!carto::ui::workspace_supports_scene_object_translation(
        carto::ui::Workspace::sculpt));
    const auto material = carto::ui::workspace_definition(
        carto::ui::Workspace::material);
    REQUIRE(material && material.value().available);
    const auto review = carto::ui::workspace_definition(
        carto::ui::Workspace::review);
    REQUIRE(review && review.value().available);
    const auto future = carto::ui::workspace_definition(carto::ui::Workspace::cad);
    REQUIRE(future && !future.value().available);
    REQUIRE(!future.value().unavailable_reason.empty());
    REQUIRE(!carto::ui::workspace_definition(static_cast<carto::ui::Workspace>(99)));

    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.set_panel_visible(carto::ui::Panel::assets, true));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::ai));
    const auto ai_snapshot = ui.snapshot();
    REQUIRE(ai_snapshot.workspace_kind == carto::ui::Workspace::ai);
    REQUIRE(ai_snapshot.bottom_panel == carto::ui::BottomPanel::graph);
    REQUIRE(std::find(ai_snapshot.visible_panels.begin(), ai_snapshot.visible_panels.end(),
                      carto::ui::Panel::graph) != ai_snapshot.visible_panels.end());
    REQUIRE(std::find(ai_snapshot.visible_panels.begin(), ai_snapshot.visible_panels.end(),
                      carto::ui::Panel::assets) == ai_snapshot.visible_panels.end());

    const auto unavailable = ui.set_workspace(carto::ui::Workspace::cad);
    REQUIRE(!unavailable);
    REQUIRE(unavailable.error().code == carto::core::ErrorCode::invalid_state);
    REQUIRE(ui.snapshot().workspace_kind == carto::ui::Workspace::ai);

    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));
    const auto sculpt_snapshot = ui.snapshot();
    REQUIRE(sculpt_snapshot.workspace_kind == carto::ui::Workspace::sculpt);
    REQUIRE(sculpt_snapshot.bottom_panel == carto::ui::BottomPanel::operations);

    REQUIRE(ui.set_workspace(carto::ui::Workspace::uv));
    const auto uv_snapshot = ui.snapshot();
    REQUIRE(uv_snapshot.workspace_kind == carto::ui::Workspace::uv);
    REQUIRE(uv_snapshot.bottom_panel == carto::ui::BottomPanel::assets);
    REQUIRE(std::find(uv_snapshot.visible_panels.begin(), uv_snapshot.visible_panels.end(),
                      carto::ui::Panel::assets) != uv_snapshot.visible_panels.end());

    REQUIRE(ui.set_workspace(carto::ui::Workspace::engineering));
    const auto engineering_snapshot = ui.snapshot();
    REQUIRE(engineering_snapshot.workspace_kind == carto::ui::Workspace::engineering);
    REQUIRE(engineering_snapshot.bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(std::find(engineering_snapshot.visible_panels.begin(),
                      engineering_snapshot.visible_panels.end(),
                      carto::ui::Panel::operations) != engineering_snapshot.visible_panels.end());

    REQUIRE(ui.set_workspace(carto::ui::Workspace::condition));
    const auto condition_snapshot = ui.snapshot();
    REQUIRE(condition_snapshot.workspace_kind == carto::ui::Workspace::condition);
    REQUIRE(condition_snapshot.bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(std::find(condition_snapshot.visible_panels.begin(),
                      condition_snapshot.visible_panels.end(),
                      carto::ui::Panel::operations) != condition_snapshot.visible_panels.end());

    REQUIRE(ui.set_workspace(carto::ui::Workspace::material));
    const auto material_snapshot = ui.snapshot();
    REQUIRE(material_snapshot.workspace_kind == carto::ui::Workspace::material);
    REQUIRE(material_snapshot.bottom_panel == carto::ui::BottomPanel::assets);
    REQUIRE(std::find(material_snapshot.visible_panels.begin(),
                      material_snapshot.visible_panels.end(),
                      carto::ui::Panel::assets) != material_snapshot.visible_panels.end());

    REQUIRE(ui.set_workspace(carto::ui::Workspace::review));
    const auto review_snapshot = ui.snapshot();
    REQUIRE(review_snapshot.workspace_kind == carto::ui::Workspace::review);
    REQUIRE(review_snapshot.bottom_panel == carto::ui::BottomPanel::problems);
    REQUIRE(std::find(review_snapshot.visible_panels.begin(),
                      review_snapshot.visible_panels.end(),
                      carto::ui::Panel::viewport) != review_snapshot.visible_panels.end());
    REQUIRE(std::find(review_snapshot.visible_panels.begin(),
                      review_snapshot.visible_panels.end(),
                      carto::ui::Panel::problems) != review_snapshot.visible_panels.end());
}

void sculpt_viewport_sampling_preserves_frontmost_surface_and_path_spacing() {
    using carto::core::Vec2d;
    using carto::core::Vec3d;
    using carto::geometry::FaceId;
    using carto::geometry::VertexId;
    using carto::scene::ObjectId;
    using carto::ui::SculptViewportTriangle;

    const ObjectId object{17U};
    const auto make_triangle = [](ObjectId owner, FaceId face, std::uint64_t first_id,
                                  double depth) {
        return SculptViewportTriangle{
            owner,
            face,
            {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
            {VertexId{first_id}, VertexId{first_id + 1U}, VertexId{first_id + 2U}},
            {Vec3d{0.0, depth, 0.0}, Vec3d{10.0, depth, 0.0}, Vec3d{0.0, depth, 10.0}},
            {depth, depth, depth}};
    };
    std::vector<SculptViewportTriangle> triangles{
        make_triangle(object, FaceId{1U}, 1U, 2.0),
        make_triangle(object, FaceId{2U}, 4U, 1.0),
        make_triangle(ObjectId{18U}, FaceId{3U}, 7U, 9.0)};

    const auto hit = carto::ui::sample_sculpt_viewport_surface(
        triangles, object, Vec2d{2.0, 2.0});
    REQUIRE(hit);
    REQUIRE(hit.value().surface_anchor.has_value());
    REQUIRE(hit.value().surface_anchor->face == FaceId{1U});
    REQUIRE(hit.value().position.x == 2.0);
    REQUIRE(hit.value().position.y == 2.0);
    REQUIRE(hit.value().position.z == 2.0);
    REQUIRE(hit.value().surface_anchor->validate());

    const std::vector<Vec2d> direct_path{{1.0, 1.0}, {7.0, 1.0}};
    const std::vector<Vec2d> segmented_path{{1.0, 1.0}, {3.0, 1.0}, {7.0, 1.0}};
    const auto direct = carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, direct_path, 2.0);
    const auto segmented = carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, segmented_path, 2.0);
    REQUIRE(direct);
    REQUIRE(segmented);
    REQUIRE(direct.value() == segmented.value());
    REQUIRE(direct.value().size() == 4U);
    for (const auto& sample : direct.value()) {
        REQUIRE(sample.surface_anchor.has_value());
        REQUIRE(sample.surface_anchor->face == FaceId{1U});
        REQUIRE(sample.position.y == 2.0);
    }

    std::reverse(triangles.begin(), triangles.end());
    const auto reordered_hit = carto::ui::sample_sculpt_viewport_surface(
        triangles, object, Vec2d{2.0, 2.0});
    REQUIRE(reordered_hit);
    REQUIRE(reordered_hit.value() == hit.value());

    const auto miss = carto::ui::sample_sculpt_viewport_surface(
        triangles, object, Vec2d{12.0, 12.0});
    REQUIRE(!miss);
    REQUIRE(miss.error().code == carto::core::ErrorCode::not_found);
    const std::vector<Vec2d> leaving_surface{{1.0, 1.0}, {12.0, 12.0}};
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, leaving_surface, 2.0));
    const std::vector<Vec2d> non_finite_path{{1.0, 1.0},
        {std::numeric_limits<double>::quiet_NaN(), 2.0}};
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, non_finite_path, 2.0));
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, direct_path, 0.0));
    REQUIRE(!carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, direct_path, 1.0e-12));
}

void viewport_surface_pick_resolves_occlusion_and_rejects_ambiguous_source_ties() {
    using carto::core::Vec2d;
    using carto::core::Vec3d;
    using carto::geometry::FaceId;
    using carto::geometry::VertexId;
    using carto::scene::ObjectId;
    using carto::ui::ProjectedSurfaceHitStatus;
    using carto::ui::SculptViewportTriangle;

    const ObjectId selected{17U};
    const ObjectId occluder{18U};
    const auto make_triangle = [](ObjectId owner, FaceId face, std::uint64_t first_id,
                                  double depth) {
        return SculptViewportTriangle{
            owner,
            face,
            {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
            {VertexId{first_id}, VertexId{first_id + 1U}, VertexId{first_id + 2U}},
            {Vec3d{0.0, depth, 0.0}, Vec3d{10.0, depth, 0.0}, Vec3d{0.0, depth, 10.0}},
            {depth, depth, depth}};
    };

    std::vector<SculptViewportTriangle> triangles{
        make_triangle(selected, FaceId{1U}, 1U, 1.0),
        make_triangle(occluder, FaceId{2U}, 4U, 2.0)};
    const auto frontmost = carto::ui::resolve_frontmost_viewport_surface(
        triangles, Vec2d{2.0, 2.0});
    REQUIRE(frontmost);
    REQUIRE(frontmost.value().status == ProjectedSurfaceHitStatus::hit);
    REQUIRE(frontmost.value().object == occluder);
    REQUIRE(frontmost.value().camera_depth == 2.0);
    REQUIRE(frontmost.value().sample.surface_anchor.has_value());
    REQUIRE(frontmost.value().sample.surface_anchor->face == FaceId{2U});

    const SculptViewportTriangle perspective_triangle{
        selected, FaceId{5U},
        {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
        {VertexId{10U}, VertexId{11U}, VertexId{12U}},
        {Vec3d{0.0, 0.0, 0.0}, Vec3d{10.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 10.0}},
        {-1.0, -2.0, -4.0}, true};
    const auto perspective_hit = carto::ui::resolve_frontmost_viewport_surface(
        std::span<const SculptViewportTriangle>{&perspective_triangle, 1U},
        Vec2d{2.0, 2.0});
    REQUIRE(perspective_hit);
    REQUIRE(perspective_hit.value().status == ProjectedSurfaceHitStatus::hit);
    REQUIRE(std::abs(perspective_hit.value().camera_depth + 4.0 / 3.0) < 1.0e-12);
    REQUIRE(perspective_hit.value().sample.surface_anchor.has_value());
    REQUIRE(std::abs(perspective_hit.value().sample.position.x - 4.0 / 3.0) < 1.0e-12);
    REQUIRE(std::abs(perspective_hit.value().sample.position.z - 2.0 / 3.0) < 1.0e-12);
    REQUIRE(std::abs(perspective_hit.value().sample.surface_anchor->barycentric[0U] -
        0.8) < 1.0e-12);

    auto near_clipped_triangle = perspective_triangle;
    near_clipped_triangle.screen_positions = {
        Vec2d{0.0, 0.0}, Vec2d{2.0, 0.0}, Vec2d{0.0, 2.0}};
    near_clipped_triangle.vertices = {
        VertexId{30U}, VertexId{31U}, VertexId{32U}};
    near_clipped_triangle.local_positions = {
        Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 0.0, 0.0}, Vec3d{0.0, 2.0, 0.0}};
    near_clipped_triangle.camera_depth = {-1.0, -3.0, -3.0};
    near_clipped_triangle.screen_source_weights = {{
        {{0.5, 0.0, 0.5}}, {{0.0, 1.0, 0.0}}, {{0.0, 0.0, 1.0}}}};
    const auto clipped_hit = carto::ui::resolve_frontmost_viewport_surface(
        std::span<const SculptViewportTriangle>{&near_clipped_triangle, 1U},
        Vec2d{0.5, 0.5});
    REQUIRE(clipped_hit);
    REQUIRE(clipped_hit.value().status == ProjectedSurfaceHitStatus::hit);
    REQUIRE(clipped_hit.value().sample.surface_anchor.has_value());
    REQUIRE(std::abs(clipped_hit.value().camera_depth + 1.5) < 1.0e-12);
    REQUIRE(std::abs(clipped_hit.value().sample.position.x - 0.25) < 1.0e-12);
    REQUIRE(std::abs(clipped_hit.value().sample.position.y - 1.0) < 1.0e-12);
    REQUIRE(std::abs(clipped_hit.value().sample.surface_anchor->barycentric[0U] -
        0.375) < 1.0e-12);
    REQUIRE(std::abs(clipped_hit.value().sample.surface_anchor->barycentric[1U] -
        0.125) < 1.0e-12);
    REQUIRE(std::abs(clipped_hit.value().sample.surface_anchor->barycentric[2U] -
        0.5) < 1.0e-12);

    auto malformed_clip_mapping = near_clipped_triangle;
    malformed_clip_mapping.screen_source_weights[0U] = {0.2, 0.2, 0.2};
    const auto rejected_clip_mapping = carto::ui::resolve_frontmost_viewport_surface(
        std::span<const SculptViewportTriangle>{&malformed_clip_mapping, 1U},
        Vec2d{0.5, 0.5});
    REQUIRE(!rejected_clip_mapping);
    REQUIRE(rejected_clip_mapping.error().code ==
        carto::core::ErrorCode::validation_failed);

    const SculptViewportTriangle perspective_occluder{
        occluder, FaceId{6U},
        {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
        {VertexId{20U}, VertexId{21U}, VertexId{22U}},
        {Vec3d{0.0, 0.0, 0.0}, Vec3d{10.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 10.0}},
        {-1.2, -1.2, -1.2}, true};
    const std::array<SculptViewportTriangle, 2U> perspective_scene{
        perspective_triangle, perspective_occluder};
    const auto perspective_frontmost = carto::ui::resolve_frontmost_viewport_surface(
        perspective_scene, Vec2d{2.0, 2.0});
    REQUIRE(perspective_frontmost);
    REQUIRE(perspective_frontmost.value().status == ProjectedSurfaceHitStatus::hit);
    REQUIRE(perspective_frontmost.value().object == occluder);
    REQUIRE(std::abs(perspective_frontmost.value().camera_depth + 1.2) < 1.0e-12);
    const auto depth_index = carto::ui::build_projected_surface_depth_index(
        perspective_scene, Vec2d{0.0, 0.0}, Vec2d{10.0, 10.0});
    REQUIRE(depth_index);
    const auto indexed_depth = carto::ui::resolve_frontmost_viewport_depth(
        perspective_scene, depth_index.value(), Vec2d{2.0, 2.0}, 2U);
    REQUIRE(indexed_depth);
    REQUIRE(indexed_depth.value().camera_depth.has_value());
    REQUIRE(std::abs(*indexed_depth.value().camera_depth + 1.2) < 1.0e-12);
    REQUIRE(indexed_depth.value().candidates_checked == 2U);
    const auto indexed_miss = carto::ui::resolve_frontmost_viewport_depth(
        perspective_scene, depth_index.value(), Vec2d{12.0, 12.0}, 0U);
    REQUIRE(indexed_miss);
    REQUIRE(!indexed_miss.value().camera_depth.has_value());
    const auto bounded_depth = carto::ui::resolve_frontmost_viewport_depth(
        perspective_scene, depth_index.value(), Vec2d{2.0, 2.0}, 1U);
    REQUIRE(!bounded_depth);
    REQUIRE(bounded_depth.error().code == carto::core::ErrorCode::unsupported);
    const auto bounded_index = carto::ui::build_projected_surface_depth_index(
        perspective_scene, Vec2d{0.0, 0.0}, Vec2d{10.0, 10.0}, 1U);
    REQUIRE(!bounded_index);
    REQUIRE(bounded_index.error().code == carto::core::ErrorCode::unsupported);
    const auto invalid_extent_index = carto::ui::build_projected_surface_depth_index(
        perspective_scene, Vec2d{0.0, 0.0}, Vec2d{0.0, 10.0});
    REQUIRE(!invalid_extent_index);
    const auto stale_index_query = carto::ui::resolve_frontmost_viewport_depth(
        std::span<const SculptViewportTriangle>{&perspective_triangle, 1U},
        depth_index.value(), Vec2d{2.0, 2.0}, 2U);
    REQUIRE(!stale_index_query);
    auto behind_camera_triangle = perspective_triangle;
    behind_camera_triangle.camera_depth[1U] = 0.0;
    const auto rejected_perspective_hit = carto::ui::resolve_frontmost_viewport_surface(
        std::span<const SculptViewportTriangle>{&behind_camera_triangle, 1U},
        Vec2d{2.0, 2.0});
    REQUIRE(!rejected_perspective_hit);
    REQUIRE(rejected_perspective_hit.error().code ==
        carto::core::ErrorCode::validation_failed);

    std::reverse(triangles.begin(), triangles.end());
    const auto reordered = carto::ui::resolve_frontmost_viewport_surface(
        triangles, Vec2d{2.0, 2.0});
    REQUIRE(reordered);
    REQUIRE(reordered.value().status == ProjectedSurfaceHitStatus::hit);
    REQUIRE(reordered.value().object == occluder);
    REQUIRE(reordered.value().sample.surface_anchor == frontmost.value().sample.surface_anchor);

    triangles[0U] = make_triangle(occluder, FaceId{2U}, 4U, 1.0);
    const auto overlap = carto::ui::resolve_frontmost_viewport_surface(
        triangles, Vec2d{2.0, 2.0});
    REQUIRE(overlap);
    REQUIRE(overlap.value().status == ProjectedSurfaceHitStatus::ambiguous);
    REQUIRE(overlap.value().object.value == 0U);
    REQUIRE(!overlap.value().sample.surface_anchor.has_value());
    const auto cross_object_tie_with_face_collapse =
        carto::ui::resolve_frontmost_viewport_surface(
            triangles, Vec2d{2.0, 2.0}, true);
    REQUIRE(cross_object_tie_with_face_collapse);
    REQUIRE(cross_object_tie_with_face_collapse.value().status ==
        ProjectedSurfaceHitStatus::ambiguous);

    const std::vector<SculptViewportTriangle> same_object_overlap{
        make_triangle(selected, FaceId{1U}, 1U, 1.0),
        make_triangle(selected, FaceId{2U}, 4U, 1.0)};
    const auto same_object_tie = carto::ui::resolve_frontmost_viewport_surface(
        same_object_overlap, Vec2d{2.0, 2.0});
    REQUIRE(same_object_tie);
    REQUIRE(same_object_tie.value().status == ProjectedSurfaceHitStatus::ambiguous);
    REQUIRE(same_object_tie.value().object.value == 0U);
    REQUIRE(!same_object_tie.value().sample.surface_anchor.has_value());
    const auto same_object_face_visibility = carto::ui::resolve_frontmost_viewport_surface(
        same_object_overlap, Vec2d{2.0, 2.0}, true);
    REQUIRE(same_object_face_visibility);
    REQUIRE(same_object_face_visibility.value().status == ProjectedSurfaceHitStatus::hit);
    REQUIRE(same_object_face_visibility.value().object == selected);
    REQUIRE(same_object_face_visibility.value().sample.surface_anchor.has_value());
    REQUIRE(same_object_face_visibility.value().sample.surface_anchor->face == FaceId{1U});

    const auto miss = carto::ui::resolve_frontmost_viewport_surface(
        triangles, Vec2d{12.0, 12.0});
    REQUIRE(miss);
    REQUIRE(miss.value().status == ProjectedSurfaceHitStatus::miss);

    triangles[0U].camera_depth[0U] = std::numeric_limits<double>::quiet_NaN();
    const auto malformed = carto::ui::resolve_frontmost_viewport_surface(
        triangles, Vec2d{2.0, 2.0});
    REQUIRE(!malformed);
    REQUIRE(malformed.error().code == carto::core::ErrorCode::validation_failed);
}

void knife_viewport_sampling_snaps_endpoints_and_stays_on_the_selected_face() {
    using carto::core::Vec2d;
    using carto::core::Vec3d;
    using carto::geometry::FaceId;
    using carto::geometry::VertexId;
    using carto::scene::ObjectId;
    using carto::ui::SculptViewportTriangle;

    const ObjectId object{17U};
    const FaceId face{9U};
    const SculptViewportTriangle first{
        object, face,
        {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
        {VertexId{1U}, VertexId{2U}, VertexId{3U}},
        {Vec3d{0.0, 0.0, 0.0}, Vec3d{10.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 10.0}},
        {1.0, 1.0, 1.0}};
    const SculptViewportTriangle second{
        object, face,
        {Vec2d{10.0, 0.0}, Vec2d{10.0, 10.0}, Vec2d{0.0, 10.0}},
        {VertexId{2U}, VertexId{4U}, VertexId{3U}},
        {Vec3d{10.0, 0.0, 0.0}, Vec3d{10.0, 0.0, 10.0}, Vec3d{0.0, 0.0, 10.0}},
        {1.0, 1.0, 1.0}};
    const SculptViewportTriangle overlapping_other_face{
        object, FaceId{10U},
        {Vec2d{0.0, 0.0}, Vec2d{10.0, 0.0}, Vec2d{0.0, 10.0}},
        {VertexId{5U}, VertexId{6U}, VertexId{7U}},
        {Vec3d{100.0, 0.0, 100.0}, Vec3d{110.0, 0.0, 100.0},
         Vec3d{100.0, 0.0, 110.0}},
        {10.0, 10.0, 10.0}};
    const std::vector<SculptViewportTriangle> triangles{
        overlapping_other_face, second, first};
    const std::vector<Vec2d> path{{0.5, 0.5}, {5.0, 4.0}, {9.5, 9.5}};

    const auto sampled = carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, path);
    REQUIRE(sampled);
    REQUIRE(sampled.value().size() == 3U);
    REQUIRE(sampled.value().front().x == 0.0 && sampled.value().front().y == 0.0 &&
        sampled.value().front().z == 0.0);
    REQUIRE(sampled.value()[1U].x == 5.0 && sampled.value()[1U].y == 0.0 &&
        sampled.value()[1U].z == 4.0);
    REQUIRE(sampled.value().back().x == 10.0 && sampled.value().back().y == 0.0 &&
        sampled.value().back().z == 10.0);

    const std::vector<Vec2d> edge_interior_path{
        {5.0, 0.5}, {5.0, 5.0}, {9.5, 5.0}};
    const auto edge_interior = carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, edge_interior_path);
    REQUIRE(edge_interior);
    REQUIRE(edge_interior.value().front().x == 5.0 &&
        edge_interior.value().front().y == 0.0 && edge_interior.value().front().z == 0.0);
    REQUIRE(edge_interior.value().back().x == 10.0 &&
        edge_interior.value().back().y == 0.0 && edge_interior.value().back().z == 5.0);
    const std::vector<Vec2d> internal_diagonal_endpoint{
        {5.0, 5.0}, {5.0, 4.0}, {0.0, 5.0}};
    REQUIRE(!carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, internal_diagonal_endpoint));

    const auto wrong_face = carto::ui::sample_knife_viewport_stroke(
        triangles, object, FaceId{11U}, path);
    REQUIRE(!wrong_face);
    const std::vector<Vec2d> off_face_path{{0.0, 0.0}, {12.0, 8.0}, {10.0, 10.0}};
    const auto off_face = carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, off_face_path);
    REQUIRE(!off_face);
    const std::vector<Vec2d> same_vertex_path{{0.0, 0.0}, {3.0, 2.0}, {0.5, 0.5}};
    const auto same_vertex = carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, same_vertex_path);
    REQUIRE(!same_vertex);
    const std::vector<Vec2d> unsnapped_path{{30.0, 30.0}, {5.0, 4.0}, {10.0, 10.0}};
    const auto unsnapped = carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, unsnapped_path);
    REQUIRE(!unsnapped);
    const std::vector<Vec2d> non_finite_path{{0.0, 0.0},
        {std::numeric_limits<double>::quiet_NaN(), 1.0}, {10.0, 10.0}};
    REQUIRE(!carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, non_finite_path));
    const std::vector<Vec2d> too_short_path{path.front()};
    REQUIRE(!carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, too_short_path));
    std::vector<Vec2d> oversized_path(
        carto::ui::kMaxKnifeViewportPathPoints + 1U, Vec2d{5.0, 4.0});
    oversized_path.front() = path.front();
    oversized_path.back() = path.back();
    REQUIRE(!carto::ui::sample_knife_viewport_stroke(
        triangles, object, face, oversized_path));
}

void sculpt_surface_strokes_commit_anchored_samples_through_human_commands() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Sculpt surface stroke test"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Surface mesh", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));

    const auto initial = ui.snapshot();
    REQUIRE(initial.objects.size() == 1U);
    const carto::scene::ObjectId object = initial.objects.front().object.id;
    const std::uint64_t mesh_asset = *initial.objects.front().object.mesh_asset;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));
    const auto selected = ui.snapshot();
    const auto generation = selected.project_generation;
    const auto* instance = selected.viewport.scene.find(object);
    REQUIRE(instance != nullptr);
    REQUIRE(instance->mesh != nullptr);
    const auto source_positions = instance->mesh->positions;

    const auto triangles_from_mesh = [object](const carto::geometry::CompiledMesh& mesh) {
        std::vector<carto::ui::SculptViewportTriangle> result;
        for (std::size_t index = 0U; index + 2U < mesh.indices.size(); index += 3U) {
            const std::array<std::uint32_t, 3> corners{
                mesh.indices[index], mesh.indices[index + 1U], mesh.indices[index + 2U]};
            carto::ui::SculptViewportTriangle triangle;
            triangle.object = object;
            triangle.face = mesh.triangle_faces[index / 3U];
            for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
                const std::uint32_t source_index = corners[corner];
                triangle.vertices[corner] = mesh.vertex_ids[source_index];
                triangle.local_positions[corner] = mesh.positions[source_index];
                triangle.screen_positions[corner] = {
                    triangle.local_positions[corner].x,
                    triangle.local_positions[corner].z};
                triangle.camera_depth[corner] = triangle.local_positions[corner].y;
            }
            result.push_back(std::move(triangle));
        }
        return result;
    };
    const auto triangles = triangles_from_mesh(*instance->mesh);
    const std::vector<carto::core::Vec2d> cursor_path{
        {-0.6, 0.0}, {-0.1, 0.0}, {0.6, 0.0}};
    const auto samples = carto::ui::sample_sculpt_viewport_stroke(
        triangles, object, cursor_path, 0.2);
    REQUIRE(samples);
    REQUIRE(samples.value().size() >= 4U);
    auto authored_mesh = carto::geometry::make_box(carto::core::Vec3d{2.0, 2.0, 2.0});
    REQUIRE(authored_mesh);
    for (const auto& sample : samples.value()) {
        REQUIRE(sample.surface_anchor.has_value());
        const auto resolved = carto::production::resolve_sculpt_surface_anchor(
            authored_mesh.value(), *sample.surface_anchor);
        REQUIRE(resolved);
        REQUIRE(sample.position.x == resolved.value().x);
        REQUIRE(sample.position.y == resolved.value().y);
        REQUIRE(sample.position.z == resolved.value().z);
        REQUIRE(sample.position.y == 1.0);
    }

    const auto revision = selected.project_revision;
    const auto missing_anchor_samples = [&samples]() {
        auto result = samples.value();
        result.front().surface_anchor.reset();
        return result;
    }();
    const auto missing_anchor = ui.dispatch(
        carto::application::AppendSculptSurfaceStrokeAction{
            revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, missing_anchor_samples, generation});
    REQUIRE(!missing_anchor);
    REQUIRE(ui.snapshot().project_revision == revision);
    REQUIRE(ui.snapshot().sculpt_layers.empty());

    auto wrong_source_samples = samples.value();
    wrong_source_samples.front().position.x += 0.01;
    const auto stale_anchor = ui.dispatch(
        carto::application::AppendSculptSurfaceStrokeAction{
            revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, wrong_source_samples, generation});
    REQUIRE(!stale_anchor);
    REQUIRE(stale_anchor.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == revision);

    const auto pre_stale_generation = ui.snapshot();
    const auto stale_generation = ui.dispatch(
        carto::application::AppendSculptSurfaceStrokeAction{
            revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, samples.value(), generation + 1U});
    REQUIRE(!stale_generation);
    REQUIRE(stale_generation.error().code == carto::core::ErrorCode::stale_data);
    const auto post_stale_generation = ui.snapshot();
    REQUIRE(post_stale_generation.project_revision == pre_stale_generation.project_revision);
    REQUIRE(post_stale_generation.project_generation == pre_stale_generation.project_generation);
    REQUIRE(post_stale_generation.operations.size() == pre_stale_generation.operations.size());
    REQUIRE(post_stale_generation.sculpt_layers.empty());

    const auto committed = ui.dispatch(carto::application::AppendSculptSurfaceStrokeAction{
        revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
        2.5, 0.05, samples.value(), generation});
    REQUIRE(committed);
    REQUIRE(committed.value().action == "Add Sculpt Surface Stroke");
    REQUIRE(committed.value().document_changed);
    REQUIRE(!ui.snapshot().operations.empty());
    REQUIRE(ui.snapshot().operations.back().action == "Add Sculpt Surface Stroke");
    const auto first_commit = ui.snapshot();
    const auto* first_commit_instance = first_commit.viewport.scene.find(object);
    REQUIRE(first_commit_instance != nullptr);
    REQUIRE(first_commit_instance->mesh != nullptr);
    REQUIRE(first_commit_instance->mesh->positions.size() == source_positions.size());
    REQUIRE(same_positions(first_commit_instance->mesh->positions, source_positions));
    const auto first_commit_positions = first_commit_instance->mesh->positions;
    const auto next_samples = carto::ui::sample_sculpt_viewport_stroke(
        triangles_from_mesh(*first_commit_instance->mesh), object, cursor_path, 0.2);
    REQUIRE(next_samples);
    const auto second_commit = ui.dispatch(carto::application::AppendSculptSurfaceStrokeAction{
        first_commit.project_revision, object, mesh_asset,
        carto::production::SculptStrokeMode::add, 2.5, 0.025,
        next_samples.value(), first_commit.project_generation});
    REQUIRE(second_commit);
    const auto sculpted = ui.snapshot();
    const auto* sculpted_instance = sculpted.viewport.scene.find(object);
    REQUIRE(sculpted_instance != nullptr && sculpted_instance->mesh != nullptr);
    REQUIRE(same_positions(sculpted_instance->mesh->positions, source_positions));
    REQUIRE(same_positions(sculpted_instance->mesh->positions, first_commit_positions));
    const auto authored_revision = ui.snapshot().project_revision;
    const auto authored_generation = ui.snapshot().project_generation;

    REQUIRE(next_samples.value() == samples.value());

    const auto ai_mutation = ui.submit_ai_proposal(
        carto::application::AppendSculptSurfaceStrokeAction{
            authored_revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
            2.5, 0.05, samples.value(), authored_generation});
    REQUIRE(!ai_mutation);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto stored = ui.snapshot();
    REQUIRE(stored.sculpt_layers.size() == 1U);
    REQUIRE(stored.sculpt_layers.front().stroke_count == 2U);
    const auto stale = ui.dispatch(carto::application::AppendSculptSurfaceStrokeAction{
        revision, object, mesh_asset, carto::production::SculptStrokeMode::add,
        2.5, 0.05, samples.value(), generation});
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto path = temp.path() / "surface-sculpt.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto decoded = carto::production::SculptStack::deserialize(
        reopened.value().sculpt_stacks().at(mesh_asset).bytes);
    REQUIRE(decoded);
    const auto& stroke = decoded.value().layers.front().strokes.front();
    REQUIRE(stroke.samples == samples.value());
    const auto stored_base_mesh = reopened.value().meshes().find(mesh_asset);
    REQUIRE(stored_base_mesh != reopened.value().meshes().end());
    const auto stored_base_compiled = stored_base_mesh->second.compile();
    REQUIRE(stored_base_compiled);
    REQUIRE(same_positions(stored_base_compiled.value().positions, source_positions));

}

void sculpt_point_strokes_are_revision_bound_human_commands() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Sculpt workspace test"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Sculpt mesh", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));

    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const carto::scene::ObjectId object = snapshot.objects.front().object.id;
    const std::uint64_t mesh_asset = *snapshot.objects.front().object.mesh_asset;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));

    const auto selected = ui.snapshot();
    const auto* source_instance = selected.viewport.scene.find(object);
    REQUIRE(source_instance != nullptr);
    REQUIRE(source_instance->mesh != nullptr);
    const auto source_positions = source_instance->mesh->positions;
    const auto original_revision = selected.project_revision;
    const auto original_generation = selected.project_generation;
    const auto invalid_mode = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, static_cast<carto::production::SculptStrokeMode>(99U), 0.5,
        0.05, original_generation});
    REQUIRE(!invalid_mode);
    REQUIRE(ui.snapshot().project_revision == original_revision);
    const auto invalid_radius = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, -1.0, 0.05,
        original_generation});
    REQUIRE(!invalid_radius);
    REQUIRE(ui.snapshot().project_revision == original_revision);

    const auto raw_ai = ui.submit_ai_proposal(
        carto::application::AppendSculptStrokeAction{
            original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05,
            original_generation});
    REQUIRE(!raw_ai);
    REQUIRE(ui.snapshot().project_revision == original_revision);

    const auto pre_stale_generation = ui.snapshot();
    const auto stale_generation = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05,
        original_generation + 1U});
    REQUIRE(!stale_generation);
    REQUIRE(stale_generation.error().code == carto::core::ErrorCode::stale_data);
    const auto post_stale_generation = ui.snapshot();
    REQUIRE(post_stale_generation.project_revision == pre_stale_generation.project_revision);
    REQUIRE(post_stale_generation.project_generation == pre_stale_generation.project_generation);
    REQUIRE(post_stale_generation.operations.size() == pre_stale_generation.operations.size());
    REQUIRE(post_stale_generation.sculpt_layers.empty());

    const auto committed = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05,
        original_generation});
    REQUIRE(committed);
    REQUIRE(committed.value().action == "Add Sculpt Stroke");
    REQUIRE(committed.value().document_changed);
    REQUIRE(committed.value().affected.objects == std::vector<carto::scene::ObjectId>{object});
    REQUIRE(committed.value().affected.vertices ==
        std::vector<carto::geometry::VertexId>{carto::geometry::VertexId{1U}});
    const auto authored_revision = ui.snapshot().project_revision;
    REQUIRE(authored_revision > original_revision);

    const auto stale = ui.dispatch(carto::application::AppendSculptStrokeAction{
        original_revision, carto::production::SculptStrokeMode::add, 0.5, 0.05,
        original_generation});
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto layer_snapshot = ui.snapshot();
    REQUIRE(layer_snapshot.sculpt_mesh_asset == mesh_asset);
    REQUIRE(layer_snapshot.sculpt_layers.size() == 1U);
    REQUIRE(!layer_snapshot.sculpt_layers.front().identity.empty());
    REQUIRE(layer_snapshot.sculpt_layers.front().visible);
    REQUIRE(layer_snapshot.sculpt_layers.front().stroke_count == 1U);
    const auto path = temp.path() / "sculpt.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));

    const auto wrong_mesh = ui.dispatch(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset + 1U,
            layer_snapshot.sculpt_layers.front().identity, false,
            layer_snapshot.project_generation});
    REQUIRE(!wrong_mesh);
    REQUIRE(wrong_mesh.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == authored_revision);

    const auto missing_layer = ui.dispatch(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset, "missing.layer", false,
            layer_snapshot.project_generation});
    REQUIRE(!missing_layer);
    REQUIRE(missing_layer.error().code == carto::core::ErrorCode::not_found);
    REQUIRE(ui.snapshot().project_revision == authored_revision);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);

    const auto ai_visibility = ui.submit_ai_proposal(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset,
            layer_snapshot.sculpt_layers.front().identity, false,
            layer_snapshot.project_generation});
    REQUIRE(!ai_visibility);
    REQUIRE(ui.snapshot().project_revision == authored_revision);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);

    const auto hidden = ui.dispatch(carto::application::SetSculptLayerVisibilityAction{
        authored_revision, mesh_asset,
        layer_snapshot.sculpt_layers.front().identity, false,
        layer_snapshot.project_generation});
    REQUIRE(hidden);
    REQUIRE(hidden.value().action == "Set Sculpt Layer Visibility");
    REQUIRE(hidden.value().document_changed);
    REQUIRE(ui.snapshot().operations.back().action == "Set Sculpt Layer Visibility");
    carto::journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries);
    REQUIRE(journal_entries.value().back().event_type == "cartographer.application_action");
    const std::string journal_payload(
        journal_entries.value().back().payload.begin(),
        journal_entries.value().back().payload.end());
    REQUIRE(journal_payload.find("operation=Set Sculpt Layer Visibility") != std::string::npos);
    const auto hidden_revision = ui.snapshot().project_revision;
    REQUIRE(!ui.snapshot().sculpt_layers.front().visible);
    const auto hidden_view = ui.snapshot();
    const auto* hidden_instance = hidden_view.viewport.scene.find(object);
    REQUIRE(hidden_instance != nullptr);
    REQUIRE(hidden_instance->mesh != nullptr);
    REQUIRE(same_positions(hidden_instance->mesh->positions, source_positions));

    const auto stale_visibility = ui.dispatch(
        carto::application::SetSculptLayerVisibilityAction{
            authored_revision, mesh_asset,
            layer_snapshot.sculpt_layers.front().identity, true,
            layer_snapshot.project_generation});
    REQUIRE(!stale_visibility);
    REQUIRE(stale_visibility.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == hidden_revision);
    REQUIRE(!ui.snapshot().sculpt_layers.front().visible);

    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().project_revision > authored_revision);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().project_revision > authored_revision);
    REQUIRE(!ui.snapshot().sculpt_layers.front().visible);
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));

    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    REQUIRE(reopened.value().sculpt_stacks().contains(mesh_asset));
    const auto& stored = reopened.value().sculpt_stacks().at(mesh_asset);
    const auto stack = carto::production::SculptStack::deserialize(stored.bytes);
    REQUIRE(stack);
    REQUIRE(stack.value().validate());
    REQUIRE(stack.value().layers.size() == 1U);
    REQUIRE(stack.value().layers.front().strokes.size() == 1U);
    REQUIRE(!stack.value().layers.front().visible);
    const auto mesh = reopened.value().meshes().find(mesh_asset);
    REQUIRE(mesh != reopened.value().meshes().end());


    const auto enabled = ui.dispatch(carto::application::SetSculptLayerVisibilityAction{
        ui.snapshot().project_revision, mesh_asset,
        layer_snapshot.sculpt_layers.front().identity, true,
        ui.snapshot().project_generation});
    REQUIRE(enabled);
    REQUIRE(ui.snapshot().sculpt_layers.front().visible);
    const auto enabled_view = ui.snapshot();
    const auto* enabled_instance = enabled_view.viewport.scene.find(object);
    REQUIRE(enabled_instance != nullptr);
    REQUIRE(enabled_instance->mesh != nullptr);
    // The authored stack is visible; this source viewport does not evaluate
    // SculptStack displacement. Its source geometry must stay unchanged.
    REQUIRE(same_positions(enabled_instance->mesh->positions, source_positions));
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    const auto reopened_enabled = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened_enabled);
    const auto enabled_stack = carto::production::SculptStack::deserialize(
        reopened_enabled.value().sculpt_stacks().at(mesh_asset).bytes);
    REQUIRE(enabled_stack);
    REQUIRE(enabled_stack.value().layers.front().visible);
    const auto enabled_mesh = reopened_enabled.value().meshes().find(mesh_asset);
    REQUIRE(enabled_mesh != reopened_enabled.value().meshes().end());


    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::object}));
    const auto no_vertex_snapshot = ui.snapshot();
    const auto no_vertex = ui.dispatch(carto::application::AppendSculptStrokeAction{
        no_vertex_snapshot.project_revision, carto::production::SculptStrokeMode::add,
        0.5, 0.05, no_vertex_snapshot.project_generation});
    REQUIRE(!no_vertex);
    REQUIRE(ui.snapshot().project_revision == reopened_enabled.value().revision());
}

void sculpt_layer_weight_and_order_edits_are_revision_bound_human_commands() {
    TempDirectory temp;
    carto::application::ApplicationSession session;
    carto::ui::UiController ui(session);
    REQUIRE(ui.dispatch(carto::application::NewProjectAction{"Sculpt layer controls"}));
    REQUIRE(ui.dispatch(carto::application::CreateBoxAction{"Layer mesh", {2.0, 2.0, 2.0}}));
    REQUIRE(ui.set_workspace(carto::ui::Workspace::sculpt));
    auto snapshot = ui.snapshot();
    const auto object = snapshot.objects.front().object.id;
    const auto mesh_asset = *snapshot.objects.front().object.mesh_asset;
    REQUIRE(ui.dispatch(carto::application::SelectObjectAction{object}));
    REQUIRE(ui.dispatch(carto::application::SetSelectionModeAction{
        carto::editor::SelectionMode::vertex}));
    REQUIRE(ui.dispatch(carto::application::SelectVertexAction{
        object, carto::geometry::VertexId{1U}}));

    auto stroke_snapshot = ui.snapshot();
    auto stroke = ui.dispatch(carto::application::AppendSculptStrokeAction{
        stroke_snapshot.project_revision, carto::production::SculptStrokeMode::add, 0.5,
        0.05, stroke_snapshot.project_generation});
    REQUIRE(stroke);
    snapshot = ui.snapshot();
    REQUIRE(snapshot.sculpt_layers.size() == 1U);
    const auto first_layer = snapshot.sculpt_layers.front().identity;
    const auto first_layer_revision = snapshot.project_revision;
    const auto first_layer_generation = snapshot.project_generation;

    const auto invalid_weight = ui.dispatch(carto::application::SetSculptLayerWeightAction{
        first_layer_revision, mesh_asset, first_layer, 1.01, first_layer_generation});
    REQUIRE(!invalid_weight);
    REQUIRE(ui.snapshot().project_revision == first_layer_revision);
    const auto stale_generation = ui.dispatch(carto::application::SetSculptLayerWeightAction{
        first_layer_revision, mesh_asset, first_layer, 0.5,
        first_layer_generation + 1U});
    REQUIRE(!stale_generation);
    REQUIRE(stale_generation.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == first_layer_revision);
    const auto ai_weight = ui.submit_ai_proposal(carto::application::SetSculptLayerWeightAction{
        first_layer_revision, mesh_asset, first_layer, 0.5, first_layer_generation});
    REQUIRE(!ai_weight);
    REQUIRE(ui.snapshot().project_revision == first_layer_revision);

    const auto zero_weight = ui.dispatch(carto::application::SetSculptLayerWeightAction{
        first_layer_revision, mesh_asset, first_layer, 0.0, first_layer_generation});
    REQUIRE(zero_weight);
    REQUIRE(zero_weight.value().action == "Set Sculpt Layer Weight");
    snapshot = ui.snapshot();
    REQUIRE(snapshot.sculpt_layers.size() == 1U);
    REQUIRE(snapshot.sculpt_layers.front().weight == 0.0);
    const auto zero_weight_revision = snapshot.project_revision;

    stroke = ui.dispatch(carto::application::AppendSculptStrokeAction{
        zero_weight_revision, carto::production::SculptStrokeMode::inflate, 0.5, 0.1,
        snapshot.project_generation});
    REQUIRE(stroke);
    snapshot = ui.snapshot();
    REQUIRE(snapshot.sculpt_layers.size() == 2U);
    REQUIRE(snapshot.sculpt_layers.front().identity == first_layer);
    const auto second_layer = snapshot.sculpt_layers.back().identity;
    const auto before_move_revision = snapshot.project_revision;
    const auto ai_move = ui.submit_ai_proposal(carto::application::MoveSculptLayerAction{
        before_move_revision, mesh_asset, second_layer, 0U, snapshot.project_generation});
    REQUIRE(!ai_move);
    REQUIRE(ui.snapshot().project_revision == before_move_revision);
    const auto invalid_destination = ui.dispatch(carto::application::MoveSculptLayerAction{
        before_move_revision, mesh_asset, second_layer, snapshot.sculpt_layers.size(),
        snapshot.project_generation});
    REQUIRE(!invalid_destination);
    REQUIRE(invalid_destination.error().code == carto::core::ErrorCode::invalid_argument);
    REQUIRE(ui.snapshot().project_revision == before_move_revision);
    const auto moved = ui.dispatch(carto::application::MoveSculptLayerAction{
        before_move_revision, mesh_asset, second_layer, 0U, snapshot.project_generation});
    REQUIRE(moved);
    REQUIRE(moved.value().action == "Move Sculpt Layer");
    snapshot = ui.snapshot();
    REQUIRE(snapshot.sculpt_layers.size() == 2U);
    REQUIRE(snapshot.sculpt_layers[0U].identity == second_layer);
    REQUIRE(snapshot.sculpt_layers[1U].identity == first_layer);
    REQUIRE(snapshot.sculpt_layers[1U].weight == 0.0);

    const auto stale_move = ui.dispatch(carto::application::MoveSculptLayerAction{
        before_move_revision, mesh_asset, first_layer, 0U, snapshot.project_generation});
    REQUIRE(!stale_move);
    REQUIRE(stale_move.error().code == carto::core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().sculpt_layers[0U].identity == second_layer);
    REQUIRE(ui.dispatch(carto::application::UndoAction{}));
    REQUIRE(ui.snapshot().sculpt_layers[0U].identity == first_layer);
    REQUIRE(ui.dispatch(carto::application::RedoAction{}));
    REQUIRE(ui.snapshot().sculpt_layers[0U].identity == second_layer);

    const auto path = temp.path() / "sculpt-layer-order.carto";
    REQUIRE(ui.dispatch(carto::application::SaveProjectAction{path}));
    const auto reopened = carto::project::ProjectDocument::load(path);
    REQUIRE(reopened);
    const auto& stored = reopened.value().sculpt_stacks().at(mesh_asset);
    const auto decoded = carto::production::SculptStack::deserialize(stored.bytes);
    REQUIRE(decoded && decoded.value().validate());
    REQUIRE(decoded.value().layers.size() == 2U);
    REQUIRE(decoded.value().layers[0U].identity == second_layer);
    REQUIRE(decoded.value().layers[1U].identity == first_layer);
    REQUIRE(decoded.value().layers[1U].weight == 0.0);
}

void decal_projection_outlines_require_exact_bounded_source_identity() {
    using namespace carto;

    geometry::CompiledMesh mesh;
    mesh.positions = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
        {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}};
    mesh.normals.assign(mesh.positions.size(), {0.0, 1.0, 0.0});
    mesh.vertex_ids = {geometry::VertexId{1U}, geometry::VertexId{2U},
        geometry::VertexId{3U}, geometry::VertexId{4U}};
    mesh.indices = {0U, 1U, 2U, 1U, 3U, 2U};
    mesh.triangle_faces = {geometry::FaceId{9U}, geometry::FaceId{9U}};
    mesh.triangle_edges.resize(2U);
    mesh.source_revision = core::Revision{7U};
    for (const auto& position : mesh.positions) mesh.bounds.include(position);
    REQUIRE(mesh.valid());

    production::DecalPlacement placement;
    placement.id = 4U;
    placement.revision = core::Revision{2U};
    placement.target_mesh_asset = 17U;
    placement.target_mesh_revision = core::Revision{7U};
    placement.target_mesh_digest.bytes[0U] = 1U;
    placement.material_id = 6U;
    REQUIRE(placement.validate());

    production::DecalProjection projection;
    projection.placement_id = placement.id;
    projection.placement_revision = placement.revision;
    projection.target_mesh_asset = placement.target_mesh_asset;
    projection.target_mesh_revision = placement.target_mesh_revision;
    projection.target_mesh_digest = placement.target_mesh_digest;
    projection.material_id = placement.material_id;
    production::DecalProjectedPolygon polygon;
    polygon.source_face = geometry::FaceId{9U};
    polygon.source_triangle = 1U;
    polygon.vertices = {
        {{0.2, 0.3}, {0.2, 0.3, 0.5}},
        {{0.6, 0.4}, {0.2, 0.3, 0.5}},
        {{0.8, 0.9}, {0.2, 0.3, 0.5}}};
    projection.polygons.push_back(polygon);

    const auto outline = ui::resolve_decal_projection_outline(
        placement, projection, placement.target_mesh_asset,
        placement.target_mesh_revision, mesh, mesh);
    REQUIRE(outline);
    REQUIRE(outline.value().size() == 1U);
    REQUIRE(outline.value().front().size() == 3U);
    REQUIRE(std::abs(outline.value().front().front().x - 0.5) < 1.0e-12);
    REQUIRE(std::abs(outline.value().front().front().z - 0.8) < 1.0e-12);

    auto sculpted_visible_mesh = mesh;
    sculpted_visible_mesh.positions.front().x = 0.01;
    const auto rejected_derived_surface = ui::resolve_decal_projection_outline(
        placement, projection, placement.target_mesh_asset,
        placement.target_mesh_revision, mesh, sculpted_visible_mesh);
    REQUIRE(!rejected_derived_surface);
    REQUIRE(rejected_derived_surface.error().code == core::ErrorCode::stale_data);

    const auto wrong_asset = ui::resolve_decal_projection_outline(
        placement, projection, placement.target_mesh_asset + 1U,
        placement.target_mesh_revision, mesh, mesh);
    REQUIRE(!wrong_asset);
    REQUIRE(wrong_asset.error().code == core::ErrorCode::stale_data);
    const auto wrong_revision = ui::resolve_decal_projection_outline(
        placement, projection, placement.target_mesh_asset,
        core::Revision{6U}, mesh, mesh);
    REQUIRE(!wrong_revision);
    REQUIRE(wrong_revision.error().code == core::ErrorCode::stale_data);

    auto missing_triangle = projection;
    missing_triangle.polygons.front().source_triangle = 2U;
    const auto absent_ordinal = ui::resolve_decal_projection_outline(
        placement, missing_triangle, placement.target_mesh_asset,
        placement.target_mesh_revision, mesh, mesh);
    REQUIRE(!absent_ordinal);
    REQUIRE(absent_ordinal.error().code == core::ErrorCode::stale_data);

    auto malformed_barycentric = projection;
    malformed_barycentric.polygons.front().vertices.front().source_barycentric =
        {0.2, 0.3, 0.6};
    const auto rejected_barycentric = ui::resolve_decal_projection_outline(
        placement, malformed_barycentric, placement.target_mesh_asset,
        placement.target_mesh_revision, mesh, mesh);
    REQUIRE(!rejected_barycentric);
    REQUIRE(rejected_barycentric.error().code == core::ErrorCode::validation_failed);

    auto mismatched_digest = projection;
    mismatched_digest.target_mesh_digest.bytes[1U] = 1U;
    const auto rejected_digest = ui::resolve_decal_projection_outline(
        placement, mismatched_digest, placement.target_mesh_asset,
        placement.target_mesh_revision, mesh, mesh);
    REQUIRE(!rejected_digest);
    REQUIRE(rejected_digest.error().code == core::ErrorCode::stale_data);

    auto over_budget = projection;
    over_budget.polygons.resize(ui::kMaxDecalOutlinePolygons + 1U, polygon);
    const auto rejected_budget = ui::resolve_decal_projection_outline(
        placement, over_budget, placement.target_mesh_asset,
        placement.target_mesh_revision, mesh, mesh);
    REQUIRE(!rejected_budget);
    REQUIRE(rejected_budget.error().code == core::ErrorCode::unsupported);

    auto over_source_triangle_budget = mesh;
    const std::size_t source_triangle_count =
        ui::kMaxDecalOutlineSourceTriangles + 1U;
    over_source_triangle_budget.indices.clear();
    over_source_triangle_budget.indices.reserve(source_triangle_count * 3U);
    for (std::size_t index = 0U; index < source_triangle_count; ++index) {
        over_source_triangle_budget.indices.insert(
            over_source_triangle_budget.indices.end(), {0U, 1U, 2U});
    }
    over_source_triangle_budget.triangle_faces.assign(
        source_triangle_count, geometry::FaceId{9U});
    over_source_triangle_budget.triangle_edges.resize(source_triangle_count);
    const auto rejected_source_budget = ui::resolve_decal_projection_outline(
        placement, projection, placement.target_mesh_asset,
        placement.target_mesh_revision, over_source_triangle_budget,
        over_source_triangle_budget);
    REQUIRE(!rejected_source_budget);
    REQUIRE(rejected_source_budget.error().code == core::ErrorCode::unsupported);
}

void viewport_decal_preparation_requires_live_current_exact_displayed_source() {
    using namespace carto;
    TempDirectory temp;
    application::ApplicationSession session;
    ui::UiController controller(session);
    const auto path = temp.path() / "viewport-decal.carto";
    REQUIRE(controller.dispatch(application::NewProjectAction{"Viewport decal admission"}));
    REQUIRE(controller.dispatch(application::SaveProjectAction{path}));
    REQUIRE(controller.dispatch(application::CreateBoxAction{"Target", {2.0, 2.0, 2.0}}));
    REQUIRE(controller.set_workspace(ui::Workspace::model));
    auto current = controller.snapshot();
    const auto object = current.objects.front().object.id;
    REQUIRE(controller.dispatch(application::SelectObjectAction{object}));
    REQUIRE(controller.dispatch(application::CreateMaterialAction{
        current.project_revision, "Decal", render::StandardMaterial{}}));
    current = controller.snapshot();
    const auto* displayed = current.viewport.scene.find(object);
    REQUIRE(displayed != nullptr && displayed->mesh != nullptr);
    const ui::ViewportDecalObservation observation{
        ui::ViewportSceneSource::live_project, current.project_generation,
        current.project_revision, *displayed};
    const auto placement = controller.make_decal_placement(
        object, current.materials.front().id, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5, 0.5, 0.1, 0.0,
        current.project_revision);
    REQUIRE(placement);
    journal::Journal journal(std::filesystem::path(path.string() + ".journal"));
    const auto entries_before = journal.read_all();
    REQUIRE(entries_before);
    const auto require_unchanged = [&]() {
        const auto after = controller.snapshot();
        REQUIRE(after.project_revision == current.project_revision);
        REQUIRE(after.undo_count == current.undo_count);
        REQUIRE(after.redo_count == current.redo_count);
        REQUIRE(after.decal_placements.empty());
        const auto entries = journal.read_all();
        REQUIRE(entries && entries.value().size() == entries_before.value().size());
    };
    const auto require_rejected = [&](const ui::ViewportDecalObservation& candidate,
                                      ui::ViewportSceneSource active_source,
                                      core::ErrorCode expected_error) {
        const auto action = controller.prepare_viewport_decal_placement(
            placement.value(), candidate, active_source);
        REQUIRE(!action && action.error().code == expected_error);
        require_unchanged();
    };

    auto preview = observation;
    preview.source = ui::ViewportSceneSource::agent_preview;
    require_rejected(preview, ui::ViewportSceneSource::agent_preview,
        core::ErrorCode::unsupported);
    require_rejected(preview, ui::ViewportSceneSource::live_project,
        core::ErrorCode::unsupported);
    require_rejected(observation, ui::ViewportSceneSource::agent_preview,
        core::ErrorCode::unsupported);
    require_rejected(ui::ViewportDecalObservation{}, ui::ViewportSceneSource::live_project,
        core::ErrorCode::unsupported);
    auto wrong_generation = observation;
    ++wrong_generation.project_generation;
    require_rejected(wrong_generation, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);
    auto wrong_revision = observation;
    wrong_revision.project_revision = current.project_revision.next();
    require_rejected(wrong_revision, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);
    auto missing_source = observation;
    missing_source.target.mesh.reset();
    require_rejected(missing_source, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);
    auto substituted_target = observation;
    ++substituted_target.target.object.value;
    require_rejected(substituted_target, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);
    auto substituted_asset = observation;
    substituted_asset.target.mesh_asset = placement.value().target_mesh_asset + 1U;
    require_rejected(substituted_asset, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);
    auto derived = observation;
    auto derived_mesh = std::make_shared<geometry::CompiledMesh>(*displayed->mesh);
    derived_mesh->positions.front().x += 0.01;
    REQUIRE(derived_mesh->valid());
    derived.target.mesh = derived_mesh;
    require_rejected(derived, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);
    auto substituted_identity = observation;
    auto identity_mesh = std::make_shared<geometry::CompiledMesh>(*displayed->mesh);
    std::swap(identity_mesh->vertex_ids[0U], identity_mesh->vertex_ids[1U]);
    REQUIRE(identity_mesh->valid());
    substituted_identity.target.mesh = identity_mesh;
    require_rejected(substituted_identity, ui::ViewportSceneSource::live_project,
        core::ErrorCode::stale_data);

    const auto admitted = controller.prepare_viewport_decal_placement(
        placement.value(), observation, ui::ViewportSceneSource::live_project);
    REQUIRE(admitted);
    REQUIRE(admitted.value().expected_project_generation == observation.project_generation);
    REQUIRE(admitted.value().expected_project_revision == observation.project_revision);
    REQUIRE(admitted.value().expected_placement_revision == core::Revision{});
    require_unchanged();
    REQUIRE(!controller.submit_ai_proposal(admitted.value()));
    require_unchanged();
    const auto committed = controller.dispatch(admitted.value());
    REQUIRE(committed && committed.value().document_changed);
    REQUIRE(committed.value().source == application::OperationSource::human);
    REQUIRE(controller.snapshot().decal_placements.size() == 1U);
    REQUIRE(controller.snapshot().undo_count == current.undo_count + 1U);
    REQUIRE(!controller.prepare_viewport_decal_placement(
        placement.value(), observation, ui::ViewportSceneSource::live_project));
    REQUIRE(controller.dispatch(application::UndoAction{}));
    REQUIRE(controller.snapshot().decal_placements.empty());
    REQUIRE(controller.dispatch(application::RedoAction{}));
    REQUIRE(controller.snapshot().decal_placements.size() == 1U);
    const auto entries_after = journal.read_all();
    REQUIRE(entries_after);
    REQUIRE(std::count_if(entries_after.value().begin(), entries_after.value().end(),
        [](const auto& entry) {
            const std::string payload(entry.payload.begin(), entry.payload.end());
            return payload.find("Set Decal Placement") != std::string::npos;
        }) == 1);
}

void viewport_decal_preparation_accepts_source_display_after_sculpt_authoring() {
    using namespace carto;
    TempDirectory temp;
    application::ApplicationSession session;
    ui::UiController controller(session);
    REQUIRE(controller.dispatch(application::NewProjectAction{"Sculpted decal source"}));
    REQUIRE(controller.dispatch(application::SaveProjectAction{temp.path() / "sculpted-decal.carto"}));
    REQUIRE(controller.dispatch(application::CreateBoxAction{"Target", {2.0, 2.0, 2.0}}));
    auto current = controller.snapshot();
    const auto object = current.objects.front().object.id;
    const auto mesh_asset = *current.objects.front().object.mesh_asset;
    REQUIRE(controller.dispatch(application::CreateMaterialAction{
        current.project_revision, "Decal", render::StandardMaterial{}}));
    REQUIRE(controller.dispatch(application::SelectObjectAction{object}));
    REQUIRE(controller.set_workspace(ui::Workspace::sculpt));
    REQUIRE(controller.dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(controller.dispatch(application::SelectVertexAction{object, geometry::VertexId{1U}}));
    REQUIRE(controller.dispatch(application::AppendSculptStrokeAction{
        controller.snapshot().project_revision, production::SculptStrokeMode::add, 0.5,
        0.05, controller.snapshot().project_generation}));
    const auto layer = controller.snapshot().sculpt_layers.front().identity;
    REQUIRE(controller.dispatch(application::SetSelectionModeAction{editor::SelectionMode::object}));
    REQUIRE(controller.set_workspace(ui::Workspace::model));
    REQUIRE(controller.dispatch(application::SelectObjectAction{object}));
    current = controller.snapshot();
    const auto* displayed = current.viewport.scene.find(object);
    REQUIRE(displayed != nullptr && displayed->mesh != nullptr);
    const auto placement = controller.make_decal_placement(
        object, current.materials.front().id, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5, 0.5, 0.1, 0.0,
        current.project_revision);
    REQUIRE(placement);
    const auto base_source = controller.decal_placement_source_mesh(
        placement.value(), current.project_revision);
    REQUIRE(base_source);
    REQUIRE(same_positions(base_source.value().positions, displayed->mesh->positions));
    const auto authored_stack_display_admitted = controller.prepare_viewport_decal_placement(
        placement.value(), ui::ViewportDecalObservation{
            ui::ViewportSceneSource::live_project, current.project_generation,
            current.project_revision, *displayed}, ui::ViewportSceneSource::live_project);
    REQUIRE(authored_stack_display_admitted);
    REQUIRE(controller.snapshot().project_revision == current.project_revision);
    REQUIRE(controller.snapshot().undo_count == current.undo_count);
    // An uncommitted display deformation must still fail source admission.
    auto derived_display = *displayed;
    auto derived_mesh = std::make_shared<geometry::CompiledMesh>(*displayed->mesh);
    derived_mesh->positions.front().x += 0.05;
    REQUIRE(derived_mesh->valid());
    REQUIRE(!same_positions(base_source.value().positions, derived_mesh->positions));
    derived_display.mesh = std::move(derived_mesh);
    const auto rejected = controller.prepare_viewport_decal_placement(
        placement.value(), ui::ViewportDecalObservation{
            ui::ViewportSceneSource::live_project, current.project_generation,
            current.project_revision, derived_display}, ui::ViewportSceneSource::live_project);
    REQUIRE(!rejected && rejected.error().code == core::ErrorCode::stale_data);
    REQUIRE(controller.snapshot().project_revision == current.project_revision);
    REQUIRE(controller.snapshot().undo_count == current.undo_count);
    REQUIRE(controller.snapshot().decal_placements.empty());

    REQUIRE(controller.set_workspace(ui::Workspace::sculpt));
    REQUIRE(controller.dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex}));
    REQUIRE(controller.dispatch(application::SelectVertexAction{object, geometry::VertexId{1U}}));
    REQUIRE(controller.dispatch(application::SetSculptLayerVisibilityAction{
        current.project_revision, mesh_asset, layer, false,
        current.project_generation}));
    REQUIRE(controller.dispatch(application::SetSelectionModeAction{editor::SelectionMode::object}));
    REQUIRE(controller.set_workspace(ui::Workspace::model));
    REQUIRE(controller.dispatch(application::SelectObjectAction{object}));
    current = controller.snapshot();
    displayed = current.viewport.scene.find(object);
    REQUIRE(displayed != nullptr && displayed->mesh != nullptr);
    REQUIRE(same_positions(base_source.value().positions, displayed->mesh->positions));
    const auto restored_draft = controller.make_decal_placement(
        object, current.materials.front().id, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, 0.5, 0.5, 0.1, 0.0,
        current.project_revision);
    REQUIRE(restored_draft);
    const auto admitted = controller.prepare_viewport_decal_placement(
        restored_draft.value(), ui::ViewportDecalObservation{
            ui::ViewportSceneSource::live_project, current.project_generation,
            current.project_revision, *displayed}, ui::ViewportSceneSource::live_project);
    REQUIRE(admitted);
    REQUIRE(controller.snapshot().project_revision == current.project_revision);
    REQUIRE(controller.snapshot().undo_count == current.undo_count);
    REQUIRE(controller.dispatch(admitted.value()));
    REQUIRE(controller.snapshot().decal_placements.size() == 1U);
}

} // namespace

void ui_routes_uv_island_packing_through_current_selection() {
    using namespace carto;
    application::ApplicationSession session;
    ui::UiController ui(session);
    geometry::EditableMesh mesh;
    const auto a = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto b = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto c = mesh.add_vertex({0.0, 1.0, 0.0});
    REQUIRE(a && b && c);
    REQUIRE(mesh.add_face({a.value(), b.value(), c.value()}));
    const auto topology = mesh.topology();
    REQUIRE(topology);
    std::vector<attributes::AttributeValue> coordinates;
    for (const auto& corner : topology.value().corners) {
        const auto* vertex = mesh.find_vertex(corner.vertex);
        REQUIRE(vertex != nullptr);
        coordinates.emplace_back(core::Vec2d{vertex->position.x, vertex->position.y});
    }
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::corner, topology.value().corners.size()}});
    REQUIRE(payload.add_layer({
        {"uv.source.corners", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec2},
        std::move(coordinates)}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({7U, "Source UV", "uv.source.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(payload), std::move(uv_sets)));
    REQUIRE(ui.dispatch(application::CreateMeshObjectAction{"UV pack target", std::move(mesh)}));
    auto snapshot = ui.snapshot();
    REQUIRE(snapshot.objects.size() == 1U);
    const auto object = snapshot.objects.front().object.id;
    REQUIRE(snapshot.objects.front().object.mesh_asset.has_value());
    REQUIRE(snapshot.objects.front().mesh_revision.has_value());
    REQUIRE(ui.dispatch(application::SelectObjectAction{object}));
    snapshot = ui.snapshot();
    const application::PackUvIslandsAction action{
        snapshot.project_generation, snapshot.project_revision,
        *snapshot.objects.front().mesh_revision, object, 7U, 8U, 128U, 2U, 2U,
        production::UvIslandDensityPolicy::preserve_source_uv, 0.0, false};

    const auto before = ui.snapshot();
    const auto stale = ui.pack_uv_islands(action, snapshot.project_generation + 1U);
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    auto wrong_selection = action;
    wrong_selection.object = scene::ObjectId{999U};
    const auto wrong_owner = ui.pack_uv_islands(wrong_selection, snapshot.project_generation);
    REQUIRE(!wrong_owner && wrong_owner.error().code == core::ErrorCode::invalid_state);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);
    const auto raw_ai = ui.submit_ai_proposal(action);
    REQUIRE(!raw_ai);
    REQUIRE(ui.snapshot().project_revision == before.project_revision);

    const auto packed = ui.pack_uv_islands(action, snapshot.project_generation);
    REQUIRE(packed && packed.value().document_changed);
    const auto after = ui.snapshot();
    REQUIRE(after.project_revision == core::Revision{before.project_revision.value() + 1U});
    REQUIRE(after.operations.size() == before.operations.size() + 1U);
    REQUIRE(after.operations.back().action.starts_with("Pack UV Islands uvp2."));
}

int main() {
    try {
        preferences_validate_shell_invariants();
        ui_routes_authoritative_actions_and_operation_lineage();
        ui_routes_uv_island_packing_through_current_selection();
        ui_routes_mesh_cleanup_review_without_implicit_acceptance();
        ui_routes_connection_graph_cleanup_without_condition_authoring();
        ui_routes_spatial_fields_through_the_human_command_boundary();
        ui_distinguishes_unsaved_from_saved_state();
        ui_rejects_unadmitted_ai_proposals();
        ui_rejects_raw_ai_material_mutations_but_admits_human_material_authoring();
        ui_imports_and_binds_private_pathless_material_texture_sources();
        ui_reimports_material_texture_source_without_retargeting_bindings();
        ui_imports_reference_images_as_project_bound_revisioned_planes();
        ui_edits_reference_image_view_sets_atomically();
        ui_routes_edge_selection_without_authoring_mutation();
        ui_routes_revision_bound_sweep_authoring_and_refuses_stale_inputs();
        ui_preserves_required_viewport_and_supports_mode_theme_density();
        cartographer_brand_palette_is_stable();
        ui_persists_preferences_and_resets_corrupt_state();
        workbench_preferences_round_trip_and_fail_closed();
        workbench_snap_selects_nearest_target_with_bounded_tolerance();
        ui_observation_queries_use_current_viewport_without_mutation();
        workbench_click_place_and_viewport_pan_fallbacks_are_explicit();
        workbench_drag_uses_the_topmost_visible_instrument();
        workbench_native_release_uses_drop_position_not_stale_imgui_pointer();
        ui_connects_surface_fidelity_preview_without_authoring_mutation();
        ui_imports_mesh_through_provider_and_authoring_boundaries();
        ui_promotes_private_botanical_output_with_bound_human_receipts();
        ui_promotes_private_botanical_glb_with_bound_human_receipts();
        ui_promotes_real_botanical_artifact_when_configured();
        ui_source_mesh_reimport_updates_in_place_and_keeps_monotonic_undo_lineage();
        ui_unsaved_mesh_import_remains_available_but_is_explicitly_unlinked();
        ui_rejects_tampered_source_blob_without_fallback_or_project_mutation();
        ui_routes_source_reimport_review_and_keeps_ai_out_of_mutation_path();
        ui_connects_local_market_boundary_and_fails_closed();
        ui_failed_shortcuts_and_actions_remain_bounded();
        ui_command_palette_is_bounded_and_explicit_about_deferred_features();
        native_key_contract_maps_core_control_without_imgui();
        workspace_registry_is_canonical_and_fail_closed();
        sculpt_viewport_sampling_preserves_frontmost_surface_and_path_spacing();
        viewport_surface_pick_resolves_occlusion_and_rejects_ambiguous_source_ties();
        knife_viewport_sampling_snaps_endpoints_and_stays_on_the_selected_face();
        sculpt_surface_strokes_commit_anchored_samples_through_human_commands();
        sculpt_point_strokes_are_revision_bound_human_commands();
        sculpt_layer_weight_and_order_edits_are_revision_bound_human_commands();
        decal_projection_outlines_require_exact_bounded_source_identity();
        viewport_decal_preparation_requires_live_current_exact_displayed_source();
        viewport_decal_preparation_accepts_source_display_after_sculpt_authoring();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
