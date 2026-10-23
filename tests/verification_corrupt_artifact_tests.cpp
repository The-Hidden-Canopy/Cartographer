#include <carto/verification/failure_injection.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/attributes/layer.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
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
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::assets::Sha256Digest;
using carto::verification::ManifestTamperKind;
using carto::verification::PublishedCorruptionKind;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

Sha256Digest digest(std::string_view value) {
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
            ("cartographer-verification-corrupt-tests-" + std::to_string(stamp) +
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

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "selector file opens for reading");
    return std::string(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

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
                .provenance = "verification-corrupt-fixture",
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
        fixture.document, journal, "verification-corrupt-test");
    require(static_cast<bool>(transaction), "corrupt fixture transaction begins");
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
    require(static_cast<bool>(transaction.value().commit("corrupt.fixture")),
        "corrupt fixture commits through one journal event");

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

std::string render_product_path(const carto::production::PreparedSceneEnvelope& envelope) {
    for (const auto& product : envelope.products) {
        if (product.kind == carto::production::PreparedProductKind::render_mesh) {
            return product.relative_path;
        }
    }
    require(false, "fixture envelope declares a render mesh product");
    return {};
}

struct ProductCase {
    const char* name;
    PublishedCorruptionKind kind;
    std::uint64_t offset;
    std::uint64_t length;
};

struct ManifestCase {
    const char* name;
    ManifestTamperKind kind;
};

void corrupt_product_corpus_fails_closed_reading() {
    using namespace carto;
    const std::vector<ProductCase> cases{
        {"flip one product byte", PublishedCorruptionKind::flip_byte, 0U, 0U},
        {"zero a product range", PublishedCorruptionKind::zero_range, 0U, 8U},
        {"truncate a product file", PublishedCorruptionKind::truncate_file, 4U, 0U},
        {"append garbage to a product", PublishedCorruptionKind::append_garbage, 0U, 0U},
    };
    for (const auto& corruption : cases) {
        TempDirectory temp;
        journal::Journal journal(temp.path() / "corrupt-journal.log");
        const Fixture fixture = make_fixture(journal);
        const auto prepared = compose(fixture);
        const auto prepared_digest = prepared.canonical_digest();
        require(static_cast<bool>(prepared_digest), "corpus digest computes");
        const auto root = temp.path() / "published";
        require(static_cast<bool>(production::publish_prepared_scene(
                    root, prepared, make_payloads(fixture, prepared), std::nullopt)),
            "corpus revision publishes");
        const auto selector_before = read_text_file(root / "current");

        const auto corrupted = carto::verification::corrupt_published_product(
            root, prepared_digest.value(), render_product_path(prepared),
            corruption.kind, corruption.offset, corruption.length);
        require(static_cast<bool>(corrupted), corruption.name);

        require(!production::read_selected_prepared_scene(root),
            "corrupted published payloads fail closed on read");
        require(read_text_file(root / "current") == selector_before,
            "post-publication corruption leaves the current selector untouched");
    }
}

void manifest_tamper_corpus_fails_closed_reading() {
    using namespace carto;
    const std::vector<ManifestCase> cases{
        {"flip one manifest byte", ManifestTamperKind::flip_byte},
        {"append garbage to the manifest", ManifestTamperKind::append_garbage},
        {"replace one manifest digest digit", ManifestTamperKind::replace_digest_text},
    };
    for (const auto& tamper : cases) {
        TempDirectory temp;
        journal::Journal journal(temp.path() / "tamper-journal.log");
        const Fixture fixture = make_fixture(journal);
        const auto prepared = compose(fixture);
        const auto prepared_digest = prepared.canonical_digest();
        require(static_cast<bool>(prepared_digest), "tamper corpus digest computes");
        const auto root = temp.path() / "published";
        require(static_cast<bool>(production::publish_prepared_scene(
                    root, prepared, make_payloads(fixture, prepared), std::nullopt)),
            "tamper corpus revision publishes");
        const auto selector_before = read_text_file(root / "current");

        const auto tampered = carto::verification::tamper_published_manifest(
            root, prepared_digest.value(), tamper.kind);
        require(static_cast<bool>(tampered), tamper.name);

        require(!production::read_selected_prepared_scene(root),
            "tampered manifests fail closed on read");
        require(read_text_file(root / "current") == selector_before,
            "manifest tampering leaves the current selector untouched");
    }
}

void corruption_helpers_reject_unsafe_targets() {
    using namespace carto;
    TempDirectory temp;
    journal::Journal journal(temp.path() / "guard-journal.log");
    const Fixture fixture = make_fixture(journal);
    const auto prepared = compose(fixture);
    const auto prepared_digest = prepared.canonical_digest();
    require(static_cast<bool>(prepared_digest), "guard digest computes");
    const auto root = temp.path() / "published";
    require(static_cast<bool>(production::publish_prepared_scene(
                root, prepared, make_payloads(fixture, prepared), std::nullopt)),
        "guard revision publishes");

    const auto traversal = carto::verification::corrupt_published_product(
        root, prepared_digest.value(), "../outside.bin",
        PublishedCorruptionKind::flip_byte, 0U, 0U);
    require(!traversal &&
            traversal.error().code == carto::core::ErrorCode::invalid_argument,
        "relative path traversal is rejected before any write");

    const auto missing_revision = carto::verification::corrupt_published_product(
        root, digest("no-such-revision"), render_product_path(prepared),
        PublishedCorruptionKind::flip_byte, 0U, 0U);
    require(!missing_revision &&
            missing_revision.error().code == carto::core::ErrorCode::io_error,
        "corrupting an unknown immutable revision fails closed");

    const auto missing_file = carto::verification::corrupt_published_product(
        root, prepared_digest.value(), "products/undeclared.bin",
        PublishedCorruptionKind::flip_byte, 0U, 0U);
    require(!missing_file,
        "corrupting a file outside the declared manifest fails closed");

    const auto out_of_range = carto::verification::corrupt_published_product(
        root, prepared_digest.value(), render_product_path(prepared),
        PublishedCorruptionKind::flip_byte, 1U << 20U, 0U);
    require(!out_of_range &&
            out_of_range.error().code == carto::core::ErrorCode::invalid_argument,
        "out-of-range corruption offsets fail closed");

    const auto zero_digest = carto::verification::tamper_published_manifest(
        root, Sha256Digest{}, ManifestTamperKind::flip_byte);
    require(!zero_digest &&
            zero_digest.error().code == carto::core::ErrorCode::invalid_argument,
        "a zero revision digest is rejected for manifest tampering");

    require(static_cast<bool>(production::read_selected_prepared_scene(root)),
        "rejected corruption attempts leave the publication fully readable");
}

} // namespace

int main() {
    corrupt_product_corpus_fails_closed_reading();
    manifest_tamper_corpus_fails_closed_reading();
    corruption_helpers_reject_unsafe_targets();
    std::cout << "verification corrupt artifact tests passed\n";
    return EXIT_SUCCESS;
}
