#include <carto/production/closure.hpp>
#include <carto/production/decision.hpp>
#include <carto/production/dependency.hpp>
#include <carto/production/artifact_store.hpp>
#include <carto/production/render_mesh.hpp>

#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/project/transaction.hpp>
#include <carto/world/model.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

using carto::assets::Sha256Digest;

Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::geometry::EditableMesh render_mesh_fixture(
    bool degenerate_uvs = false,
    bool with_uv_seam = true,
    bool out_of_range_color = false) {
    using namespace carto;
    geometry::EditableMesh mesh;
    const auto v0 = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto v1 = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto v2 = mesh.add_vertex({1.0, 1.0, 0.0});
    const auto v3 = mesh.add_vertex({0.0, 1.0, 0.0});
    require(v0 && v1 && v2 && v3, "render mesh fixture vertices are valid");
    const auto f0 = mesh.add_face({v0.value(), v1.value(), v2.value()});
    const auto f1 = mesh.add_face({v0.value(), v2.value(), v3.value()});
    require(f0 && f1, "render mesh fixture faces are valid");

    const auto topology = mesh.topology();
    const auto faces = mesh.faces_sorted();
    require(topology && topology.value().corners.size() == 6U && faces.size() == 2U,
            "render mesh fixture exposes stable face-corner ordering");

    const std::map<geometry::VertexId, core::Vec2d> authored_uvs{
        {v0.value(), {0.0, 0.0}},
        {v1.value(), {1.0, 0.0}},
        {v2.value(), {1.0, 1.0}},
        {v3.value(), {0.0, 1.0}},
    };
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, 6U},
        {attributes::AttributeDomain::face, 2U},
        {attributes::AttributeDomain::vertex, 4U},
    });
    std::vector<attributes::AttributeValue> uv_values;
    uv_values.reserve(topology.value().corners.size());
    for (const auto& corner : topology.value().corners) {
        core::Vec2d uv = degenerate_uvs ? core::Vec2d{} : authored_uvs.at(corner.vertex);
        if (!degenerate_uvs && with_uv_seam && corner.face == f1.value() &&
            corner.vertex == v0.value()) {
            uv.x += 2.0;
        }
        uv_values.emplace_back(uv);
    }
    require(static_cast<bool>(attributes.add_layer({
                {"uv.render", attributes::AttributeDomain::corner,
                 attributes::AttributeType::vec2},
                std::move(uv_values)})),
            "render mesh fixture UV layer is valid");
    std::vector<attributes::AttributeValue> uv1_values;
    uv1_values.reserve(topology.value().corners.size());
    for (const auto& corner : topology.value().corners) {
        const core::Vec2d source_uv = authored_uvs.at(corner.vertex);
        uv1_values.emplace_back(core::Vec2d{source_uv.x * 0.5, source_uv.y * 0.5});
    }
    require(static_cast<bool>(attributes.add_layer({
                {"uv.lightmap", attributes::AttributeDomain::corner,
                 attributes::AttributeType::vec2},
                std::move(uv1_values)})),
            "render mesh fixture UV1 layer is valid");
    std::vector<attributes::AttributeValue> vertex_colors{
        core::Vec4d{1.0, 0.0, 0.0, 1.0},
        core::Vec4d{0.0, 1.0, 0.0, 1.0},
        core::Vec4d{0.0, 0.0, 1.0, 1.0},
        core::Vec4d{1.0, 1.0, 1.0, out_of_range_color ? 1.5 : 1.0},
    };
    require(static_cast<bool>(attributes.add_layer({
                {"color.display", attributes::AttributeDomain::vertex,
                 attributes::AttributeType::color4},
                std::move(vertex_colors)})),
            "render mesh fixture vertex-color layer is valid");
    std::vector<attributes::AttributeValue> material_slots;
    material_slots.reserve(faces.size());
    for (const auto& face : faces) {
        material_slots.emplace_back(face.id == f0.value() ? 9U : 3U);
    }
    require(static_cast<bool>(attributes.add_layer({
                {"cartographer.material_slot", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                std::move(material_slots)})),
            "render mesh fixture material-slot layer is valid");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add({7U, "Render UV", "uv.render", false, true, false, 1U})),
            "render mesh fixture UV identity is valid");
    require(static_cast<bool>(uv_sets.add({
                8U, "Lightmap UV", "uv.lightmap", false, false, true, 1U})),
            "render mesh fixture UV1 identity is valid");
    require(static_cast<bool>(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets))),
            "render mesh fixture attributes bind to the mesh topology");
    return mesh;
}

carto::production::ProductionArtifactReceipt admitted_receipt() {
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key.source = {"scene/house", carto::core::Revision{7U}, digest("source-v7")};
    receipt.key.kind = carto::production::ArtifactKind::render_mesh;
    receipt.key.form = carto::production::ExecutionForm::cached;
    receipt.key.algorithm = "surface-closure-v1";
    receipt.key.settings_digest = digest("settings");
    receipt.key.platform_profile = "cpu-reference";
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = digest("render-mesh-output");
    receipt.output_bytes = 4096U;
    receipt.quality = {.measured_error = 0.02, .importance = 0.9, .confidence = 0.98};
    receipt.cost = {.cook_milliseconds = 4.5,
                    .cpu_milliseconds = 0.6,
                    .gpu_milliseconds = 0.0,
                    .resident_bytes = 8192U,
                    .disk_bytes = 4096U};
    require(static_cast<bool>(receipt.refresh_digest()), "valid receipt refreshes its digest");
    return receipt;
}

std::string legacy_v1_ledger(
    const carto::production::ProductionArtifactReceipt& receipt) {
    std::ostringstream output;
    output << "CARTOGRAPHER_PRODUCTION_LEDGER_V1\nrecords 1\n";
    output << std::setprecision(17);
    const auto& key = receipt.key;
    output << "record " << std::quoted(key.source.identity) << ' '
           << key.source.revision.value() << ' '
           << key.source.content_digest.hex() << ' '
           << carto::production::artifact_kind_name(key.kind) << ' '
           << carto::production::execution_form_name(key.form) << ' '
           << std::quoted(key.algorithm) << ' '
           << key.settings_digest.hex() << ' '
           << std::quoted(key.platform_profile) << ' '
           << carto::production::receipt_status_name(receipt.status) << ' '
           << receipt.output_digest.hex() << ' ' << receipt.output_bytes << ' '
           << receipt.quality.measured_error << ' '
           << receipt.quality.importance << ' '
           << receipt.quality.confidence << ' '
           << receipt.cost.cook_milliseconds << ' '
           << receipt.cost.cpu_milliseconds << ' '
           << receipt.cost.gpu_milliseconds << ' '
           << receipt.cost.resident_bytes << ' '
           << receipt.cost.disk_bytes << ' '
           << receipt.receipt_digest.hex() << '\n';
    return output.str();
}

