#include <carto/production/preparation_job.hpp>
#include <carto/project/project.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-preparation-job-tests-" + std::to_string(stamp) +
             "-" + std::to_string(sequence.fetch_add(1U)));
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

carto::production::PreparationOperationRequest request_for(
    const carto::project::ProjectDocument& document,
    const std::filesystem::path& source_root,
    const std::filesystem::path& output_root,
    std::stop_token stop_token = {}) {
    return carto::production::PreparationOperationRequest{
        .scope = {
            carto::production::PreparationScopeKind::scene,
            document.revision(),
            {},
            {},
        },
        .profile = carto::production::portable_preparation_profile(),
        .source_root = source_root,
        .output_root = output_root,
        .expected_selected_digest = std::nullopt,
        .stop_token = stop_token,
    };
}

void background_and_headless_paths_agree() {
    using namespace carto;
    TempDirectory temp;
    const project::ProjectDocument document;
    const auto direct = production::prepare_and_publish(
        document, request_for(document, temp.path(), temp.path() / "direct"));
    require(direct.succeeded(), "direct empty-scene preparation succeeds");

    auto started = production::PreparationJob::start(
        document, request_for(document, temp.path(), temp.path() / "background"));
    require(static_cast<bool>(started), "background preparation job starts");
    auto job = std::move(started.value());
    job->wait();
    const auto snapshot = job->snapshot();
    const auto report = job->report();
    require(snapshot.state == production::PreparationJobState::published &&
            report.has_value() && report->succeeded(),
        "background preparation reaches a published terminal state");
    require(report->operation_id == direct.operation_id &&
            report->envelope->serialize() == direct.envelope->serialize() &&
            report->publication->prepared_scene_digest ==
                direct.publication->prepared_scene_digest,
        "background and headless preparation have identical logical output");
    require(!job->request_cancel(),
        "cancellation cannot relabel an already published operation");
}

void external_cancellation_never_publishes() {
    using namespace carto;
    for (std::uint32_t attempt = 0U; attempt < 32U; ++attempt) {
        TempDirectory temp;
        const project::ProjectDocument document;
        std::stop_source stop;
        stop.request_stop();
        auto started = production::PreparationJob::start(
            document, request_for(document, temp.path(), temp.path() / "cancelled",
                          stop.get_token()));
        require(static_cast<bool>(started),
            "pre-cancelled job still returns a receipt owner");
        auto job = std::move(started.value());
        job->wait();
        const auto snapshot = job->snapshot();
        const auto report = job->report();
        require(snapshot.state == production::PreparationJobState::cancelled &&
                report.has_value() && !report->succeeded() &&
                !report->diagnostics.empty() &&
                report->diagnostics.front().code ==
                    "carto.prepare.resolve.cancelled",
            "external cancellation deterministically becomes a cancelled result");
        require(!std::filesystem::exists(temp.path() / "cancelled"),
            "cancelled background job creates no publication state");
    }
}

void stale_background_result_preserves_current() {
    using namespace carto;
    TempDirectory temp;
    const project::ProjectDocument document;
    const auto output_root = temp.path() / "prepared";
    const auto baseline = production::prepare_and_publish(
        document, request_for(document, temp.path(), output_root));
    require(baseline.succeeded(), "stale job fixture publishes its baseline");

    auto stale_request = request_for(document, temp.path(), output_root);
    stale_request.expected_selected_digest = std::nullopt;
    auto started = production::PreparationJob::start(document, stale_request);
    require(static_cast<bool>(started), "stale background candidate starts");
    auto job = std::move(started.value());
    job->wait();
    const auto report = job->report();
    require(job->snapshot().state == production::PreparationJobState::stale &&
            report.has_value() && !report->succeeded() &&
            !report->diagnostics.empty() &&
            report->diagnostics.back().code ==
                "carto.prepare.publish.stale_data",
        "compare-and-swap rejection remains a distinct stale job outcome");
    const auto selected = production::read_selected_prepared_scene(output_root);
    require(selected && selected.value().has_value() &&
            selected.value()->prepared_scene_digest ==
                baseline.publication->prepared_scene_digest,
        "stale background candidate preserves the selected valid output");
}

} // namespace

int main() {
    background_and_headless_paths_agree();
    external_cancellation_never_publishes();
    stale_background_result_preserves_current();
    std::cout << "preparation job tests passed\n";
    return EXIT_SUCCESS;
}
