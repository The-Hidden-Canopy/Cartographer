#include <carto/production_authoring/authoring.hpp>

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace carto;
using namespace carto::production_authoring;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

biological::AuthoringSourceIdentity source(std::string_view suffix) {
    return biological::AuthoringSourceIdentity{
        "fixtures/production-authoring", 8001U, core::Revision{2U},
        digest(std::string("production-source-") + std::string(suffix))};
}

SculptStroke stroke(SculptStrokeId id) {
    return SculptStroke{
        .id = id,
        .brush = BrushKind::clay,
        .symmetry = SymmetryAxis::x,
        .falloff = 0.65,
        .mask_digest = digest("mask-v1"),
        .samples = {
            SculptSample{{0.0, 0.0, 0.0}, 0.2, 0.4, 101U, "surface"},
            SculptSample{{0.1, 0.0, 0.0}, 0.15, 0.25, 102U, "surface"},
        },
    };
}

SculptStack sculpt_fixture() {
    SculptStack stack;
    stack.source = source("sculpt");
    stack.mesh_asset = 71U;
    stack.base_mesh_digest = digest("mesh-base-v2");
    stack.default_symmetry = SymmetryAxis::x;
    require(static_cast<bool>(stack.insert_layer(SculptLayer{
                .id = SculptLayerId{1U},
                .kind = SculptLayerKind::form,
                .parent = std::nullopt,
                .order = 10U,
                .settings_digest = digest("form-settings"),
                .displacement_digest = digest("form-displacement"),
                .strokes = {stroke(SculptStrokeId{1U})},
            })),
        "form sculpt layer enters the stack");
    require(static_cast<bool>(stack.insert_layer(SculptLayer{
                .id = SculptLayerId{2U},
                .kind = SculptLayerKind::microdetail,
                .parent = SculptLayerId{1U},
                .order = 20U,
                .settings_digest = digest("microdetail-settings"),
                .displacement_digest = std::nullopt,
                .strokes = {stroke(SculptStrokeId{2U})},
            })),
        "microdetail sculpt layer enters the stack");
    require(static_cast<bool>(stack.validate()), "sculpt fixture validates");
    return stack;
}

GarmentAsset garment_fixture() {
    GarmentAsset garment;
    garment.source = source("garment");
    garment.semantic_name = "field-jacket";
    require(static_cast<bool>(garment.insert_fabric(FabricIntent{
                .id = FabricIntentId{1U},
                .family = "plain-weave",
                .weave_field_digest = digest("warp-weft-field"),
                .areal_density = 0.32,
                .thickness = 0.001,
                .stretch_stiffness = 12.0,
                .shear_stiffness = 8.0,
                .bend_stiffness = 2.0,
                .friction = 0.4,
                .damping = 0.1,
            })),
        "fabric intent enters the garment");
    require(static_cast<bool>(garment.insert_panel(GarmentPanel{
                .id = PanelId{1U},
                .semantic_type = "front-left",
                .outline_digest = digest("panel-outline"),
                .rest_shape_digest = digest("panel-rest-shape"),
                .fabric = FabricIntentId{1U},
                .boundary_edges = 4U,
            })),
        "garment panel enters the asset");
    require(static_cast<bool>(garment.insert_panel(GarmentPanel{
                .id = PanelId{2U},
                .semantic_type = "front-right",
                .outline_digest = digest("panel-outline-right"),
                .rest_shape_digest = digest("panel-rest-shape-right"),
                .fabric = FabricIntentId{1U},
                .boundary_edges = 4U,
            })),
        "second garment panel enters the asset");
    require(static_cast<bool>(garment.insert_seam(GarmentSeam{
                .id = SeamId{1U},
                .first_panel = PanelId{1U},
                .second_panel = PanelId{2U},
                .first_edge = 2U,
                .second_edge = 2U,
                .rest_length = 0.7,
                .construction_digest = digest("center-seam"),
            })),
        "garment seam enters the asset");
    require(static_cast<bool>(garment.insert_pin(GarmentPin{
                .id = PinId{1U},
                .panel = PanelId{1U},
                .body_source = source("body"),
                .body_structure = biological::StructureId{44U},
                .panel_vertex = 1U,
                .weight = 1.0,
                .attachment_digest = digest("shoulder-pin"),
            })),
        "garment pin enters the asset");
    require(static_cast<bool>(garment.insert_constraint(ClothConstraint{
                .id = ClothConstraintId{1U},
                .kind = ClothConstraintKind::stretch,
                .compliance = 0.001,
                .limit = 1.2,
                .thickness = 0.001,
            })),
        "stretch constraint enters the asset");
    require(static_cast<bool>(garment.insert_constraint(ClothConstraint{
                .id = ClothConstraintId{2U},
                .kind = ClothConstraintKind::body_collision,
                .compliance = 0.0,
                .limit = 0.02,
                .thickness = 0.001,
            })),
        "body collision constraint enters the asset");
    require(static_cast<bool>(garment.add_representation(ClothRepresentation{
                .kind = ClothRepresentationKind::baked,
                .product_digest = digest("baked-cloth"),
                .rest_pose_digest = digest("baked-rest"),
                .lineage_digest = digest("baked-lineage"),
                .resident_bytes = 4096U,
            })),
        "baked cloth representation enters the asset");
    require(static_cast<bool>(garment.validate()), "garment fixture validates");
    return garment;
}