void valid_receipt_round_trips_through_admission() {
    const auto receipt = admitted_receipt();
    require(static_cast<bool>(receipt.validate()), "valid receipt validates");
    const carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = std::nullopt,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = std::nullopt,
    };
    require(static_cast<bool>(carto::production::validate_publication(
        receipt, receipt.key.source, policy)), "valid receipt is admitted");
}

void stale_source_and_budget_fail_closed() {
    auto receipt = admitted_receipt();
    auto stale_source = receipt.key.source;
    stale_source.revision = carto::core::Revision{8U};
    const carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = std::nullopt,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = std::nullopt,
    };
    const auto stale_result = carto::production::validate_publication(
        receipt, stale_source, policy);
    require(!stale_result && stale_result.error().code == carto::core::ErrorCode::stale_data,
            "stale source is rejected before publication");

    auto bad_quality = receipt;
    bad_quality.quality.measured_error = 0.2;
    require(static_cast<bool>(bad_quality.refresh_digest()), "bad-quality receipt refreshes evidence");
    require(!carto::production::validate_publication(
        bad_quality, bad_quality.key.source, policy),
        "error budget rejects an admitted artifact");
}

void cost_budgets_fail_closed_and_allow_exact_limits() {
    const auto receipt = admitted_receipt();
    carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 8192U,
        .max_cook_milliseconds = 4.5,
        .max_cpu_milliseconds = 0.6,
        .max_gpu_milliseconds = 0.0,
        .max_disk_bytes = 4096U,
        .max_output_bytes = 4096U,
    };
    require(static_cast<bool>(carto::production::validate_publication(
                receipt, receipt.key.source, policy)),
            "cost budgets admit values exactly at their limits");

    policy.max_cook_milliseconds = 4.49;
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "cook time over budget is rejected");
    policy.max_cook_milliseconds = 4.5;

    policy.max_cpu_milliseconds = 0.59;
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "CPU time over budget is rejected");
    policy.max_cpu_milliseconds = 0.6;

    policy.max_gpu_milliseconds = 0.0;
    auto over_gpu = receipt;
    over_gpu.cost.gpu_milliseconds = 0.01;
    require(static_cast<bool>(over_gpu.refresh_digest()),
            "GPU-cost receipt refreshes its evidence digest");
    require(!carto::production::validate_publication(
                over_gpu, over_gpu.key.source, policy),
            "GPU time over budget is rejected");

    policy.max_disk_bytes = 4095U;
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "disk use over budget is rejected");
    policy.max_disk_bytes = 4096U;

    policy.max_resident_bytes = 8191U;
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "resident memory over budget is rejected");
    policy.max_resident_bytes = 8192U;

    policy.max_output_bytes = 4095U;
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "artifact output over budget is rejected");

    policy.max_cook_milliseconds = -1.0;
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "negative time budget is rejected as an invalid policy");

    policy.max_cook_milliseconds = 4.5;
    policy.max_cpu_milliseconds = std::numeric_limits<double>::quiet_NaN();
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "NaN time budget is rejected as an invalid policy");

    policy.max_cpu_milliseconds = 0.6;
    policy.max_gpu_milliseconds = std::numeric_limits<double>::infinity();
    require(!carto::production::validate_publication(
                receipt, receipt.key.source, policy),
            "infinite time budget is rejected as an invalid policy");
}

void failed_receipts_cannot_publish_output() {
    auto receipt = admitted_receipt();
    receipt.status = carto::production::ReceiptStatus::failed;
    require(!receipt.refresh_digest(), "failed receipt with output is rejected");

    receipt.output_digest = {};
    receipt.output_bytes = 0U;
    require(static_cast<bool>(receipt.refresh_digest()), "failed receipt records no output");
    require(static_cast<bool>(receipt.validate()), "failed receipt remains valid evidence");

    auto invalid_kind = admitted_receipt();
    invalid_kind.key.kind = static_cast<carto::production::ArtifactKind>(255U);
    require(!invalid_kind.refresh_digest(), "unknown artifact kinds fail closed");
    auto invalid_form = admitted_receipt();
    invalid_form.key.form = static_cast<carto::production::ExecutionForm>(255U);
    require(!invalid_form.refresh_digest(), "unknown execution forms fail closed");
    auto invalid_status = admitted_receipt();
    invalid_status.status = static_cast<carto::production::ReceiptStatus>(255U);
    require(!invalid_status.refresh_digest(), "unknown receipt states fail closed");
}

void ledger_never_falls_back_to_stale_evidence() {
    auto receipt = admitted_receipt();
    carto::production::ProductionReceiptLedger ledger;
    require(static_cast<bool>(ledger.append(receipt)), "admitted receipt enters ledger");
    require(static_cast<bool>(ledger.append(receipt)) && ledger.size() == 1U,
            "an exact receipt replay is idempotent and cannot exhaust ledger capacity");

    auto conflicting = receipt;
    conflicting.output_digest = digest("different-render-mesh-output");
    require(static_cast<bool>(conflicting.refresh_digest()),
            "conflicting deterministic output forms valid standalone evidence");
    require(!ledger.append(conflicting) && ledger.size() == 1U,
            "one cook identity cannot admit two different deterministic outputs");

    auto found = ledger.query(receipt.key);
    require(found && found.value().has_value(), "admitted receipt is queryable");

    auto stale = receipt;
    stale.status = carto::production::ReceiptStatus::stale;
    stale.output_digest = {};
    stale.output_bytes = 0U;
    require(static_cast<bool>(stale.refresh_digest()), "stale receipt refreshes evidence");
    require(static_cast<bool>(ledger.append(stale)), "stale receipt enters ledger");
    found = ledger.query(receipt.key);
    require(found && !found.value().has_value(), "stale evidence masks older admission");
    require(static_cast<bool>(ledger.validate()), "ledger validates after stale evidence");
}

