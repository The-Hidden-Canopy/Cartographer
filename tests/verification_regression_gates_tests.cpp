#include <carto/verification/regression_gate.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/production/material_artifact.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/prepared_scene.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/transaction.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::production::PreparedCapabilityEvidence;
using carto::verification::CapabilityExpectation;
using carto::verification::CorrespondenceCountExpectation;
using carto::verification::RegressionGateOutcome;
using carto::verification::RegressionGateSpec;

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
            ("cartographer-verification-gate-tests-" + std::to_string(stamp) +
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

carto::geometry::EditableMesh workshop_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    require(static_cast<bool>(mesh.insert_bulk(
                {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
                 {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
                 {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
                 {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
                {{geometry::FaceId{1U},
                  {geometry::VertexId{1U}, geometry::VertexId{2U},
                   geometry::VertexId{3U}}},
                 {geometry::FaceId{2U},
                  {geometry::VertexId{1U}, geometry::VertexId{3U},
                   geometry::VertexId{4U}}}})),
        "workshop mesh topology is valid");
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, 6U},
        {attributes::AttributeDomain::face, 2U},
    });
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"uv0.corners", attributes::AttributeDomain::corner,
                 attributes::AttributeType::vec2},
                {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
                 core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 0.0},
                 core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}})),
        "workshop mesh owns UV0 corners");
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"condition.material_region", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                {std::uint32_t{3U}, std::uint32_t{9U}}})),
        "workshop mesh owns stable material regions");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
                1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "workshop mesh declares UV0");
    require(static_cast<bool>(mesh.set_attribute_payload(
                std::move(attributes), std::move(uv_sets))),
        "workshop mesh accepts its material-region payload");
    return mesh;
}

carto::render::MaterialAsset material(
    carto::render::MaterialId id,
    std::string name) {
    using namespace carto;
    render::MaterialAsset value;
    value.id = id;
    value.name = std::move(name);
    value.revision = core::Revision{1U};
    value.definition.roughness = 0.8;
    return value;
}

carto::project::MaterialCatalog workshop_catalog() {
    using namespace carto;
    project::MaterialCatalog catalog;
    require(static_cast<bool>(catalog.insert_texture(project::TextureSourceRecord{
                .id = 501U,
                .name = "Workshop shared image",
                .relative_locator = "textures/workshop-shared.png",
                .content_digest = digest("workshop-shared-pixels"),
                .format = assets::TextureSourceFormat::png,
                .width = 512U,
                .height = 512U,
                .provenance = "verification-gate-fixture",
                .revision = core::Revision{1U},
            })),
        "workshop source texture enters catalog");
    require(static_cast<bool>(catalog.insert_sampler(project::SamplerIntent{
                .id = 31U,
                .name = "Workshop linear repeat",
                .revision = core::Revision{1U},
            })),
        "workshop sampler enters catalog");
    require(static_cast<bool>(catalog.insert_material(material(71U, "Painted shell"))),
        "paint material enters catalog");
    require(static_cast<bool>(catalog.insert_material(material(72U, "Rubber grip"))),
        "rubber material enters catalog");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {1U, 3U}, .material = 71U, .variant = std::nullopt})),
        "paint region receives its source assignment");
    require(static_cast<bool>(catalog.insert_assignment({
                .key = {1U, 9U}, .material = 72U, .variant = std::nullopt})),
        "rubber region receives its source assignment");
    require(static_cast<bool>(catalog.validate()), "workshop catalog validates");
    return catalog;
}

struct Fixture {
    carto::project::ProjectDocument document;
    std::uint64_t mesh_asset = 0U;
    carto::production::RenderMeshArtifact render_mesh;
    carto::production::LookdevDrawPackage lookdev;
};

