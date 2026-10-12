#include <carto/verification/evidence_report.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/lookdev_draw_package.hpp>
#include <carto/production/material_artifact.hpp>
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

using carto::verification::CapabilityLedger;
using carto::verification::CapabilityNonClaim;
using carto::verification::NamedGateOutcome;
using carto::verification::PipelineEvidenceReport;
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

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-verification-evidence-tests-" + std::to_string(stamp) +
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
                .provenance = "verification-evidence-fixture",
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
        fixture.document, journal, "verification-evidence-test");
    require(static_cast<bool>(transaction), "evidence fixture transaction begins");
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
    require(static_cast<bool>(transaction.value().commit("evidence.fixture")),
        "evidence fixture commits through one journal event");

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

CapabilityLedger truthful_ledger(
    const carto::production::PreparedSceneEnvelope& prepared) {
    using namespace carto;
    CapabilityLedger ledger;
    const auto admission_receipt = digest("workshop-admission-receipt");
    require(static_cast<bool>(ledger.record_prepared_capabilities(
                prepared.capabilities, admission_receipt)),
        "prepared capabilities record into the truthful ledger");
    CapabilityNonClaim non_claim;
    non_claim.feature = "scene.runtime-destruction";
    non_claim.admission_receipt_digest = admission_receipt;
    non_claim.reason =
        "no neutral prepared product provides runtime destruction semantics";
    require(static_cast<bool>(ledger.non_claim(std::move(non_claim))),
        "explicit non-claim enters the evidence ledger");
    return ledger;
}

RegressionGateSpec exact_spec(const carto::production::PreparedSceneEnvelope& prepared) {
    RegressionGateSpec spec;
    const auto observed = prepared.canonical_digest();
    require(static_cast<bool>(observed), "observed prepared digest computes");
    spec.expected_prepared_digest = observed.value();
    for (const auto& capability : prepared.capabilities) {
        spec.expected_capabilities.push_back(
            carto::verification::CapabilityExpectation{
                capability.feature, capability.evidence, capability.required});
    }
    std::map<std::pair<unsigned, unsigned>, std::uint64_t> counts;
    for (const auto& correspondence : prepared.correspondences) {
        ++counts[{static_cast<unsigned>(correspondence.source_kind),
            static_cast<unsigned>(correspondence.prepared_kind)}];
    }
    for (const auto& [key, tally] : counts) {
        spec.expected_correspondence_counts.push_back(
            carto::verification::CorrespondenceCountExpectation{
                static_cast<carto::production::PreparedSourceEntityKind>(key.first),
                static_cast<carto::production::PreparedEntityKind>(key.second),
                tally});
    }
    require(static_cast<bool>(spec.normalize()),
        "exact expectations normalize into canonical form");
    return spec;
}

void report_assembles_full_pipeline_evidence() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "evidence-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto ledger = truthful_ledger(prepared);
    const auto spec = exact_spec(prepared);
    const auto gate = verification::run_regression_gate(spec, prepared);
    require(gate && gate.value().passed, "evidence fixture gate passes");
    const std::vector<NamedGateOutcome> gates{
        NamedGateOutcome{"workshop-scene-gate", gate.value()},
    };

    const auto report = verification::assemble_pipeline_evidence_report(
        prepared, ledger, std::span{gates});
    require(report && static_cast<bool>(report.value().validate()),
        "assembled evidence report validates");
    const auto& value = report.value();
    require(value.source_namespace == prepared.source_namespace &&
            value.source_revision == prepared.source_revision &&
            value.source_digest == prepared.source_digest &&
            value.profile_identity == prepared.profile_identity &&
            value.profile_digest == prepared.profile_digest,
        "report binds source identity and preparation profile");
    require(value.capability_claims.size() == ledger.claims().size() &&
            value.capability_non_claims.size() == ledger.non_claims().size(),
        "report carries the ledger claims and non-claims verbatim");
    require(value.admission_receipt_digests.size() == 1U &&
            value.admission_receipt_digests.front() == digest("workshop-admission-receipt"),
        "admission receipt digests collapse to one unique sorted binding");
    require(value.correspondence_counts.size() == 3U,
        "fixture correspondence tallies group into three entity pairs");
    require(value.gate_outcomes.size() == 1U &&
            value.gate_outcomes.front().gate_identity == "workshop-scene-gate" &&
            value.gate_outcomes.front().outcome.passed,
        "gate outcomes bind to the report by stable identity");
}