void ledger_round_trips_and_rejects_malformed_input() {
    auto receipt = admitted_receipt();
    receipt.key.representation_kind = "consumer/beauty";
    receipt.key.spatial_scope = "tile/0/0";
    receipt.key.decision_context = {
        .truth_class = "derived",
        .observer_class = "consumer/beauty",
        .precision_requirement = "precision/standard",
        .materialization_requirement = "materialization/on-demand",
    };
    require(static_cast<bool>(receipt.refresh_digest()),
            "scoped receipt refreshes its evidence digest");
    carto::production::ProductionReceiptLedger ledger;
    require(static_cast<bool>(ledger.append(receipt)), "round-trip receipt enters ledger");
    const auto encoded = ledger.serialize();
    const auto restored = carto::production::ProductionReceiptLedger::deserialize(encoded);
    require(restored && restored.value().size() == 1U, "ledger round-trips one receipt");
    const auto found = restored.value().query(receipt.key);
    require(found && found.value().has_value(), "round-tripped receipt remains queryable");
    require(found.value()->receipt_digest == receipt.receipt_digest,
            "round-tripped receipt preserves evidence digest");
    require(found.value()->key.representation_kind == "consumer/beauty" &&
                found.value()->key.spatial_scope == "tile/0/0" &&
                found.value()->key.decision_context == receipt.key.decision_context,
            "versioned ledger preserves representation, scope, and decision context");

    const auto malformed = carto::production::ProductionReceiptLedger::deserialize(
        "CARTOGRAPHER_PRODUCTION_LEDGER_V1\nrecords 4097\n");
    require(!malformed, "oversized ledger is rejected before allocation");

    std::string oversized(carto::production::ProductionReceiptLedger::max_serialized_bytes + 1U, 'x');
    require(!carto::production::ProductionReceiptLedger::deserialize(oversized),
            "oversized serialized input is rejected before parsing");

    const auto trailing = carto::production::ProductionReceiptLedger::deserialize(
        encoded + "unexpected");
    require(!trailing, "trailing serialized data is rejected");

    std::string duplicate = encoded;
    const auto count = duplicate.find("records 1\n");
    const auto record = duplicate.find("record ");
    require(count != std::string::npos && record != std::string::npos,
            "duplicate-ledger fixture exposes count and record fields");
    duplicate.replace(count, std::string("records 1\n").size(), "records 2\n");
    duplicate.append(encoded.substr(record));
    require(!carto::production::ProductionReceiptLedger::deserialize(duplicate),
            "serialized duplicate receipts are rejected instead of silently collapsed");
}

void legacy_v1_ledger_remains_readable_and_upgrades() {
    const auto receipt = admitted_receipt();
    const auto legacy = legacy_v1_ledger(receipt);
    const auto restored = carto::production::ProductionReceiptLedger::deserialize(legacy);
    require(restored && restored.value().size() == 1U,
            "version-one production ledger remains readable");
    const auto found = restored.value().query(receipt.key);
    require(found && found.value().has_value() &&
                found.value()->receipt_digest == receipt.receipt_digest,
            "legacy receipt digest and key remain valid after reading");

    const auto upgraded_text = restored.value().serialize();
    require(upgraded_text.starts_with("CARTOGRAPHER_PRODUCTION_LEDGER_V2\n"),
            "legacy ledger serializes in the current version");
    const auto upgraded = carto::production::ProductionReceiptLedger::deserialize(
        upgraded_text);
    require(upgraded && upgraded.value().size() == 1U,
            "upgraded legacy ledger remains readable");
    const auto upgraded_record = upgraded.value().query(receipt.key);
    require(upgraded_record && upgraded_record.value().has_value() &&
                upgraded_record.value()->receipt_digest == receipt.receipt_digest,
            "upgrading an unscoped key preserves its receipt digest");
}

carto::production::ProductionCookKey cook_key(
    std::string identity,
    std::string token) {
    carto::production::ProductionCookKey key;
    key.source = {std::move(identity), carto::core::Revision{1U}, digest(token + "-source")};
    key.kind = carto::production::ArtifactKind::render_mesh;
    key.form = carto::production::ExecutionForm::cached;
    key.algorithm = "cook-v1";
    key.settings_digest = digest(token + "-settings");
    key.platform_profile = "cpu-reference";
    return key;
}

void cook_graph_is_deterministic_and_minimally_invalidating() {
    const auto source = cook_key("source", "source");
    const auto material = cook_key("material", "material");
    const auto mesh = cook_key("mesh", "mesh");
    const auto unrelated = cook_key("unrelated", "unrelated");
    const auto external = cook_key("external", "external");

    carto::production::ProductionCookGraph graph;
    require(static_cast<bool>(graph.add_node(source)), "source node enters cook graph");
    require(static_cast<bool>(graph.add_node(material, {source})),
            "material node enters cook graph");
    require(static_cast<bool>(graph.add_node(mesh, {material, external})),
            "mesh node enters cook graph with external input");
    require(static_cast<bool>(graph.add_node(unrelated)),
            "unrelated node enters cook graph");

    const auto order = graph.topological_order();
    require(order && order.value().size() == 4U, "cook graph has a full topological order");
    require(order.value()[0] == source && order.value()[1] == material &&
                order.value()[2] == mesh,
            "registered dependencies precede their derived products");

    const auto material_invalidation = graph.invalidation_order(material);
    require(material_invalidation && material_invalidation.value().size() == 2U,
            "material invalidation is bounded to dependent products");
    require(material_invalidation.value()[0] == material &&
                material_invalidation.value()[1] == mesh,
            "material invalidation is deterministic and downstream-only");

    const auto external_invalidation = graph.invalidation_order(external);
    require(external_invalidation && external_invalidation.value().size() == 1U &&
                external_invalidation.value()[0] == mesh,
            "external source invalidation reaches only direct dependents");

    carto::production::ProductionCookGraph cyclic;
    const auto left = cook_key("left", "left");
    const auto right = cook_key("right", "right");
    require(static_cast<bool>(cyclic.add_node(left, {right})),
            "incomplete graph may name an external dependency");
    require(!cyclic.add_node(right, {left}), "cycle is rejected when it becomes closed");
    require(cyclic.size() == 1U, "rejected cycle does not mutate the graph");

    carto::production::ProductionCookGraph invalid;
    require(!invalid.add_node(left, {left}), "self-dependency is rejected");
    require(!invalid.add_node(left, {right, right}), "duplicate dependency is rejected");
    require(invalid.size() == 0U, "rejected graph nodes do not mutate the graph");
}