Fixture make_fixture(carto::journal::Journal& journal) {
    using namespace carto;
    Fixture fixture;
    auto transaction = project::ProjectTransaction::begin(
        fixture.document, journal, "verification-gate-test");
    require(static_cast<bool>(transaction), "gate fixture transaction begins");
    const auto mesh = transaction.value().add_mesh(workshop_mesh());
    require(static_cast<bool>(mesh), "workshop mesh enters project");
    fixture.mesh_asset = mesh.value();
    require(static_cast<bool>(transaction.value().set_material_catalog(
                workshop_catalog())),
        "workshop catalog enters project");
    const auto root = transaction.value().create_object("Workshop root");
    require(static_cast<bool>(root) &&
            transaction.value().attach_mesh(root.value(), mesh.value()),
        "workshop placement references the reusable asset");
    const auto second = transaction.value().create_object(
        "Workshop repeat", core::Transform{{2.0, 0.0, 0.0}});
    require(static_cast<bool>(second) &&
            transaction.value().attach_mesh(second.value(), mesh.value()),
        "second placement references the same reusable asset");
    require(static_cast<bool>(transaction.value().commit("gate.fixture")),
        "gate fixture commits through one journal event");

    production::RenderMeshCompileOptions options;
    options.uv_set = 1U;
    options.generate_tangents = true;
    options.material_slot_layer = "condition.material_region";
    const auto compiled_mesh = production::compile_render_mesh_artifact(
        "public.workshop", fixture.document, fixture.mesh_asset, options);
    require(static_cast<bool>(compiled_mesh), "exact workshop render mesh compiles");
    fixture.render_mesh = compiled_mesh.value();

    const auto& catalog = fixture.document.material_catalog();
    const auto paint = production::compile_material_artifact(
        "public.workshop.material.71", catalog.materials().at(71U));
    const auto rubber = production::compile_material_artifact(
        "public.workshop.material.72", catalog.materials().at(72U));
    require(paint && rubber, "workshop material products compile");
    const std::vector<production::LookdevMaterialBinding> bindings{
        {3U, paint.value()},
        {9U, rubber.value()},
    };
    const auto lookdev = production::compile_lookdev_draw_package(
        fixture.render_mesh, bindings);
    require(static_cast<bool>(lookdev), "workshop lookdev package compiles");
    fixture.lookdev = lookdev.value();
    return fixture;
}

carto::production::PreparedSceneEnvelope compose(const Fixture& fixture) {
    using namespace carto;
    const production::PreparedAssetInput input{
        fixture.mesh_asset, &fixture.render_mesh, &fixture.lookdev};
    const auto prepared = production::compose_prepared_scene(
        "public.workshop", fixture.document, "portable.static-workshop.v1",
        digest("portable.static-workshop.v1"), std::span{&input, 1U});
    require(static_cast<bool>(prepared), "neutral prepared scene composes");
    return prepared.value();
}

std::vector<carto::production::PreparedProductPayload> make_payloads(
    const Fixture& fixture,
    const carto::production::PreparedSceneEnvelope& envelope) {
    using namespace carto;
    const auto scope = production::resolve_preparation_scope(fixture.document, {
        production::PreparationScopeKind::scene,
        fixture.document.revision(), {}, {}});
    require(static_cast<bool>(scope), "payload material scope resolves");
    const auto materials = production::compose_prepared_material_catalog(
        fixture.document, scope.value());
    require(static_cast<bool>(materials), "payload material catalog composes");
    const auto render_bytes = fixture.render_mesh.serialize();
    require(static_cast<bool>(render_bytes), "render mesh serializes");
    const auto lookdev_bytes = production::serialize_lookdev_draw_package(
        fixture.lookdev);
    require(static_cast<bool>(lookdev_bytes), "lookdev package serializes");

    std::vector<production::PreparedProductPayload> payloads;
    for (const auto& product : envelope.products) {
        std::vector<std::uint8_t> content;
        switch (product.kind) {
        case production::PreparedProductKind::render_mesh:
            content = render_bytes.value();
            break;
        case production::PreparedProductKind::lookdev_draw_package:
            content = lookdev_bytes.value();
            break;
        case production::PreparedProductKind::material_catalog: {
            const auto text = materials.value().serialize();
            content.assign(text.begin(), text.end());
            break;
        }
        case production::PreparedProductKind::texture_source:
            content = bytes("workshop-shared-pixels");
            break;
        case production::PreparedProductKind::world_model: {
            const auto text = fixture.document.world_model().serialize();
            content.assign(text.begin(), text.end());
            break;
        }
        default:
            require(false, "payload builder met an unknown product kind");
        }
        payloads.push_back({product.identity, std::move(content)});
    }
    return payloads;
}

