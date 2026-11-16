#include <carto/agent/workbench.hpp>
#include <carto/application/application.hpp>

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace carto;
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string("requirement failed at line ") + std::to_string(__LINE__) + ": " + #condition); } while (false)

class TempDirectory final {
public:
    TempDirectory() : path_(std::filesystem::temp_directory_path() /
        ("cartographer-edge-checkpoint-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))) {
        REQUIRE(std::filesystem::create_directory(path_));
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

scene::ObjectId create_box(application::ApplicationSession& source) {
    REQUIRE(application::HumanApplicationAccess::dispatch(source,
        application::CreateBoxAction{"Edge target", {1.0, 1.0, 1.0}}));
    return source.snapshot().objects.front().object.id;
}

agent::AgentPlan plan_for(const application::ApplicationSession& source,
    scene::ObjectId object, geometry::EdgeId edge) {
    return agent::AgentPlan{"edge-task", agent::AuthoringObjective{
        "edge-objective", "Split one selected edge", source.snapshot().project_revision,
        {object}, {}, {}, {}, {}},
        {{"edge-step", {object, edge}, "split edge 0.25", {}}}};
}

agent::AgentWorkEnvelope envelope_for(scene::ObjectId object) {
    agent::AgentWorkEnvelope envelope;
    envelope.scope_objects = {object};
    envelope.max_commands = 1U;
    envelope.max_objects_touched = 1U;
    return envelope;
}

std::vector<std::string> fields(std::string_view bytes) {
    std::vector<std::string> result;
    std::size_t cursor = 0U;
    while (cursor < bytes.size()) {
        const auto end = bytes.find(':', cursor);
        REQUIRE(end != std::string_view::npos);
        std::size_t length = 0U;
        const auto parsed = std::from_chars(bytes.data() + cursor, bytes.data() + end, length);
        REQUIRE(parsed.ec == std::errc{} && parsed.ptr == bytes.data() + end);
        cursor = end + 1U;
        REQUIRE(length <= bytes.size() - cursor);
        result.emplace_back(bytes.substr(cursor, length));
        cursor += length;
        REQUIRE(cursor < bytes.size() && bytes[cursor] == '|');
        ++cursor;
    }
    return result;
}

std::string field(std::string_view value) {
    return std::to_string(value.size()) + ":" + std::string(value) + "|";
}

// Recompute both length framing and checksum, so rejection exercises the
// versioned target contract rather than merely detecting corrupted bytes.
std::string rewrite_version(std::string_view encoding, std::string version) {
    constexpr std::string_view magic = "CARTOGRAPHER_AGENT_TASK_CHECKPOINT_V1\n";
    REQUIRE(encoding.starts_with(magic));
    auto outer = fields(encoding.substr(magic.size()));
    REQUIRE(outer.size() == 2U);
    auto body_fields = fields(outer[0]);
    REQUIRE(body_fields.front() == "13");
    body_fields.front() = std::move(version);
    std::string body;
    for (const auto& value : body_fields) body += field(value);
    const auto digest = assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    return std::string(magic) + field(body) + field(digest.hex());
}

agent::AgentTaskCheckpoint saved_checkpoint(const agent::AgentTask& task,
    const std::filesystem::path& path) {
    REQUIRE(task.save_checkpoint(path));
    auto loaded = agent::AgentTask::load_checkpoint(path);
    REQUIRE(loaded);
    return std::move(loaded.value());
}

void prepared_targets_preserve_identity_and_version_boundaries() {
    TempDirectory directory;
    application::ApplicationSession source;
    const auto object = create_box(source);
    constexpr std::uint64_t large = 9007199254740993ULL;
    for (const auto identity : {large, std::numeric_limits<std::uint64_t>::max()}) {
        auto plan = plan_for(source, object, geometry::EdgeId{identity});
        const auto digest = plan.context_digest();
        REQUIRE(digest);
        auto changed = plan;
        changed.steps.front().target.component = geometry::EdgeId{identity - 1U};
        REQUIRE(changed.context_digest());
        REQUIRE(changed.context_digest().value() != digest.value());
        changed.steps.front().target.component = geometry::FaceId{identity};
        REQUIRE(changed.context_digest().value() != digest.value());
        auto task = agent::AgentTask::begin(source, plan, envelope_for(object));
        REQUIRE(task);
        auto checkpoint = saved_checkpoint(task.value(), directory.path() /
            (std::to_string(identity) + ".carto-agent-task"));
        REQUIRE(checkpoint.status == agent::TaskStatus::prepared);
        REQUIRE(checkpoint.plan_context_digest == digest.value());
        REQUIRE(std::get<geometry::EdgeId>(checkpoint.plan.steps.front().target.component).value == identity);
        const auto encoding = checkpoint.serialize();
        REQUIRE(encoding);
        const auto decoded = agent::AgentTaskCheckpoint::deserialize(encoding.value());
        REQUIRE(decoded);
        REQUIRE(decoded.value().plan_context_digest == digest.value());
        REQUIRE(std::get<geometry::EdgeId>(decoded.value().plan.steps.front().target.component).value == identity);
        const auto downgraded = agent::AgentTaskCheckpoint::deserialize(
            rewrite_version(encoding.value(), "12"));
        REQUIRE(!downgraded);
        REQUIRE(downgraded.error().message.find("target kind is unknown") != std::string::npos);
        auto forged = checkpoint;
        forged.plan.steps.front().target.component = geometry::EdgeId{identity - 1U};
        REQUIRE(!forged.validate());
    }
    auto invalid = plan_for(source, object, geometry::EdgeId{0U});
    REQUIRE(!invalid.steps.front().target.validate());
    REQUIRE(!agent::AgentTask::begin(source, invalid, envelope_for(object)));

    // v12 face targets retain the same encoding and digest after adding edges.
    auto legacy_plan = plan_for(source, object, geometry::EdgeId{1U});
    legacy_plan.steps.front().target.component = geometry::FaceId{1U};
    legacy_plan.steps.front().intent = "inset face 0.1";
    auto legacy_task = agent::AgentTask::begin(source, legacy_plan, envelope_for(object));
    REQUIRE(legacy_task);
    const auto legacy = saved_checkpoint(legacy_task.value(), directory.path() / "legacy.carto-agent-task");
    const auto bytes = legacy.serialize();
    REQUIRE(bytes);
    const auto decoded_legacy = agent::AgentTaskCheckpoint::deserialize(rewrite_version(bytes.value(), "12"));
    REQUIRE(decoded_legacy);
    REQUIRE(decoded_legacy.value().plan_context_digest == legacy.plan_context_digest);
    REQUIRE(std::get<geometry::FaceId>(decoded_legacy.value().plan.steps.front().target.component).value == 1U);
}

void split_checkpoint_replay_requires_explicit_acceptance() {
    TempDirectory directory;
    application::ApplicationSession source;
    const auto object = create_box(source);
    const auto before = source.snapshot();
    const auto& edges = before.viewport.scene.find(object)->mesh->triangle_edges;
    REQUIRE(!edges.empty());
    geometry::EdgeId edge{};
    for (const auto& triangle : edges) {
        for (const auto& candidate : triangle) {
            if (candidate.has_value()) { edge = *candidate; break; }
        }
        if (edge) break;
    }
    REQUIRE(edge);
    const auto plan = plan_for(source, object, edge);
    auto task = agent::AgentTask::begin(source, plan, envelope_for(object));
    REQUIRE(task);
    const auto run = task.value().run();
    REQUIRE(run);
    REQUIRE(run.value().status == agent::TaskStatus::review);
    REQUIRE(run.value().can_accept);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    const auto checkpoint = saved_checkpoint(task.value(), directory.path() / "split.carto-agent-task");
    auto retargeted = plan;
    retargeted.steps.front().target.component = geometry::EdgeId{edge.value + 1U};
    REQUIRE(!agent::AgentTask::resume_checkpoint(source, checkpoint, retargeted));
    auto resumed = agent::AgentTask::resume_checkpoint(source, checkpoint, plan);
    REQUIRE(resumed);
    REQUIRE(resumed.value().snapshot().status == agent::TaskStatus::review);
    REQUIRE(resumed.value().snapshot().can_accept);
    REQUIRE(resumed.value().snapshot().diff.digest == checkpoint.diff_digest);
    REQUIRE(source.snapshot().project_revision == before.project_revision);
    REQUIRE(application::HumanApplicationAccess::accept_agent_task(resumed.value()));
    REQUIRE(!application::HumanApplicationAccess::accept_agent_task(resumed.value()));
    const auto after = source.snapshot();
    REQUIRE(after.project_revision == before.project_revision.next());
    REQUIRE(after.viewport.scene.find(object)->mesh->vertex_ids.size() >
        before.viewport.scene.find(object)->mesh->vertex_ids.size());
    REQUIRE(!agent::AgentTask::resume_checkpoint(source, checkpoint, plan));
}
} // namespace

int main() {
    try {
        prepared_targets_preserve_identity_and_version_boundaries();
        split_checkpoint_replay_requires_explicit_acceptance();
        std::cout << "edge checkpoint tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