void representation_and_spatial_scope_are_cook_identity() {
    const auto base = cook_key("shared-source", "shared-output");
    auto beauty = base;
    beauty.representation_kind = "consumer/beauty";
    auto shadow = base;
    shadow.representation_kind = "consumer/shadow";
    auto tile = beauty;
    tile.spatial_scope = "tile/12/4";
    auto predicted = base;
    predicted.decision_context.truth_class = "predicted";
    auto sensor = base;
    sensor.decision_context.observer_class = "consumer/sensor";
    auto high_precision = base;
    high_precision.decision_context.precision_requirement = "precision/high";
    auto deferred = base;
    deferred.decision_context.materialization_requirement = "materialization/deferred";

    require(static_cast<bool>(beauty.validate()) &&
                static_cast<bool>(shadow.validate()) &&
                static_cast<bool>(tile.validate()) &&
                static_cast<bool>(predicted.validate()) &&
                static_cast<bool>(sensor.validate()) &&
                static_cast<bool>(high_precision.validate()) &&
                static_cast<bool>(deferred.validate()),
            "scoped cook keys validate");
    require(base != beauty && beauty != shadow && beauty != tile,
            "representation and spatial scope distinguish cook identities");
    require(base != predicted && base != sensor && base != high_precision &&
                base != deferred,
            "truth, observer, precision, and materialization context distinguish cook keys");
    require(base.canonical() != beauty.canonical() &&
                beauty.canonical() != tile.canonical() &&
                base.canonical() != predicted.canonical(),
            "canonical cook identity binds representation, scope, and decision context");

    auto overlong = base;
    overlong.representation_kind = std::string(129U, 'r');
    require(!overlong.validate(), "overlong representation identity is rejected");
    auto unsafe_scope = base;
    unsafe_scope.spatial_scope = "tile/12\n4";
    require(!unsafe_scope.validate(), "unsafe spatial scope is rejected");
    auto overlong_context = base;
    overlong_context.decision_context.truth_class = std::string(129U, 't');
    require(!overlong_context.validate(), "overlong decision context identifier is rejected");
    auto unsafe_context = base;
    unsafe_context.decision_context.observer_class = "consumer/\nprivate";
    require(!unsafe_context.validate(), "unsafe decision context identifier is rejected");

    carto::production::ProductionCookGraph first;
    require(static_cast<bool>(first.add_node(base)) &&
                static_cast<bool>(first.add_node(beauty)) &&
                static_cast<bool>(first.add_node(shadow)) &&
                static_cast<bool>(first.add_node(tile)) &&
                static_cast<bool>(first.add_node(predicted)) &&
                static_cast<bool>(first.add_node(sensor)) &&
                static_cast<bool>(first.add_node(high_precision)) &&
                static_cast<bool>(first.add_node(deferred)),
            "distinct scoped products coexist in the cook graph");
    const auto first_order = first.topological_order();
    require(first_order && first_order.value().size() == 8U,
            "cook graph retains every scoped product");

    carto::production::ProductionCookGraph reordered;
    require(static_cast<bool>(reordered.add_node(deferred)) &&
                static_cast<bool>(reordered.add_node(high_precision)) &&
                static_cast<bool>(reordered.add_node(sensor)) &&
                static_cast<bool>(reordered.add_node(predicted)) &&
                static_cast<bool>(reordered.add_node(tile)) &&
                static_cast<bool>(reordered.add_node(shadow)) &&
                static_cast<bool>(reordered.add_node(beauty)) &&
                static_cast<bool>(reordered.add_node(base)),
            "scoped products can be registered in a different order");
    const auto reordered_order = reordered.topological_order();
    require(reordered_order && reordered_order.value() == first_order.value(),
            "scoped cook ordering stays deterministic across insertion order");
}

void spatial_cook_invalidation_is_sparse_and_fail_closed() {
    using namespace carto::production;
    const auto changed = cook_key("road-source", "road-source");

    const SpatialTileRange left_range{
        .grid_identity = "world-grid",
        .level = 4U,
        .minimum_x = 0,
        .minimum_y = 0,
        .minimum_z = 0,
        .maximum_x = 3,
        .maximum_y = 3,
        .maximum_z = 0,
    };
    const SpatialTileRange right_range{
        .grid_identity = "world-grid",
        .level = 4U,
        .minimum_x = 4,
        .minimum_y = 0,
        .minimum_z = 0,
        .maximum_x = 7,
        .maximum_y = 3,
        .maximum_z = 0,
    };
    const SpatialTileRange dirty{
        .grid_identity = "world-grid",
        .level = 4U,
        .minimum_x = 1,
        .minimum_y = 1,
        .minimum_z = 0,
        .maximum_x = 2,
        .maximum_y = 2,
        .maximum_z = 0,
    };
    require(left_range.intersects(dirty) &&
                left_range.intersects(dirty).value() &&
                right_range.intersects(dirty) &&
                !right_range.intersects(dirty).value(),
            "typed tile ranges distinguish overlapping and unrelated regions");

    auto left = cook_key("terrain-left", "terrain-left");
    left.spatial_scope = left_range.canonical();
    auto right = cook_key("terrain-right", "terrain-right");
    right.spatial_scope = right_range.canonical();
    auto vegetation = cook_key("vegetation-left", "vegetation-left");
    vegetation.spatial_scope = left_range.canonical();
    auto global_navigation = cook_key("navigation-global", "navigation-global");

    ProductionCookGraph graph;
    require(static_cast<bool>(graph.add_spatial_node(left, left_range, {changed})),
            "left spatial product enters the graph");
    require(static_cast<bool>(graph.add_spatial_node(right, right_range, {changed})),
            "right spatial product enters the graph");
    require(static_cast<bool>(graph.add_spatial_node(
                vegetation, left_range, {left})),
            "left downstream spatial product enters the graph");
    require(static_cast<bool>(graph.add_node(global_navigation, {left})),
            "global downstream product enters the graph");

    const auto sparse = graph.invalidation_order(changed, dirty);
    require(sparse && sparse.value().size() == 3U &&
                std::find(sparse.value().begin(), sparse.value().end(), left) !=
                    sparse.value().end() &&
                std::find(sparse.value().begin(), sparse.value().end(), vegetation) !=
                    sparse.value().end() &&
                std::find(sparse.value().begin(), sparse.value().end(), global_navigation) !=
                    sparse.value().end() &&
                std::find(sparse.value().begin(), sparse.value().end(), right) ==
                    sparse.value().end(),
            "dirty region invalidates only overlap plus downstream global products");

    auto mismatched_scope = left;
    mismatched_scope.spatial_scope = "unbound-scope";
    ProductionCookGraph rejected;
    require(!rejected.add_spatial_node(mismatched_scope, left_range, {changed}) &&
                rejected.size() == 0U,
            "spatial footprint must be part of the immutable cook identity");

    auto incompatible = dirty;
    incompatible.grid_identity = "another-grid";
    require(!graph.invalidation_order(changed, incompatible),
            "incompatible spatial frames fail closed instead of skipping work");

    auto invalid_range = dirty;
    invalid_range.minimum_x = invalid_range.maximum_x + 1;
    require(!graph.invalidation_order(changed, invalid_range),
            "invalid dirty bounds are rejected before traversal");

    auto unsafe_grid = dirty;
    unsafe_grid.grid_identity = "world\ngrid";
    require(!unsafe_grid.validate(), "unsafe grid identities are rejected");
}