void report_is_deterministic_and_round_trips() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "evidence-determinism-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto ledger = truthful_ledger(prepared);
    const auto spec = exact_spec(prepared);
    const auto gate = verification::run_regression_gate(spec, prepared);
    require(gate && gate.value().passed, "determinism fixture gate passes");
    const std::vector<NamedGateOutcome> gates{
        NamedGateOutcome{"workshop-scene-gate", gate.value()},
    };

    const auto first = verification::assemble_pipeline_evidence_report(
        prepared, ledger, std::span{gates});
    const auto second = verification::assemble_pipeline_evidence_report(
        prepared, ledger, std::span{gates});
    require(first && second, "both evidence assemblies succeed");
    const auto first_digest = first.value().canonical_digest();
    const auto second_digest = second.value().canonical_digest();
    require(first.value().serialize() == second.value().serialize() &&
            first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "same admitted truth assembles to the same report digest");

    const auto text = first.value().serialize();
    const auto reopened = PipelineEvidenceReport::deserialize(text);
    require(reopened && reopened.value().serialize() == text &&
            static_cast<bool>(reopened.value().validate()),
        "evidence report survives a deterministic digest-bound round trip");
    const auto reopened_digest = reopened.value().canonical_digest();
    require(reopened_digest && reopened_digest.value() == first_digest.value(),
        "report identity survives reopen");
}

void tampered_and_hostile_reports_fail_closed() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "evidence-hostile-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto ledger = truthful_ledger(prepared);
    const auto empty_gates = std::span<const NamedGateOutcome>{};
    const auto report = verification::assemble_pipeline_evidence_report(
        prepared, ledger, empty_gates);
    require(report && static_cast<bool>(report.value().validate()),
        "minimal evidence report assembles without gates");

    auto tampered = report.value();
    tampered.source_revision = core::Revision{prepared.source_revision.value() + 41U};
    require(!tampered.validate(),
        "a tampered source revision breaks the bound report digest");
    auto unbound = report.value();
    unbound.report_digest = digest("rebound-by-attacker");
    require(!unbound.validate(),
        "a rebound report digest fails against canonical content");

    const auto text = report.value().serialize();
    std::string future = text;
    future.replace(
        0U, std::string{"CARTOGRAPHER_PIPELINE_EVIDENCE_REPORT 1"}.size(),
        "CARTOGRAPHER_PIPELINE_EVIDENCE_REPORT 99");
    const auto future_result = PipelineEvidenceReport::deserialize(future);
    require(!future_result &&
            future_result.error().code == core::ErrorCode::version_mismatch,
        "unknown future evidence report versions fail precisely");
    require(!PipelineEvidenceReport::deserialize(text + "trailing"),
        "trailing unbound report data is rejected");

    std::string oversized = text;
    const auto claim_pos = oversized.find("CLAIMS ");
    require(claim_pos != std::string::npos, "evidence report carries a claim count");
    const auto claim_end = oversized.find('\n', claim_pos);
    oversized.replace(claim_pos, claim_end - claim_pos, "CLAIMS 99999");
    require(!PipelineEvidenceReport::deserialize(oversized),
        "hostile evidence report counts are rejected before allocation");
}

} // namespace

int main() {
    report_assembles_full_pipeline_evidence();
    report_is_deterministic_and_round_trips();
    tampered_and_hostile_reports_fail_closed();
    std::cout << "verification evidence report tests passed\n";
    return EXIT_SUCCESS;
}
