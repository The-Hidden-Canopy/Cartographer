#include <carto/production/prepared_publication.hpp>

#include "link_test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::vector<std::uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-prepared-publication-tests-" + std::to_string(stamp) +
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

void write_text(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "test file opens");
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    require(static_cast<bool>(output), "test file writes completely");
}

struct PublicationFixture {
    carto::production::PreparedSceneEnvelope envelope;
    std::vector<carto::production::PreparedProductPayload> payloads;
};

PublicationFixture make_fixture(
    std::uint64_t revision,
    std::string content,
    std::string source_namespace = "public.fixture",
    std::string profile_identity = "portable.fixture.v1") {
    using namespace carto;
    PublicationFixture fixture;
    fixture.payloads.push_back({"mesh.1.render", bytes(content)});

    auto& envelope = fixture.envelope;
    envelope.source_namespace = std::move(source_namespace);
    envelope.source_revision = core::Revision{revision};
    envelope.source_digest = digest(
        envelope.source_namespace + "/source-" + std::to_string(revision));
    envelope.profile_identity = std::move(profile_identity);
    envelope.profile_digest = digest(envelope.profile_identity);
    envelope.products.push_back({
        .identity = "mesh.1.render",
        .kind = production::PreparedProductKind::render_mesh,
        .format_version = 1U,
        .content_digest = digest(content),
        .relative_path = "products/mesh-1.carto-render-mesh",
    });
    envelope.assets.push_back({
        .id = 1U,
        .name = "Fixture mesh",
        .render_mesh_product = "mesh.1.render",
        .lookdev_product = std::nullopt,
        .channel_mask = production::prepared_positions |
            production::prepared_normals |
            production::prepared_source_correspondence,
        .material_regions = {},
    });
    envelope.dependencies.push_back({
        .identity = "mesh.1.render",
        .disposition = production::PreparedDependencyDisposition::included_product,
        .required = true,
        .revision = core::Revision{revision},
        .content_digest = digest(content),
        .relative_path = "products/mesh-1.carto-render-mesh",
    });
    envelope.capabilities.push_back({
        .feature = "geometry",
        .evidence = production::PreparedCapabilityEvidence::derived_product,
        .required = true,
        .detail = "Fixture geometry is present as a verified neutral product.",
    });
    envelope.correspondences.push_back({
        .source_kind = production::PreparedSourceEntityKind::mesh_asset,
        .source_primary = 1U,
        .source_secondary = 0U,
        .prepared_kind = production::PreparedEntityKind::asset,
        .prepared_primary = 1U,
        .prepared_secondary = 0U,
    });
    require(static_cast<bool>(envelope.validate()), "publication fixture validates");
    return fixture;
}

carto::assets::Sha256Digest fixture_digest(
    const carto::production::PreparedSceneEnvelope& envelope) {
    const auto result = envelope.canonical_digest();
    require(static_cast<bool>(result), "fixture digest computes");
    return result.value();
}

carto::assets::Sha256Digest selected_digest(const std::filesystem::path& root) {
    const auto selected = carto::production::read_selected_prepared_scene(root);
    require(selected && selected.value().has_value(), "selected publication reopens");
    return selected.value()->prepared_scene_digest;
}

