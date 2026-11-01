#include <carto/native_source/project_mesh_source.hpp>
#include <carto/native_source/project_sweep_source.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace carto;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
void okay(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
assets::Sha256Digest hash_text(const std::string& text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
template<class F> auto read_only(const project::ProjectDocument& document, F call) {
    const auto before = document.serialize();
    const auto revision = document.revision();
    auto result = call();
    check(document.serialize() == before && document.revision() == revision,
          "candidate derivation/refusal must not mutate project state");
    return result;
}

class TempDirectory {
public:
    TempDirectory() : path_(std::filesystem::temp_directory_path() /
        ("cartographer-native-source-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))) {
        check(std::filesystem::create_directory(path_), "create a new owned fixture directory");
    }
    ~TempDirectory() {
        std::error_code ignored;
        if (path_.lexically_normal().parent_path() ==
            std::filesystem::temp_directory_path().lexically_normal())
            std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

// Synthetic fixture mutations use the real journaled owner boundary.
// Producer calls are read-only and grant no user/AI mutation authority.
struct Fixture {
    TempDirectory directory;
    journal::Journal journal;
    project::ProjectDocument document;
    std::uint64_t asset = 7U;
    scene::ObjectId first{11U};
    scene::ObjectId second{12U};

    explicit Fixture(geometry::EditableMesh mesh, bool visible = true,
                     bool scaled = false, bool parent_second = false)
        : journal(directory.path() / "fixture.events") {
        auto first_transform = core::Transform::identity();
        first_transform.translation = {2.0, 3.0, 4.0};
        if (scaled) first_transform.scale.x = 2.0;
        auto second_transform = core::Transform::identity();
        second_transform.translation = {20.0, 30.0, 40.0};
        change([&](auto& transaction) {
            okay(transaction.insert_mesh(asset, std::move(mesh)));
            okay(transaction.insert_object({first, "Synthetic selected instance", first_transform,
                std::nullopt, asset, visible, false}));
            okay(transaction.insert_object({second, "Synthetic shared instance", second_transform,
                parent_second ? std::optional<scene::ObjectId>{first} : std::nullopt,
                asset, true, false}));
        }, "native.source.fixture.create");
        okay(document.validate());
    }
    template<class F> void change(F edits, const std::string& operation) {
        const auto count = take(journal.read_all()).size();
        auto transaction = take(project::ProjectTransaction::begin(
            document, journal, "native-source-test-fixture"));
        edits(transaction);
        const auto receipt = take(transaction.commit(operation));
        const auto events = take(journal.read_all());
        check(events.size() == count + 1U && events.back().event_type == operation &&
              events.back().revision_before == receipt.revision_before &&
              events.back().revision_after == receipt.revision_after,
              "fixture mutation must retain its exact journal event and revisions");
        okay(journal.verify());
    }
    void advance_mesh_revision() {
        change([&](auto& transaction) {
            okay(transaction.replace_mesh(asset, document.meshes().at(asset)));
        }, "native.source.fixture.mesh-revision");
    }
    geometry::FaceId face() const { return document.meshes().at(asset).faces_sorted().front().id; }
    render::RenderScene viewport() const {
        const auto& mesh = document.meshes().at(asset);
        render::RenderScene result;
        okay(result.upsert({first, std::make_shared<const geometry::CompiledMesh>(take(mesh.compile())),
            take(document.scene().world_transform(first)), mesh.revision(), asset}));
        return result;
    }
};

geometry::EditableMesh triangle() {
    geometry::EditableMesh mesh;
    const auto a = take(mesh.add_vertex({0.0, 0.0, 0.0}));
    const auto b = take(mesh.add_vertex({2.0, 0.0, 0.0}));
    const auto c = take(mesh.add_vertex({0.0, 3.0, 0.0}));
    take(mesh.add_face({a, b, c}));
    return mesh;
}

void selected_instance_and_persistence() {
    Fixture fixture(triangle());
    const auto first = take(read_only(fixture.document, [&] {
        return native_source::derive_project_mesh_source(fixture.document, fixture.first);
    }));
    const auto second = take(read_only(fixture.document, [&] {
        return native_source::derive_project_mesh_source(fixture.document, fixture.second);
    }));
    check(first.mesh_asset == second.mesh_asset && first.object != second.object,
          "a shared mesh does not collapse distinct selected instances");
    check(first.source_digest != second.source_digest &&
          first.occurrence.external_id != second.occurrence.external_id,
          "source identity must retain selected object and transform");
    check(first.source_projection.starts_with("CARTO_NATIVE_PROJECT_MESH 1\n") &&
          first.source_digest == hash_text(first.source_projection),
          "source digest binds actual native projection bytes");
    check(first.render_mesh_digest == assets::sha256(first.render_mesh_bytes) &&
          first.editable_mesh_digest == project::canonical_mesh_snapshot_digest(
              fixture.document.meshes().at(fixture.asset)),
          "render bytes and editable mesh retain their own digest domains");
    check(first.physical_mesh_candidate.valid() && !first.render_asset_binding_verified(),
          "even an open triangle is a source candidate, not physical or display admission");
    fixture.change([](auto& transaction) {
        take(transaction.create_object("Unrelated synthetic object"));
    }, "native.source.fixture.unrelated-object");
    const auto unrelated = take(native_source::derive_project_mesh_source(fixture.document, fixture.first));
    check(unrelated.source_digest == first.source_digest,
          "unrelated project/scene revisions must not create a circular source hash");
    const auto reopened = take(project::ProjectDocument::deserialize(fixture.document.serialize()));
    const auto restored = take(read_only(reopened, [&] {
        return native_source::derive_project_mesh_source(reopened, fixture.first);
    }));
    check(restored.source_digest == first.source_digest &&
          restored.source_projection == first.source_projection,
          "save/reopen preserves this exact selected source without claiming historical survival");
    check(!native_source::derive_project_mesh_source(fixture.document, scene::ObjectId{}) &&
          !native_source::derive_project_mesh_source(fixture.document, scene::ObjectId{999U}),
          "missing selected-object identities refuse");
}

template<class F> void reject_mesh_tamper(Fixture& fixture, F change, const char* message) {
    auto viewport = fixture.viewport();
    auto instance = *viewport.find(fixture.first);
    auto changed = std::make_shared<geometry::CompiledMesh>(*instance.mesh);
    change(*changed);
    check(changed->valid(), "tampered viewport fixture must remain structurally valid");
    instance.source_revision = changed->source_revision;
    instance.mesh = std::move(changed);
    okay(viewport.upsert(std::move(instance)));
    check(!read_only(fixture.document, [&] {
        return native_source::verify_project_viewport_face(
            fixture.document, viewport, fixture.first, fixture.face());
    }), message);
}

void viewport_requires_exact_current_structure() {
    Fixture fixture(triangle());
    auto viewport = fixture.viewport();
    const auto selected = take(read_only(fixture.document, [&] {
        return native_source::verify_project_viewport_face(
            fixture.document, viewport, fixture.first, fixture.face());
    }));
    check(selected.object == fixture.first && selected.mesh_asset == fixture.asset &&
          selected.compiled_triangles.size() == 1U,
          "matching viewport maps the selected authored face to actual compiled triangles");
    // This is structural consistency, not authentication of a caller-created
    // RenderScene, proof of displayed pixels, or an admitted picking gesture.
    check(!native_source::verify_project_viewport_face(
              fixture.document, viewport, fixture.second, fixture.face()) &&
          !native_source::verify_project_viewport_face(
              fixture.document, viewport, fixture.first, geometry::FaceId{999U}),
          "wrong shared instance and absent source face refuse");
    auto incoherent = *viewport.find(fixture.first);
    incoherent.source_revision = core::Revision{999U};
    render::RenderScene rejected_view;
    check(!rejected_view.upsert(std::move(incoherent)),
          "renderer itself blocks an incoherent instance/mesh revision");
    for (int field = 0; field != 3; ++field) {
        auto changed_view = fixture.viewport();
        auto changed = *changed_view.find(fixture.first);
        if (field == 0) changed.mesh_asset = 999U;
        if (field == 1) {
            auto stale_mesh = std::make_shared<geometry::CompiledMesh>(*changed.mesh);
            stale_mesh->source_revision = core::Revision{999U};
            changed.source_revision = stale_mesh->source_revision;
            changed.mesh = std::move(stale_mesh);
        }
        if (field == 2) changed.world_transform.translation.x += 0.25;
        okay(changed_view.upsert(std::move(changed)));
        check(!read_only(fixture.document, [&] {
            return native_source::verify_project_viewport_face(
                fixture.document, changed_view, fixture.first, fixture.face());
        }), "wrong asset, source revision or world transform refuses without project mutation");
    }
    reject_mesh_tamper(fixture, [](auto& m) { m.positions.front().x += 0.125; }, "altered position refuses");
    reject_mesh_tamper(fixture, [](auto& m) { m.normals.front().x += 0.125; }, "altered normal refuses");
    reject_mesh_tamper(fixture, [](auto& m) { std::swap(m.indices[0], m.indices[1]); }, "altered winding refuses");
    reject_mesh_tamper(fixture, [](auto& m) { m.vertex_ids.front() = geometry::VertexId{777U}; }, "altered vertex identity refuses");
    reject_mesh_tamper(fixture, [](auto& m) { m.triangle_faces.front() = geometry::FaceId{777U}; }, "altered face identity refuses");
    reject_mesh_tamper(fixture, [](auto& m) { m.triangle_edges.front()[0] = geometry::EdgeId{777U}; }, "altered edge identity refuses");
    reject_mesh_tamper(fixture, [](auto& m) { m.bounds.maximum.x += 0.125; }, "altered bounds refuse");
    reject_mesh_tamper(fixture, [](auto& m) { m.source_revision = core::Revision{999U}; }, "altered compiled revision refuses");
    fixture.advance_mesh_revision();
    check(!read_only(fixture.document, [&] {
        return native_source::verify_project_viewport_face(
            fixture.document, viewport, fixture.first, fixture.face());
    }), "a stale viewport refuses after the actual source asset revision changes");
    Fixture hidden(triangle(), false);
    auto hidden_view = hidden.viewport();
    check(!native_source::verify_project_viewport_face(
        hidden.document, hidden_view, hidden.first, hidden.face()), "hidden source cannot be a visible viewport face");
    Fixture scaled(triangle(), true, true);
    check(!native_source::derive_project_mesh_source(scaled.document, scaled.first),
          "unsupported non-rigid native source refuses instead of silently changing units/frame");
    Fixture parented(triangle(), true, false, true);
    check(!native_source::derive_project_mesh_source(parented.document, parented.second),
          "parented instance refuses at this explicitly root-only producer boundary");
}

geometry::CurvePath axis_path() {
    geometry::CurvePath path;
    path.source_identity = "synthetic.sweep.axis";
    path.source_revision = core::Revision{1U};
    path.segments.push_back({{0, 0, 0}, {0, 0.25, 0}, {0, 2.75, 0}, {0, 3, 0}});
    return path;
}
geometry::SweepSource rectangle_source() {
    geometry::SweepSource source;
    source.source_identity = "synthetic.rectangle.sweep";
    source.source_revision = core::Revision{1U};
    source.centerline_identity = "synthetic.sweep.axis";
    source.centerline_revision = core::Revision{1U};
    source.owner_object_id = 11U;
    source.profile = {{-1, -2}, {1, -2}, {1, 2}, {-1, 2}};
    source.reference_up = {0, 0, 1};
    source.samples_per_segment = 3U;
    return source;
}

void sweep_measure_and_exact_project_correspondence() {
    auto source = rectangle_source();
    auto path = axis_path();
    const auto measured = take(geometry::measure_straight_sweep(source, path));
    check(std::abs(measured.cross_section_area - 8.0) < 1e-12 &&
          std::abs(measured.length - 3.0) < 1e-12 &&
          std::abs(measured.source_model_volume - 24.0) < 1e-12,
          "independent rectangle area and axis length give analytic source volume");
    check(!measured.volume_absolute_numeric_error_bound.has_value(),
          "unestablished numerical error stays unknown, never fabricated zero");
    auto scaled = source;
    scaled.start_scale = scaled.end_scale = 2.0;
    check(std::abs(take(geometry::measure_straight_sweep(scaled, path)).source_model_volume - 96.0) < 1e-12,
          "constant profile scale squares area without inventing physical density");
    auto reversed = path;
    reversed.segments.front() = {{0, 0, 0}, {0, -0.25, 0}, {0, -2.75, 0}, {0, -3, 0}};
    check(std::abs(take(geometry::measure_straight_sweep(source, reversed)).source_model_volume - 24.0) < 1e-12,
          "reversed axis preserves positive source-model volume");
    Fixture fixture(take(geometry::build_sweep_mesh(source, path)).mesh);
    fixture.change([&](auto& transaction) {
        okay(transaction.set_curve_path(path));
        okay(transaction.set_sweep_source(source));
    }, "native.source.fixture.sweep-source");
    const auto product = take(read_only(fixture.document, [&] {
        return native_source::derive_project_straight_sweep(
            fixture.document, fixture.first, source.source_identity);
    }));
    check(product.sweep_source_digest == hash_text(product.sweep_source_bytes) &&
          product.centerline_source_digest == hash_text(product.centerline_source_bytes) &&
          product.generated_mesh_digest == product.selected_mesh.editable_mesh_digest,
          "actual serialized source/path and regenerated selected mesh remain content-bound");
    check(!product.historical_origin_verified() && !product.selected_mesh.render_asset_binding_verified() &&
          product.operation_identity.hex() != std::string(64U, '0'),
          "content-derived operation identity does not become history, displayed-asset or physical authority");
    check(!read_only(fixture.document, [&] {
        return native_source::derive_project_straight_sweep(
            fixture.document, fixture.second, source.source_identity);
    }), "same mesh on a different instance cannot take the sweep owner's frame");
    const auto reopened = take(project::ProjectDocument::deserialize(fixture.document.serialize()));
    const auto restored = take(native_source::derive_project_straight_sweep(reopened, fixture.first, source.source_identity));
    check(restored.operation_identity == product.operation_identity,
          "save/reopen retains this exact current source/mesh correspondence without proving historical origin");
    fixture.advance_mesh_revision();
    check(!read_only(fixture.document, [&] {
        return native_source::derive_project_straight_sweep(
            fixture.document, fixture.first, source.source_identity);
    }), "changed selected mesh revision cannot reuse an earlier regenerated-sweep claim");
}

void unsupported_sweep_bases_refuse() {
    const auto original_source = rectangle_source();
    const auto original_path = axis_path();
    for (int case_id = 0; case_id != 9; ++case_id) {
        auto source = original_source;
        auto path = original_path;
        if (case_id == 0) source.end_scale = 0.5;
        if (case_id == 1) source.inner_profile_scale = 0.5;
        if (case_id == 2) source.cap_policy = geometry::SweepCapPolicy::open;
        if (case_id == 3) path.segments.front().p1.x = 0.25;
        if (case_id == 4) path.segments.front().p1.y = 2.9;
        if (case_id == 5) path.segments.push_back({{0, 3, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}});
        if (case_id == 6) path.source_revision = core::Revision{2U};
        if (case_id == 7) source.start_scale = source.end_scale = std::numeric_limits<double>::max();
        if (case_id == 8) source.start_scale = std::numeric_limits<double>::quiet_NaN();
        const auto before_source = source.serialize();
        const auto before_path = path.serialize();
        check(!geometry::measure_straight_sweep(source, path),
              "taper/hollow/open/curved/nonmonotone/multisegment/stale/overflow/nonfinite bases refuse");
        check(source.serialize() == before_source && path.serialize() == before_path,
              "unsupported model refusal never rewrites source into a supported fallback");
    }
}
} // namespace

int main() {
    try {
        selected_instance_and_persistence();
        viewport_requires_exact_current_structure();
        sweep_measure_and_exact_project_correspondence();
        unsupported_sweep_bases_refuse();
        std::cout << "Native project source boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native source boundary failure: " << error.what() << '\n';
        return 1;
    }
}