void artifact_store_binds_bytes_to_admitted_receipts() {
    const auto root = std::filesystem::temp_directory_path() /
        "cartographer-production-closure-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    carto::assets::BlobStore blobs(root / "blobs");
    carto::production::ProductionArtifactStore store(blobs);

    const std::string payload_text = "derived-render-mesh";
    const auto payload = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(payload_text.data()), payload_text.size()};
    auto receipt = admitted_receipt();
    receipt.output_digest = digest(payload_text);
    receipt.output_bytes = payload.size();
    require(static_cast<bool>(receipt.refresh_digest()),
            "artifact receipt refreshes after binding its payload");
    const carto::production::AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = std::nullopt,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = std::nullopt,
    };
    require(static_cast<bool>(store.publish(receipt, payload, receipt.key.source, policy)),
            "matching derived bytes publish with their receipt");
    const auto restored = store.read(receipt.key, receipt.key.source, policy);
    require(restored && std::string(reinterpret_cast<const char*>(restored.value().data()),
                                    restored.value().size()) == payload_text,
            "admitted derived bytes reopen through the receipt key");

    const std::string conflicting_payload_text = "different-derived-render-mesh";
    const auto conflicting_payload = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(conflicting_payload_text.data()),
        conflicting_payload_text.size()};
    auto conflicting_receipt = receipt;
    conflicting_receipt.output_digest = digest(conflicting_payload_text);
    conflicting_receipt.output_bytes = conflicting_payload.size();
    require(static_cast<bool>(conflicting_receipt.refresh_digest()),
            "conflicting artifact receipt refreshes its standalone evidence");
    require(!store.publish(
                conflicting_receipt, conflicting_payload,
                conflicting_receipt.key.source, policy),
            "artifact store rejects conflicting output for one deterministic cook key");
    require(store.ledger().size() == 1U &&
                !std::filesystem::is_regular_file(
                    blobs.path_for(conflicting_receipt.output_digest)),
            "conflicting output is rejected before blob or receipt mutation");

    const auto wrong_payload = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>("tampered"), 8U};
    require(!store.publish(receipt, wrong_payload, receipt.key.source, policy),
            "payload digest mismatch is rejected before publication");
    require(store.ledger().size() == 1U,
            "rejected payload does not append evidence");

    const carto::production::AdmissionPolicy over_budget{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = 0.0,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = std::nullopt,
    };
    auto expensive = receipt;
    expensive.cost.gpu_milliseconds = 1.0;
    require(static_cast<bool>(expensive.refresh_digest()),
            "over-budget receipt refreshes its evidence digest");
    require(!store.publish(expensive, payload, expensive.key.source, over_budget),
            "artifact store rejects an over-budget production receipt");
    require(store.ledger().size() == 1U,
            "over-budget publication does not append a receipt");

    const carto::production::AdmissionPolicy low_output_budget{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = std::nullopt,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = static_cast<std::uint64_t>(payload.size() - 1U),
    };
    require(!store.publish(receipt, payload, receipt.key.source, low_output_budget),
            "artifact store rejects output above the per-publication size budget");
    require(store.ledger().size() == 1U,
            "over-sized output does not append a receipt");

    auto stale_source = receipt.key.source;
    stale_source.revision = carto::core::Revision{2U};
    const auto stale_read = store.read(receipt.key, stale_source, policy);
    require(!stale_read && stale_read.error().code == carto::core::ErrorCode::stale_data,
            "stale source cannot read an admitted derived artifact");

    std::filesystem::remove_all(root, cleanup_error);
}