void publication_is_atomic_complete_and_relocatable() {
    using namespace carto;
    TempDirectory temp;
    const auto root = temp.path() / "published";
    const auto first = make_fixture(1U, "first-render-product");
    const auto second = make_fixture(2U, "second-render-product");
    const auto third = make_fixture(3U, "third-render-product");
    const auto first_digest = fixture_digest(first.envelope);
    const auto second_digest = fixture_digest(second.envelope);
    const auto third_digest = fixture_digest(third.envelope);

    const auto absent = production::read_selected_prepared_scene(root);
    require(absent && !absent.value().has_value(),
        "an unpublished root has no fabricated current revision");

    const auto published_first = production::publish_prepared_scene(
        root, first.envelope, first.payloads, std::nullopt);
    require(published_first &&
            published_first.value().prepared_scene_digest == first_digest &&
            !published_first.value().previous_selected_digest.has_value() &&
            !published_first.value().reused_immutable_revision,
        "first complete revision publishes and becomes current");
    require(selected_digest(root) == first_digest,
        "current resolves to the first verified revision");

    const std::vector<production::PreparedProductPayload> missing;
    require(!production::publish_prepared_scene(
                root, second.envelope, missing, first_digest),
        "missing product payload is rejected");
    auto wrong = second.payloads;
    wrong.front().bytes = bytes("tampered");
    require(!production::publish_prepared_scene(
                root, second.envelope, wrong, first_digest),
        "payload digest mismatch is rejected");
    auto extra = second.payloads;
    extra.push_back({"undeclared.product", bytes("extra")});
    require(!production::publish_prepared_scene(
                root, second.envelope, extra, first_digest),
        "undeclared payload is rejected");
    require(selected_digest(root) == first_digest,
        "invalid publication attempts do not alter current");

    const auto stale = production::publish_prepared_scene(
        root, second.envelope, second.payloads, std::nullopt);
    require(!stale && stale.error().code == core::ErrorCode::stale_data,
        "a writer with a stale selection expectation is rejected");
    require(selected_digest(root) == first_digest,
        "stale writer leaves current unchanged");

    const auto published_second = production::publish_prepared_scene(
        root, second.envelope, second.payloads, first_digest);
    require(published_second &&
            published_second.value().prepared_scene_digest == second_digest &&
            published_second.value().previous_selected_digest == first_digest,
        "a current writer atomically advances selection");
    const auto replay = production::publish_prepared_scene(
        root, second.envelope, second.payloads, second_digest);
    require(replay && replay.value().reused_immutable_revision,
        "an exact replay reuses the verified immutable revision");

    const auto conflict_path = root / "revisions" / third_digest.hex();
    write_text(conflict_path / "manifest.carto-prepared", "corrupt");
    const auto conflict = production::publish_prepared_scene(
        root, third.envelope, third.payloads, second_digest);
    require(!conflict, "a conflicting immutable identity is rejected");
    require(selected_digest(root) == second_digest,
        "conflicting immutable content cannot advance current");

    const auto relocated = temp.path() / "relocated";
    std::filesystem::copy(root, relocated, std::filesystem::copy_options::recursive);
    require(selected_digest(relocated) == second_digest,
        "publication remains valid after relocation");

    const auto selected = production::read_selected_prepared_scene(relocated);
    require(selected && selected.value().has_value(), "relocated product resolves");
    write_text(
        selected.value()->revision_path /
            selected.value()->envelope.products.front().relative_path,
        "corrupted-after-publication");
    require(!production::read_selected_prepared_scene(relocated),
        "post-publication product corruption is detected");

    const auto with_extra = temp.path() / "with-extra";
    std::filesystem::copy(root, with_extra, std::filesystem::copy_options::recursive);
    const auto extra_revision = with_extra / "revisions" / second_digest.hex();
    write_text(extra_revision / "undeclared.bin", "undeclared");
    require(!production::read_selected_prepared_scene(with_extra),
        "undeclared files invalidate an otherwise complete revision");

    const auto malformed = temp.path() / "malformed";
    write_text(malformed / "current", "REVISION definitely-not-a-digest\n");
    require(!production::read_selected_prepared_scene(malformed),
        "malformed current selector fails closed");
}

void concurrent_writers_observe_compare_and_swap() {
    using namespace carto;
    TempDirectory temp;
    const auto root = temp.path() / "concurrent";
    const auto first = make_fixture(10U, "concurrent-first");
    const auto second = make_fixture(11U, "concurrent-second");
    const auto third = make_fixture(12U, "concurrent-third");
    const auto first_digest = fixture_digest(first.envelope);
    const auto second_digest = fixture_digest(second.envelope);
    const auto third_digest = fixture_digest(third.envelope);
    require(static_cast<bool>(production::publish_prepared_scene(
                root, first.envelope, first.payloads, std::nullopt)),
        "concurrency fixture publishes its initial revision");

    std::atomic<bool> start{false};
    bool second_succeeded = false;
    bool third_succeeded = false;
    core::ErrorCode second_error = core::ErrorCode::invalid_state;
    core::ErrorCode third_error = core::ErrorCode::invalid_state;
    const auto writer = [&](const PublicationFixture& fixture, bool& succeeded,
                            core::ErrorCode& error) {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto result = production::publish_prepared_scene(
            root, fixture.envelope, fixture.payloads, first_digest);
        succeeded = static_cast<bool>(result);
        if (!result) error = result.error().code;
    };
    std::thread left(writer, std::cref(second),
        std::ref(second_succeeded), std::ref(second_error));
    std::thread right(writer, std::cref(third),
        std::ref(third_succeeded), std::ref(third_error));
    start.store(true, std::memory_order_release);
    left.join();
    right.join();

    require(second_succeeded != third_succeeded,
        "exactly one concurrent writer advances the shared selection");
    require((second_succeeded || second_error == core::ErrorCode::stale_data) &&
            (third_succeeded || third_error == core::ErrorCode::stale_data),
        "the losing concurrent writer receives an explicit stale result");
    require(selected_digest(root) ==
            (second_succeeded ? second_digest : third_digest),
        "current resolves only to the winning complete revision");
}