void sculpt_round_trip_and_publication_gate() {
    const auto stack = sculpt_fixture();
    const auto first_digest = stack.canonical_digest();
    require(static_cast<bool>(first_digest), "sculpt stack digest computes");
    const auto reparsed = SculptStack::deserialize(stack.serialize());
    require(static_cast<bool>(reparsed), "sculpt stack round-trips");
    require(reparsed.value().canonical_digest().value() == first_digest.value(),
        "sculpt stack digest is stable across a round trip");

    SculptPublication publication{
        .source = stack.source,
        .mesh_asset = stack.mesh_asset,
        .stack_digest = first_digest.value(),
        .product_digest = digest("sculpt-product"),
        .topology_lineage_digest = digest("topology-lineage"),
        .target_profile = "prepared-render",
        .receipt_digest = {},
    };
    require(static_cast<bool>(publication.refresh_receipt_digest()),
        "sculpt publication receipt refreshes");
    require(static_cast<bool>(validate_sculpt_publication(stack, publication)),
        "current sculpt publication is admitted");
    auto stale_publication = publication;
    stale_publication.stack_digest = digest("stale-stack");
    require(static_cast<bool>(stale_publication.refresh_receipt_digest()),
        "stale sculpt publication receipt refreshes independently");
    require(!validate_sculpt_publication(stack, stale_publication),
        "stale sculpt publication is rejected");

    std::string future = stack.serialize();
    const std::string current = "CARTOGRAPHER_SCULPT_STACK 1";
    future.replace(0U, current.size(), "CARTOGRAPHER_SCULPT_STACK 99");
    require(!SculptStack::deserialize(future), "future sculpt schema is rejected");
    require(!SculptStack::deserialize(stack.serialize() + "junk"),
        "sculpt trailing input is rejected");

    // The public stack is intentionally immutable through its map accessors;
    // construct the cycle through a serialized hostile record instead.
    std::string cyclic_text = stack.serialize();
    const auto first_parent = cyclic_text.find("LAYER 1 form 0");
    const auto second_parent = cyclic_text.find("LAYER 2 microdetail 1");
    require(first_parent != std::string::npos && second_parent != std::string::npos,
        "sculpt fixture exposes parent tokens for failure injection");
    cyclic_text.replace(first_parent, std::string("LAYER 1 form 0").size(), "LAYER 1 form 2");
    cyclic_text.replace(second_parent, std::string("LAYER 2 microdetail 1").size(),
        "LAYER 2 microdetail 1");
    require(!SculptStack::deserialize(cyclic_text), "sculpt parent cycle is rejected");
}

void garment_round_trip_and_drape_gate() {
    const auto garment = garment_fixture();
    const auto garment_digest = garment.canonical_digest();
    require(static_cast<bool>(garment_digest), "garment digest computes");
    const auto reparsed = GarmentAsset::deserialize(garment.serialize());
    require(static_cast<bool>(reparsed), "garment asset round-trips");
    require(reparsed.value().canonical_digest().value() == garment_digest.value(),
        "garment digest is stable across a round trip");

    DrapeRequest request{
        .source = garment.source,
        .garment_digest = garment_digest.value(),
        .iterations = 32U,
        .time_step = 1.0 / 60.0,
        .convergence_tolerance = 1e-4,
        .stable_seed = 99U,
    };
    const auto request_digest = request.canonical_digest();
    require(static_cast<bool>(request_digest), "drape request digest computes");
    DrapeReceipt receipt{
        .source = garment.source,
        .request_digest = request_digest.value(),
        .output_digest = digest("drape-output"),
        .evidence_source = DrapeEvidenceSource::cpu_reference,
        .iterations = 32U,
        .maximum_strain = 8e-5,
        .maximum_shear = 4e-5,
        .maximum_bend = 2e-5,
        .converged = true,
    };
    require(static_cast<bool>(validate_drape_receipt(garment, request, receipt)),
        "current CPU drape receipt is admitted");

    auto over_tolerance = receipt;
    over_tolerance.maximum_strain = request.convergence_tolerance * 2.0;
    require(!validate_drape_receipt(garment, request, over_tolerance),
        "converged drape receipt over the requested tolerance is rejected");

    auto stale_request = request;
    stale_request.garment_digest = digest("changed-garment");
    auto stale_receipt = receipt;
    stale_receipt.request_digest = stale_request.canonical_digest().value();
    require(!validate_drape_receipt(garment, stale_request, stale_receipt),
        "drape receipt for a changed garment is rejected");

    require(garment.serialize().find("telemetry") == std::string::npos &&
                garment.serialize().find("runtime_handle") == std::string::npos &&
                garment.serialize().find("pose") == std::string::npos,
        "public garment serialization contains no runtime state");

    auto invalid = garment;
    auto broken = invalid.panels().at(PanelId{1U});
    broken.boundary_edges = 2U;
    require(!broken.validate(), "invalid panel boundary is rejected");
}

} // namespace

int main() {
    sculpt_round_trip_and_publication_gate();
    garment_round_trip_and_drape_gate();
    std::cout << "production authoring tests passed\n";
    return EXIT_SUCCESS;
}