RegressionGateSpec exact_spec(const carto::production::PreparedSceneEnvelope& prepared) {
    RegressionGateSpec spec;
    const auto observed = prepared.canonical_digest();
    require(static_cast<bool>(observed), "observed prepared digest computes");
    spec.expected_prepared_digest = observed.value();
    for (const auto& capability : prepared.capabilities) {
        spec.expected_capabilities.push_back(CapabilityExpectation{
            capability.feature, capability.evidence, capability.required});
    }
    std::map<std::pair<unsigned, unsigned>, std::uint64_t> counts;
    for (const auto& correspondence : prepared.correspondences) {
        ++counts[{static_cast<unsigned>(correspondence.source_kind),
            static_cast<unsigned>(correspondence.prepared_kind)}];
    }
    for (const auto& [key, tally] : counts) {
        spec.expected_correspondence_counts.push_back(CorrespondenceCountExpectation{
            static_cast<carto::production::PreparedSourceEntityKind>(key.first),
            static_cast<carto::production::PreparedEntityKind>(key.second),
            tally});
    }
    require(static_cast<bool>(spec.normalize()),
        "exact expectations normalize into canonical form");
    return spec;
}

void gate_passes_on_exact_fixture_project() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "gate-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto spec = exact_spec(prepared);

    const auto outcome = verification::run_regression_gate(spec, prepared);
    require(outcome && outcome.value().passed &&
            outcome.value().failures.empty() &&
            outcome.value().observed_prepared_digest == spec.expected_prepared_digest,
        "an exact fixture project passes its regression gate");

    const auto published = production::publish_prepared_scene(
        temp.path() / "published", prepared, make_payloads(fixture, prepared),
        std::nullopt);
    require(static_cast<bool>(published), "gate fixture publishes");
    const auto with_receipt = verification::run_regression_gate(
        spec, prepared, &published.value());
    require(with_receipt && with_receipt.value().passed,
        "a matching publication receipt also passes the gate");
}

void gate_fails_on_any_drift() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "gate-drift-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto spec = exact_spec(prepared);

    auto wrong_digest = spec;
    wrong_digest.expected_prepared_digest = digest("some-other-prepared-scene");
    const auto digest_outcome = verification::run_regression_gate(
        wrong_digest, prepared);
    require(digest_outcome && !digest_outcome.value().passed &&
            !digest_outcome.value().failures.empty(),
        "a drifted prepared digest fails the gate with reasons");

    auto wrong_capability = spec;
    for (auto& expectation : wrong_capability.expected_capabilities) {
        if (expectation.feature == "scene.geometry") {
            expectation.evidence = PreparedCapabilityEvidence::preserved_source;
        }
    }
    const auto capability_outcome = verification::run_regression_gate(
        wrong_capability, prepared);
    require(capability_outcome && !capability_outcome.value().passed,
        "drifted capability evidence fails the gate");

    auto extra_capability = spec;
    extra_capability.expected_capabilities.push_back(CapabilityExpectation{
        "scene.extra-capability", PreparedCapabilityEvidence::derived_product, false});
    require(static_cast<bool>(extra_capability.normalize()),
        "extra expectation normalizes for evaluation");
    const auto extra_outcome = verification::run_regression_gate(
        extra_capability, prepared);
    require(extra_outcome && !extra_outcome.value().passed,
        "an unexpected capability fails the exact-set gate");

    auto wrong_correspondence = spec;
    require(!wrong_correspondence.expected_correspondence_counts.empty(),
        "fixture carries correspondence expectations");
    wrong_correspondence.expected_correspondence_counts.front().expected_count += 1U;
    const auto correspondence_outcome = verification::run_regression_gate(
        wrong_correspondence, prepared);
    require(correspondence_outcome && !correspondence_outcome.value().passed,
        "drifted correspondence counts fail the gate");

    const production::PreparedPublicationReceipt foreign_receipt{
        digest("foreign-admitted-revision"),
        digest("foreign-dependency-closure"),
        std::nullopt,
        prepared.source_revision,
        temp.path() / "published" / "revisions" / digest("foreign").hex(),
        false,
    };
    const auto foreign_outcome = verification::run_regression_gate(
        spec, prepared, &foreign_receipt);
    require(foreign_outcome && !foreign_outcome.value().passed,
        "a publication receipt from another admitted revision fails the gate");
}