void publication_is_bound_to_source_lineage() {
    using namespace carto;
    TempDirectory temp;
    const auto root = temp.path() / "lineage";
    const auto current = make_fixture(2U, "revision-two");
    const auto current_digest = fixture_digest(current.envelope);
    require(static_cast<bool>(production::publish_prepared_scene(
                root, current.envelope, current.payloads, std::nullopt)),
        "lineage fixture publishes its current revision");

    const auto foreign = make_fixture(
        3U, "foreign-revision", "another.project");
    const auto foreign_digest = fixture_digest(foreign.envelope);
    const auto foreign_result = production::publish_prepared_scene(
        root, foreign.envelope, foreign.payloads, current_digest);
    require(!foreign_result &&
            foreign_result.error().code == core::ErrorCode::stale_data,
        "a foreign project cannot replace a publication root");
    require(!std::filesystem::exists(
                root / "revisions" / foreign_digest.hex()),
        "foreign project rejection writes no immutable revision");
    require(selected_digest(root) == current_digest,
        "foreign project rejection preserves the selected revision");

    const auto older = make_fixture(1U, "revision-one");
    const auto older_digest = fixture_digest(older.envelope);
    const auto rollback = production::publish_prepared_scene(
        root, older.envelope, older.payloads, current_digest);
    require(!rollback && rollback.error().code == core::ErrorCode::stale_data,
        "a refreshed writer still cannot roll source revision backward");
    require(!std::filesystem::exists(root / "revisions" / older_digest.hex()),
        "revision rollback rejection writes no immutable revision");
    require(selected_digest(root) == current_digest,
        "revision rollback rejection preserves the selected revision");

    auto divergent = make_fixture(2U, "divergent-revision-two");
    divergent.envelope.source_digest = digest("divergent-source-revision-two");
    require(static_cast<bool>(divergent.envelope.validate()),
        "divergent source fixture remains structurally valid");
    const auto divergent_result = production::publish_prepared_scene(
        root, divergent.envelope, divergent.payloads, current_digest);
    require(!divergent_result &&
            divergent_result.error().code == core::ErrorCode::stale_data,
        "one source revision cannot be rebound to different source content");
    require(selected_digest(root) == current_digest,
        "source identity collision preserves the selected revision");

    const auto profile_variant = make_fixture(
        2U, "revision-two", "public.fixture", "unity.fixture.v1");
    const auto profile_digest = fixture_digest(profile_variant.envelope);
    const auto profile_result = production::publish_prepared_scene(
        root, profile_variant.envelope, profile_variant.payloads, current_digest);
    require(profile_result && selected_digest(root) == profile_digest,
        "another preparation profile may publish from the same source revision");
}

void publication_rejects_linked_ancestor_paths() {
    using namespace carto;
    TempDirectory temp;
    const auto actual_parent = temp.path() / "actual-parent";
    const auto linked_parent = temp.path() / "linked-parent";
    std::filesystem::create_directories(actual_parent);
    std::error_code error;
    require(carto::test_support::create_directory_link(
                actual_parent, linked_parent, error),
        "test creates a symbolic-link or junction ancestor");

    const auto fixture = make_fixture(1U, "linked-path-product");
    const auto through_link = linked_parent / "published";
    const auto rejected = production::publish_prepared_scene(
        through_link, fixture.envelope, fixture.payloads, std::nullopt);
    require(!rejected &&
            rejected.error().code == core::ErrorCode::validation_failed,
        "publication refuses a symbolic-link ancestor");
    require(!std::filesystem::exists(actual_parent / "published"),
        "linked-ancestor rejection performs no publication mutation");

    const auto direct = actual_parent / "published";
    require(static_cast<bool>(production::publish_prepared_scene(
                direct, fixture.envelope, fixture.payloads, std::nullopt)),
        "direct publication remains valid after linked-path rejection");
    require(!production::read_selected_prepared_scene(through_link),
        "reader refuses to traverse a symbolic-link ancestor");
}

void publication_rejects_missing_selector_over_nonempty_store() {
    using namespace carto;
    TempDirectory temp;
    const auto root = temp.path() / "missing-selector";
    const auto current = make_fixture(2U, "current-revision");
    const auto current_digest = fixture_digest(current.envelope);
    require(static_cast<bool>(production::publish_prepared_scene(
                root, current.envelope, current.payloads, std::nullopt)),
        "missing-selector fixture publishes its current revision");

    std::error_code error;
    require(std::filesystem::remove(root / "current", error) && !error,
        "test removes only the selector and preserves immutable revisions");
    require(!production::read_selected_prepared_scene(root),
        "reader fails closed when a selector is missing over stored revisions");

    const auto foreign = make_fixture(
        3U, "foreign-after-selector-loss", "another.project");
    const auto foreign_digest = fixture_digest(foreign.envelope);
    const auto takeover = production::publish_prepared_scene(
        root, foreign.envelope, foreign.payloads, std::nullopt);
    require(!takeover && takeover.error().code == core::ErrorCode::stale_data,
        "selector loss cannot turn an owned publication root into an empty root");
    require(!std::filesystem::exists(root / "revisions" / foreign_digest.hex()),
        "selector-loss takeover rejection writes no foreign revision");
    require(std::filesystem::exists(root / "revisions" / current_digest.hex()),
        "selector-loss rejection preserves the existing immutable revision");
}

} // namespace

int main() {
    publication_is_atomic_complete_and_relocatable();
    concurrent_writers_observe_compare_and_swap();
    publication_is_bound_to_source_lineage();
    publication_rejects_linked_ancestor_paths();
    publication_rejects_missing_selector_over_nonempty_store();
    std::cout << "prepared publication tests passed\n";
    return EXIT_SUCCESS;
}