void render_mesh_artifact_is_deterministic_and_fail_closed() {
    using namespace carto;
    using namespace production;
    const AuthoritativeSource source{
        "project/fixture",
        core::Revision{17U},
        digest("serialized-cartographer-project-v17"),
    };
    const geometry::EditableMesh mesh = render_mesh_fixture();
    const RenderMeshCompileOptions options{
        .uv_set = 7U,
        .uv1_set = 8U,
        .generate_tangents = true,
        .material_slot_layer = "cartographer.material_slot",
        .vertex_color_layer = "color.display",
    };
    const auto compiled = compile_render_mesh_artifact(source, mesh, options);
    require(static_cast<bool>(compiled), "authored mesh compiles to a render artifact");
    const RenderMeshArtifact& artifact = compiled.value();
    require(static_cast<bool>(artifact.validate()), "render artifact validates");
    const auto artifact_bounds = artifact.bounds();
    const auto cluster_bounds = artifact.submesh_bounds();
    require(artifact_bounds && cluster_bounds && cluster_bounds.value().size() == 2U,
            "render artifact derives global and material-cluster bounds");
    require(artifact_bounds.value().aabb.minimum.x == 0.0 &&
                artifact_bounds.value().aabb.minimum.y == 0.0 &&
                artifact_bounds.value().aabb.maximum.x == 1.0 &&
                artifact_bounds.value().aabb.maximum.y == 1.0 &&
                artifact_bounds.value().sphere_center.x == 0.5 &&
                artifact_bounds.value().sphere_center.y == 0.5 &&
                std::abs(artifact_bounds.value().sphere_radius - std::sqrt(0.5)) < 1e-12,
            "render artifact bounds are deterministic in authored coordinates");
    require(std::all_of(cluster_bounds.value().begin(), cluster_bounds.value().end(),
                [](const RenderMeshBounds& bounds) {
                    return static_cast<bool>(bounds.validate()) && bounds.sphere_radius > 0.0;
                }),
            "each material submesh has a finite non-empty cluster bound");
    auto malformed_bounds = artifact_bounds.value();
    malformed_bounds.sphere_center.x = malformed_bounds.aabb.maximum.x + 1.0;
    require(!malformed_bounds.validate(),
            "render-mesh bounds reject a sphere center outside the AABB");
    require(artifact.source_revision == source.revision &&
                artifact.mesh_revision == mesh.revision() &&
                artifact.source_digest == source.content_digest,
            "artifact binds enclosing source and exact mesh revisions");
    require(artifact.vertices.size() == 5U && artifact.indices.size() == 6U &&
                artifact.triangle_faces.size() == 2U,
            "artifact splits the authored UV seam and shares seam-compatible vertices");
    require(artifact.submeshes.size() == 2U &&
                artifact.submeshes[0].material_slot == 3U &&
                artifact.submeshes[1].material_slot == 9U &&
                artifact.submeshes[0].first_index == 0U &&
                artifact.submeshes[0].index_count == 3U &&
                artifact.submeshes[1].first_index == 3U &&
                artifact.submeshes[1].index_count == 3U,
            "material slots become deterministic sorted contiguous submeshes");
    require(artifact.triangle_faces[0].value == 2U &&
                artifact.triangle_faces[1].value == 1U,
            "triangle provenance follows deterministic material grouping");
    require(std::all_of(artifact.vertices.begin(), artifact.vertices.end(), [](const auto& vertex) {
                return vertex.uv.has_value() && vertex.uv1.has_value() &&
                    vertex.color.has_value() && vertex.tangent.has_value();
            }),
            "explicit UV0, UV1, color, and tangent requests produce complete vertex attributes");
    require(std::count_if(artifact.vertices.begin(), artifact.vertices.end(), [&](const auto& vertex) {
                return vertex.source_vertex == geometry::VertexId{1U};
            }) == 2,
            "one stable source vertex is duplicated at the authored UV seam");

    const auto upload = pack_pbr_mesh_upload(artifact);
    require(upload && static_cast<bool>(upload.value().validate()),
            "compiled render artifact packs into the native PBR vertex ABI");
    require(upload.value().source_revision == artifact.source_revision &&
                upload.value().mesh_revision == artifact.mesh_revision &&
                upload.value().source_digest == artifact.source_digest &&
                upload.value().indices == artifact.indices &&
                upload.value().submeshes == artifact.submeshes,
            "native PBR upload preserves source identity, topology, and material ranges");
    require(upload.value().resident_bytes() ==
                upload.value().vertices.size() * sizeof(PbrUploadVertex) +
                    upload.value().indices.size() * sizeof(std::uint32_t),
            "native PBR upload reports exact vertex and index residency bytes");
    require(std::all_of(upload.value().vertices.begin(), upload.value().vertices.end(),
                [](const PbrUploadVertex& vertex) {
                    return vertex.position[3U] == 1.0F &&
                        (vertex.tangent[3U] == -1.0F || vertex.tangent[3U] == 1.0F);
                }),
            "native PBR upload emits positional W and tangent handedness explicitly");

    auto invalid_upload = upload.value();
    invalid_upload.indices.front() = static_cast<std::uint32_t>(invalid_upload.vertices.size());
    require(!invalid_upload.validate(), "native PBR upload rejects out-of-range indices");

    auto float_overflow = artifact;
    const auto overflow_vertex = std::find_if(
        float_overflow.vertices.begin(), float_overflow.vertices.end(),
        [&](const auto& candidate) {
            return std::count_if(
                float_overflow.vertices.begin(), float_overflow.vertices.end(),
                [&](const auto& other) {
                    return other.source_vertex == candidate.source_vertex;
                }) == 1;
        });
    require(overflow_vertex != float_overflow.vertices.end(),
            "float-overflow fixture has a non-seam source vertex");
    overflow_vertex->position.z = 1e40;
    require(static_cast<bool>(float_overflow.validate()) &&
                !pack_pbr_mesh_upload(float_overflow),
            "native PBR upload rejects finite doubles outside the float32 range");

    auto inconsistent_seam = artifact;
    const auto seam_vertex = std::find_if(
        inconsistent_seam.vertices.begin(), inconsistent_seam.vertices.end(),
        [&](const auto& vertex) { return vertex.source_vertex == geometry::VertexId{1U}; });
    require(seam_vertex != inconsistent_seam.vertices.end(),
            "seam fixture exposes its source vertex");
    seam_vertex->position.x += 0.25;
    require(!inconsistent_seam.validate(),
            "UV duplicates cannot claim conflicting source geometry");
    auto degenerate_geometry = artifact;
    const std::uint32_t collapsed_vertex = degenerate_geometry.indices[2U];
    const std::uint32_t other_vertex = degenerate_geometry.indices[1U];
    degenerate_geometry.vertices[collapsed_vertex].position =
        degenerate_geometry.vertices[other_vertex].position;
    require(!degenerate_geometry.validate(),
            "artifact validation rejects triangles collapsed after compilation");

    const auto encoded = artifact.serialize();
    require(static_cast<bool>(encoded), "valid render artifact serializes");
    const auto encoded_again = artifact.serialize();
    require(encoded_again && encoded_again.value() == encoded.value(),
            "artifact serialization is byte-for-byte deterministic");
    const auto decoded = RenderMeshArtifact::deserialize(encoded.value());
    require(static_cast<bool>(decoded), "valid render artifact deserializes");
    const auto reencoded = decoded.value().serialize();
    require(reencoded && reencoded.value() == encoded.value(),
            "artifact deserialization preserves its canonical bytes");

    auto trailing = encoded.value();
    trailing.push_back(0U);
    require(!RenderMeshArtifact::deserialize(trailing),
            "artifact decoder rejects trailing bytes");
    auto unsupported_flags = encoded.value();
    unsupported_flags[16U] |= 0x80U;
    require(!RenderMeshArtifact::deserialize(unsupported_flags),
            "artifact decoder rejects unknown schema flags");
    auto hostile_counts = encoded.value();
    std::fill(hostile_counts.begin() + 68, hostile_counts.begin() + 72, std::uint8_t{0U});
    require(!RenderMeshArtifact::deserialize(hostile_counts),
            "artifact decoder rejects hostile vertex counts before allocation");
    auto zero_digest = encoded.value();
    std::fill(zero_digest.begin() + 36, zero_digest.begin() + 68, std::uint8_t{0U});
    require(!RenderMeshArtifact::deserialize(zero_digest),
            "artifact decoder rejects missing source provenance");
    auto non_finite_position = encoded.value();
    constexpr std::array<std::uint8_t, 8U> quiet_nan{
        0U, 0U, 0U, 0U, 0U, 0U, 0xf8U, 0x7fU};
    std::copy(quiet_nan.begin(), quiet_nan.end(), non_finite_position.begin() + 88);
    require(!RenderMeshArtifact::deserialize(non_finite_position),
            "artifact decoder rejects non-finite vertex data");
    auto out_of_range_index = encoded.value();
    const std::size_t first_index_byte = 80U + artifact.vertices.size() * 152U;
    std::fill(out_of_range_index.begin() + static_cast<std::ptrdiff_t>(first_index_byte),
              out_of_range_index.begin() + static_cast<std::ptrdiff_t>(first_index_byte + 4U),
              std::uint8_t{0xffU});
    require(!RenderMeshArtifact::deserialize(out_of_range_index),
            "artifact decoder rejects out-of-range geometry indices");

    const auto no_uv = compile_render_mesh_artifact(source, render_mesh_fixture(false, false));
    require(no_uv && std::all_of(no_uv.value().vertices.begin(), no_uv.value().vertices.end(),
                [](const auto& vertex) { return !vertex.uv && !vertex.tangent; }) &&
                no_uv.value().submeshes.size() == 1U &&
                !no_uv.value().submeshes.front().material_slot,
            "omitted UV/material data remains explicitly absent, not fabricated");
    require(!pack_pbr_mesh_upload(no_uv.value()),
            "native PBR upload does not fabricate missing UV or tangent data");

    const auto legacy_artifact = compile_render_mesh_artifact(
        source,
        mesh,
        RenderMeshCompileOptions{
            .uv_set = 7U,
            .uv1_set = std::nullopt,
            .generate_tangents = true,
            .material_slot_layer = "cartographer.material_slot",
            .vertex_color_layer = std::nullopt,
        });
    require(static_cast<bool>(legacy_artifact),
            "legacy-compatible render-mesh fixture compiles");
    auto legacy_bytes = legacy_artifact.value().serialize();
    require(static_cast<bool>(legacy_bytes), "legacy-compatible fixture serializes");
    legacy_bytes.value()[12U] = 1U;
    legacy_bytes.value()[13U] = 0U;
    legacy_bytes.value()[14U] = 0U;
    legacy_bytes.value()[15U] = 0U;
    const auto legacy_decoded = RenderMeshArtifact::deserialize(legacy_bytes.value());
    require(legacy_decoded &&
                std::all_of(
                    legacy_decoded.value().vertices.begin(),
                    legacy_decoded.value().vertices.end(),
                    [](const auto& vertex) {
                        return vertex.uv.has_value() && !vertex.uv1.has_value() &&
                            !vertex.color.has_value() && vertex.tangent.has_value();
                    }),
            "V2 decoder retains bounded V1 render-mesh readability");

    auto missing_uv = options;
    missing_uv.uv_set = 99U;
    require(!compile_render_mesh_artifact(source, mesh, missing_uv),
            "missing requested UV set is rejected");
    auto missing_uv1 = options;
    missing_uv1.uv1_set = 99U;
    require(!compile_render_mesh_artifact(source, mesh, missing_uv1),
            "missing requested UV1 set is rejected");
    auto aliased_uv1 = options;
    aliased_uv1.uv1_set = aliased_uv1.uv_set;
    require(!compile_render_mesh_artifact(source, mesh, aliased_uv1),
            "UV1 cannot silently alias the authored UV0 identity");
    auto invalid_layer = options;
    invalid_layer.material_slot_layer = "uv.render";
    require(!compile_render_mesh_artifact(source, mesh, invalid_layer),
            "wrong-domain material slot layer is rejected");
    auto invalid_color_layer = options;
    invalid_color_layer.vertex_color_layer = "cartographer.material_slot";
    require(!compile_render_mesh_artifact(source, mesh, invalid_color_layer),
            "wrong-domain vertex-color layer is rejected");
    require(!compile_render_mesh_artifact(
                source, render_mesh_fixture(false, true, true), options),
            "out-of-range authored vertex colors are rejected");
    auto tangent_without_uv = RenderMeshCompileOptions{};
    tangent_without_uv.generate_tangents = true;
    require(!compile_render_mesh_artifact(source, mesh, tangent_without_uv),
            "tangent request without UV identity is rejected");
    require(!compile_render_mesh_artifact(
                source, render_mesh_fixture(true), options),
            "degenerate authored UV triangles do not receive fabricated tangents");
    auto invalid_source = source;
    invalid_source.content_digest = {};
    require(!compile_render_mesh_artifact(invalid_source, mesh, options),
            "render artifact rejects a source without a content digest");
}