void gate_records_and_specs_round_trip() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "gate-roundtrip-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto spec = exact_spec(prepared);

    const auto text = spec.serialize();
    const auto reopened = RegressionGateSpec::deserialize(text);
    require(reopened && reopened.value().serialize() == text,
        "regression gate spec survives a deterministic round trip");

    std::string future = text;
    future.replace(0U, std::string{"CARTOGRAPHER_REGRESSION_GATE_SPEC 1"}.size(),
        "CARTOGRAPHER_REGRESSION_GATE_SPEC 99");
    const auto future_result = RegressionGateSpec::deserialize(future);
    require(!future_result &&
            future_result.error().code == core::ErrorCode::version_mismatch,
        "unknown future gate spec versions fail precisely");
    require(!RegressionGateSpec::deserialize(text + "trailing"),
        "trailing unbound gate spec data is rejected");

    std::string oversized = text;
    const auto capability_pos = oversized.find("CAPABILITIES ");
    require(capability_pos != std::string::npos, "gate spec carries capability count");
    oversized.replace(capability_pos, std::string{"CAPABILITIES 4"}.size(),
        "CAPABILITIES 99999");
    require(!RegressionGateSpec::deserialize(oversized),
        "hostile gate spec counts are rejected before allocation");

    const std::string duplicated =
        "CARTOGRAPHER_REGRESSION_GATE_SPEC 1\n"
        "DIGEST " + digest("gate").hex() + "\n"
        "CAPABILITIES 2\n"
        "CAPABILITY \"scene.a\" 1 1\n"
        "CAPABILITY \"scene.a\" 1 0\n"
        "CORRESPONDENCES 0\n"
        "END\n";
    require(!RegressionGateSpec::deserialize(duplicated),
        "duplicated capability expectations fail closed");

    const auto failing = verification::run_regression_gate(
        [] {
            RegressionGateSpec drifted;
            drifted.expected_prepared_digest = digest("drifted");
            return drifted;
        }(),
        prepared);
    require(failing && !failing.value().passed,
        "drifted gate produces a failing outcome for serialization");
    const auto outcome_text = failing.value().canonical();
    const auto outcome_reopened = RegressionGateOutcome::deserialize(outcome_text);
    require(outcome_reopened &&
            outcome_reopened.value().canonical() == outcome_text &&
            !outcome_reopened.value().passed,
        "failing gate outcome survives a deterministic round trip");

    RegressionGateOutcome inconsistent;
    inconsistent.passed = true;
    inconsistent.observed_prepared_digest = digest("observed");
    inconsistent.failures.push_back("leftover failure");
    require(!inconsistent.validate(),
        "a passing outcome may not carry failure reasons");
    RegressionGateOutcome reasonless;
    reasonless.passed = false;
    reasonless.observed_prepared_digest = digest("observed");
    require(!reasonless.validate(),
        "a failing outcome requires at least one failure reason");
}

} // namespace

int main() {
    gate_passes_on_exact_fixture_project();
    gate_fails_on_any_drift();
    gate_records_and_specs_round_trip();
    std::cout << "verification regression gate tests passed\n";
    return EXIT_SUCCESS;
}