void compiled_render_mesh_publishes_through_receipt_store() {
    using namespace carto;
    using namespace production;
    const auto root = std::filesystem::temp_directory_path() /
        "cartographer-production-render-mesh-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    project::ProjectDocument document;
    require(static_cast<bool>(document.validate()), "empty source document starts valid");
    journal::Journal journal(root / "authoring.journal");
    auto transaction = project::ProjectTransaction::begin(document, journal, "render-fixture");
    require(static_cast<bool>(transaction), "saved-source fixture transaction begins");
    auto mesh_asset = transaction.value().add_mesh(render_mesh_fixture());
    require(static_cast<bool>(mesh_asset), "saved-source fixture mesh enters the project");
    world::WorldModel world_model;
    require(static_cast<bool>(world_model.insert_area(world::SemanticArea{
                .id = world::AreaId{1U},
                .semantic_type = "production-surface",
                .geometry = {
                    world::AuthoringBindingKind::mesh_asset, mesh_asset.value()},
                .parent = std::nullopt,
                .tags = {"public", "renderable"},
            })),
            "saved-source fixture has provider-neutral semantic authoring");
    require(static_cast<bool>(transaction.value().set_world_model(std::move(world_model))),
            "semantic authoring joins the same project transaction as geometry");
    require(static_cast<bool>(transaction.value().commit("render_mesh.fixture")),
            "fixture source commits through the project journal");
    const auto project_path = root / "source.carto";
    require(static_cast<bool>(document.save_atomic(project_path)),
            "fixture project saves atomically before production");
    auto loaded = project::ProjectDocument::load(project_path);
    require(static_cast<bool>(loaded), "saved fixture project reopens");
    require(loaded.value().world_model().areas().size() == 1U,
            "saved production source reopens its semantic world identity");
    const std::string canonical_source = loaded.value().serialize();
    const AuthoritativeSource source{
        "project/public-fixture",
        loaded.value().revision(),
        digest(canonical_source),
    };
    const RenderMeshCompileOptions options{
        .uv_set = 7U,
        .uv1_set = 8U,
        .generate_tangents = true,
        .material_slot_layer = "cartographer.material_slot",
        .vertex_color_layer = "color.display",
    };
    const auto artifact = compile_render_mesh_artifact(
        "public-fixture", loaded.value(), mesh_asset.value(), options);
    require(static_cast<bool>(artifact), "receipt fixture compiles its source mesh");
    require(artifact.value().source_revision == source.revision &&
                artifact.value().source_digest == source.content_digest,
            "project overload binds canonical saved content and project revision");
    require(!compile_render_mesh_artifact("../private/path", loaded.value(), mesh_asset.value(), options),
            "saved-project production identity rejects filesystem paths");
    require(!compile_render_mesh_artifact("public-fixture", loaded.value(), 0U, options) &&
                !compile_render_mesh_artifact("public-fixture", loaded.value(), 999U, options),
            "saved-project production rejects missing mesh identities");
    const auto payload = artifact.value().serialize();
    require(static_cast<bool>(payload), "receipt fixture serializes its derived mesh");

    assets::BlobStore blobs(root / "blobs");
    ProductionArtifactStore store(blobs);
    auto receipt = admitted_receipt();
    receipt.key.source = source;
    receipt.key.algorithm = "cartographer-render-mesh-v1";
    receipt.key.settings_digest = digest("uv=7;tangents=1;material=cartographer.material_slot");
    receipt.key.representation_kind = "render-mesh/native-pbr";
    receipt.key.spatial_scope = "project/public-fixture/mesh/1";
    receipt.key.decision_context = {
        .truth_class = "derived",
        .observer_class = "public-native-consumer",
        .precision_requirement = "authoring-f64/upload-f32",
        .materialization_requirement = "resident-on-publication",
    };
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = payload.value().size();
    require(static_cast<bool>(receipt.refresh_digest()),
            "render artifact receipt binds the encoded product");
    const AdmissionPolicy policy{
        .max_measured_error = 0.05,
        .min_confidence = 0.9,
        .max_resident_bytes = 16'384U,
        .max_cook_milliseconds = std::nullopt,
        .max_cpu_milliseconds = std::nullopt,
        .max_gpu_milliseconds = std::nullopt,
        .max_disk_bytes = std::nullopt,
        .max_output_bytes = std::nullopt,
    };
    const auto policy_digest = policy.canonical_digest();
    require(static_cast<bool>(policy_digest),
            "render artifact admission policy has a canonical digest");
    ProductionDecisionReceipt decision;
    decision.basis = {
        .source = source,
        .context = receipt.key.decision_context,
        .policy_identity = "public-render-threshold-v1",
        .admission_policy_digest = policy_digest.value(),
        .hardware_profile = receipt.key.platform_profile,
        .candidate_limit = 1U,
    };
    decision.status = ReceiptStatus::admitted;
    decision.selected_index = 0U;
    decision.candidates.push_back({
        .artifact = receipt,
        .disposition = CandidateDisposition::selected,
        .cost_evidence = {
            .cook = TimingEvidenceSource::host_clock,
            .cpu = TimingEvidenceSource::host_clock,
            .gpu = TimingEvidenceSource::none,
        },
        .reason_code = {},
    });
    require(static_cast<bool>(decision.refresh_digest()) &&
                static_cast<bool>(validate_decision_publication(
                    decision, source, policy)),
            "saved source reaches publication through an exact decision receipt");
    const auto serialized_decision = serialize_production_decision(decision);
    require(static_cast<bool>(serialized_decision) &&
                static_cast<bool>(deserialize_production_decision(
                    serialized_decision.value())),
            "saved-source production decision survives deterministic replay");
    require(static_cast<bool>(store.publish(receipt, payload.value(), source, policy)),
            "admitted render artifact publishes through content-addressed storage");
    const auto restored = store.read(receipt.key, source, policy);
    require(static_cast<bool>(restored), "current source reopens its admitted render artifact");
    const auto decoded = RenderMeshArtifact::deserialize(restored.value());
    require(decoded && decoded.value().source_digest == source.content_digest,
            "receipt-backed bytes reopen as the same source-bound render product");

    auto edited_mesh = loaded.value().meshes().at(mesh_asset.value());
    require(static_cast<bool>(edited_mesh.set_vertex_position(
                geometry::VertexId{1U}, {0.25, 0.0, 0.0})),
            "source edit changes authoritative mesh geometry");
    auto edit_transaction = project::ProjectTransaction::begin(
        loaded.value(), journal, "render-fixture-edit");
    require(static_cast<bool>(edit_transaction), "source-edit transaction begins");
    require(static_cast<bool>(edit_transaction.value().replace_mesh(
                mesh_asset.value(), std::move(edited_mesh))),
            "source edit stages through the project transaction boundary");
    require(static_cast<bool>(edit_transaction.value().commit("render_mesh.source_edit")),
            "source edit commits through the journal");
    require(static_cast<bool>(loaded.value().save_atomic(project_path)),
            "edited source saves atomically");
    const std::string edited_source_text = loaded.value().serialize();
    const AuthoritativeSource edited_source{
        "project/public-fixture",
        loaded.value().revision(),
        digest(edited_source_text),
    };
    const auto edited_artifact = compile_render_mesh_artifact(
        "public-fixture", loaded.value(), mesh_asset.value(), options);
    require(edited_artifact && edited_artifact.value().source_digest != source.content_digest,
            "real project edit changes the render artifact source identity");
    const auto stale_read = store.read(receipt.key, edited_source, policy);
    require(!stale_read && stale_read.error().code == core::ErrorCode::stale_data,
            "saved source edit invalidates the previously published render artifact");
    const auto stale_decision = validate_decision_publication(
        decision, edited_source, policy);
    require(!stale_decision &&
                stale_decision.error().code == core::ErrorCode::stale_data,
            "saved source edit invalidates the prior production decision");
    std::filesystem::remove_all(root, cleanup_error);
}

} // namespace

int main() {
    valid_receipt_round_trips_through_admission();
    stale_source_and_budget_fail_closed();
    cost_budgets_fail_closed_and_allow_exact_limits();
    failed_receipts_cannot_publish_output();
    ledger_never_falls_back_to_stale_evidence();
    ledger_round_trips_and_rejects_malformed_input();
    legacy_v1_ledger_remains_readable_and_upgrades();
    cook_graph_is_deterministic_and_minimally_invalidating();
    representation_and_spatial_scope_are_cook_identity();
    spatial_cook_invalidation_is_sparse_and_fail_closed();
    artifact_store_binds_bytes_to_admitted_receipts();
    render_mesh_artifact_is_deterministic_and_fail_closed();
    compiled_render_mesh_publishes_through_receipt_store();
    std::cout << "production closure tests passed\n";
}
