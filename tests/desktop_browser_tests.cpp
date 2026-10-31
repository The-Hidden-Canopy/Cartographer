#include "../apps/desktop/studio_browser.hpp"

#include <carto/geometry/primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

using carto::application::ObjectView;
using carto::scene::ObjectId;
using carto::studio::SceneBrowserFilter;

ObjectView object(std::uint64_t id, std::string name,
                  std::optional<std::uint64_t> parent = std::nullopt,
                  std::optional<std::uint64_t> mesh = std::nullopt,
                  bool visible = true, bool locked = false) {
    ObjectView result;
    result.object.id = ObjectId{id};
    result.object.name = std::move(name);
    if (parent.has_value()) result.object.parent = ObjectId{*parent};
    result.object.mesh_asset = mesh;
    result.object.visible = visible;
    result.object.locked = locked;
    return result;
}

std::vector<std::uint64_t> row_ids(const std::vector<ObjectView>& objects,
                                  const carto::studio::SceneBrowser& browser) {
    std::vector<std::uint64_t> ids;
    for (const auto& row : browser.rows) ids.push_back(objects.at(row.object_index).object.id.value);
    return ids;
}

void unchanged(const std::vector<ObjectView>& before, const std::vector<ObjectView>& after) {
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0U; i < before.size(); ++i) {
        const auto& left = before[i].object;
        const auto& right = after[i].object;
        REQUIRE(left.id == right.id && left.name == right.name && left.parent == right.parent);
        REQUIRE(left.mesh_asset == right.mesh_asset && left.visible == right.visible && left.locked == right.locked);
        REQUIRE(before[i].mesh_revision == after[i].mesh_revision);
    }
}

void hierarchy_and_filter_context() {
    std::vector<ObjectView> objects{
        object(40U, "Fuel Pump", 20U, 301U, false, true),
        object(30U, "Sibling", 10U, 302U),
        object(10U, "Assembly"), object(20U, "Utilities", 10U), object(50U, "Other root")};
    const auto before = objects;
    const auto full = carto::studio::build_scene_browser(objects);
    REQUIRE(full && full.value().matching_objects == 5U);
    REQUIRE((row_ids(objects, full.value()) == std::vector<std::uint64_t>{10U, 20U, 40U, 30U, 50U}));
    REQUIRE(full.value().rows[0].has_children && full.value().rows[1].has_children);
    REQUIRE(full.value().rows[2].depth == 2U && !full.value().rows[2].has_children);

    const auto search = carto::studio::build_scene_browser(objects, "  fUEL puMP \t");
    REQUIRE(search && search.value().matching_objects == 1U);
    REQUIRE((row_ids(objects, search.value()) == std::vector<std::uint64_t>{10U, 20U, 40U}));
    REQUIRE(search.value().rows[0].context_only && search.value().rows[1].context_only);
    REQUIRE(!search.value().rows[2].context_only);
    for (const auto filter : {SceneBrowserFilter::hidden, SceneBrowserFilter::locked}) {
        const auto filtered = carto::studio::build_scene_browser(objects, {}, filter);
        REQUIRE(filtered && filtered.value().matching_objects == 1U);
        REQUIRE(row_ids(objects, filtered.value()) == row_ids(objects, search.value()));
    }
    const auto asset_id_search = carto::studio::build_scene_browser(objects, "301");
    REQUIRE(asset_id_search && asset_id_search.value().matching_objects == 1U);
    const auto absent = carto::studio::build_scene_browser(objects, "missing");
    REQUIRE(absent && absent.value().rows.empty());
    unchanged(before, objects);
    std::reverse(objects.begin(), objects.end());
    const auto reordered = carto::studio::build_scene_browser(objects);
    REQUIRE(reordered && row_ids(objects, reordered.value()) == row_ids(before, full.value()));
}

void malformed_hierarchy_is_explicit_even_when_filtered() {
    std::vector<ObjectView> duplicate{object(1U, "One"), object(1U, "Hidden duplicate")};
    REQUIRE(!carto::studio::build_scene_browser(duplicate, "no match"));
    REQUIRE(!carto::studio::build_mesh_asset_browser(duplicate, "no match"));
    std::vector<ObjectView> orphan{object(2U, "Orphan", 99U)};
    const auto missing = carto::studio::build_scene_browser(orphan, "no match");
    REQUIRE(!missing && missing.error().message.find("missing parent") != std::string::npos);
    for (const auto& cycle : std::vector<std::vector<ObjectView>>{
             {object(1U, "Self", 1U)}, {object(1U, "First", 2U), object(2U, "Second", 1U)}}) {
        const auto before = cycle;
        const auto result = carto::studio::build_scene_browser(cycle, "unmatched");
        REQUIRE(!result && result.error().code == carto::core::ErrorCode::cycle_detected);
        unchanged(before, cycle);
    }
    const std::vector<ObjectView> valid{object(1U, "Valid")};
    REQUIRE(!carto::studio::build_scene_browser(valid, {}, static_cast<SceneBrowserFilter>(99)));
    REQUIRE(!carto::studio::build_scene_browser(valid, std::string(257U, 'a')));
    REQUIRE(!carto::studio::build_mesh_asset_browser(valid, std::string_view("a\0b", 3U)));
    REQUIRE(!carto::studio::build_scene_browser(std::vector<ObjectView>{object(0U, "Invalid ID")}));
    REQUIRE(!carto::studio::build_scene_browser(std::vector<ObjectView>{object(1U, "")}));
    REQUIRE(!carto::studio::build_mesh_asset_browser(std::vector<ObjectView>{object(1U, "Invalid mesh", {}, 0U)}));
    const auto empty = carto::studio::build_scene_browser({});
    REQUIRE(empty && empty.value().rows.empty() && empty.value().matching_objects == 0U);
}

void deep_hierarchy_is_bounded_and_iterative() {
    std::vector<ObjectView> objects;
    for (std::uint64_t id = 1U; id <= 2048U; ++id) {
        objects.push_back(object(id, id == 2048U ? "Deep target" : "Ancestor",
            id == 1U ? std::nullopt : std::optional<std::uint64_t>(id - 1U)));
    }
    const auto deep = carto::studio::build_scene_browser(objects, "Deep target");
    REQUIRE(deep && deep.value().matching_objects == 1U && deep.value().rows.size() == 2048U);
    REQUIRE(deep.value().rows.back().depth == 2047U);
    REQUIRE(deep.value().rows.front().context_only && !deep.value().rows.back().context_only);
    objects.resize(carto::studio::kMaxBrowserObjects + 1U);
    const auto oversized = carto::studio::build_scene_browser(objects);
    REQUIRE(!oversized && oversized.error().code == carto::core::ErrorCode::unsupported);
}

void real_assets_group_instances_and_preserve_counts() {
    std::vector<ObjectView> objects{
        object(30U, "Pump alternate", {}, 7U, false, true), object(10U, "Pump", {}, 7U),
        object(20U, "Floor", {}, 2U), object(40U, "Empty assembly")};
    const auto before = objects;
    const auto all = carto::studio::build_mesh_asset_browser(objects);
    REQUIRE(all && all.value().total_mesh_assets == 2U && all.value().total_mesh_instances == 3U);
    REQUIRE(all.value().rows.size() == 2U && all.value().rows[0].mesh_asset == 2U);
    REQUIRE(all.value().rows[1].mesh_asset == 7U && all.value().rows[1].object_indices.size() == 2U);
    REQUIRE(objects[all.value().rows[1].object_indices[0]].object.id == ObjectId{10U});
    const auto searched = carto::studio::build_mesh_asset_browser(objects, "ALTERNATE");
    REQUIRE(searched && searched.value().rows.size() == 1U);
    REQUIRE(searched.value().total_mesh_assets == 2U && searched.value().total_mesh_instances == 3U);
    REQUIRE(searched.value().rows[0].object_indices.size() == 2U);
    REQUIRE(objects[searched.value().rows[0].representative_index].object.id == ObjectId{30U});
    const auto absent = carto::studio::build_mesh_asset_browser(objects, "absent");
    REQUIRE(absent && absent.value().rows.empty() && absent.value().total_mesh_instances == 3U);
    const auto mesh_id = carto::studio::build_mesh_asset_browser(objects, "7");
    REQUIRE(mesh_id && mesh_id.value().rows.size() == 1U && mesh_id.value().rows[0].mesh_asset == 7U);
    carto::application::SourceAssetView source;
    source.identity = "source.plants-pine";
    source.relative_source_path = "plants/pine.obj";
    source.kind = carto::assets::SourceAssetKind::mesh;
    source.source_generation = carto::core::Revision{4U};
    source.bound_mesh_asset = 7U;
    source.bound_mesh_vertex_count = 3U;
    source.bound_mesh_face_count = 1U;
    const std::vector<carto::application::SourceAssetView> sources{source};
    const auto source_search = carto::studio::build_mesh_asset_browser(
        objects, "plants/pine.obj", sources);
    REQUIRE(source_search && source_search.value().rows.size() == 1U &&
        source_search.value().rows.front().mesh_asset == 7U &&
        source_search.value().rows.front().source_index == 0U);
    auto conflicting_source = source;
    conflicting_source.identity = "source.other-pine";
    const std::vector<carto::application::SourceAssetView> ambiguous_sources{
        source, conflicting_source};
    REQUIRE(!carto::studio::build_mesh_asset_browser(objects, {}, ambiguous_sources));
    auto non_mesh_binding = source;
    non_mesh_binding.kind = carto::assets::SourceAssetKind::texture;
    const std::vector<carto::application::SourceAssetView> invalid_binding{non_mesh_binding};
    REQUIRE(!carto::studio::build_mesh_asset_browser(objects, {}, invalid_binding));
    auto missing_mesh_counts = source;
    missing_mesh_counts.bound_mesh_face_count.reset();
    const std::vector<carto::application::SourceAssetView> invalid_counts{missing_mesh_counts};
    REQUIRE(!carto::studio::build_mesh_asset_browser(objects, {}, invalid_counts));
    unchanged(before, objects);
    std::reverse(objects.begin(), objects.end());
    const auto reversed = carto::studio::build_mesh_asset_browser(objects);
    REQUIRE(reversed && reversed.value().rows[0].mesh_asset == all.value().rows[0].mesh_asset);
    REQUIRE(objects[reversed.value().rows[1].representative_index].object.id == ObjectId{10U});
}

void mesh_drop_sources_require_current_revision_bound_roots() {
    auto locked = object(1U, "Locked root", {}, 90U, true, true);
    locked.mesh_revision = carto::core::Revision{1U};
    auto hidden = object(2U, "Hidden root", {}, 90U, false, false);
    hidden.mesh_revision = carto::core::Revision{1U};
    auto child = object(3U, "Nested root member", 1U, 90U, true, false);
    child.mesh_revision = carto::core::Revision{1U};
    auto current = object(4U, "Current root", {}, 90U, true, false);
    current.mesh_revision = carto::core::Revision{3U};
    const auto unversioned = object(5U, "Unversioned root", {}, 90U, true, false);
    auto zero_revision = object(6U, "Zero revision root", {}, 90U, true, false);
    zero_revision.mesh_revision = carto::core::Revision{};
    std::vector<ObjectView> objects{
        locked, hidden, child, current, unversioned, zero_revision};

    const auto browser = carto::studio::build_mesh_asset_browser(objects);
    REQUIRE(browser && browser.value().rows.size() == 1U);
    const auto& row = browser.value().rows.front();
    REQUIRE(row.object_indices.size() == 6U);
    REQUIRE(row.placeable_object_indices.size() == 1U);
    REQUIRE(objects[row.placeable_object_indices.front()].object.id == ObjectId{4U});
    REQUIRE(objects[row.representative_index].object.id == ObjectId{1U});
    REQUIRE(carto::studio::has_mesh_revision_binding(current));
    REQUIRE(!carto::studio::has_mesh_revision_binding(unversioned));
    REQUIRE(!carto::studio::has_mesh_revision_binding(zero_revision));
    REQUIRE(carto::studio::mesh_revision_matches_current_snapshot(
        current, carto::core::Revision{3U}));
    REQUIRE(!carto::studio::mesh_revision_matches_current_snapshot(
        current, carto::core::Revision{4U}));
    REQUIRE(!carto::studio::mesh_revision_matches_current_snapshot(
        unversioned, carto::core::Revision{1U}));
    REQUIRE(!carto::studio::mesh_revision_matches_current_snapshot(
        zero_revision, carto::core::Revision{}));
}

void source_registry_search_is_read_only_and_generation_bound() {
    using carto::application::SourceAssetView;
    const std::array<std::uint8_t, 4U> source_bytes{'p', 'i', 'n', 'e'};
    const std::array<std::uint8_t, 4U> artifact_bytes{'m', 'e', 's', 'h'};
    const std::array<std::uint8_t, 4U> geometry_bytes{'g', 'e', 'o', 'm'};
    SourceAssetView mesh;
    mesh.identity = "source.pine-01";
    mesh.relative_source_path = "plants/pine.obj";
    mesh.kind = carto::assets::SourceAssetKind::mesh;
    mesh.source_generation = carto::core::Revision{7U};
    mesh.importer_identity = "obj-importer";
    mesh.importer_version = 314159U;
    mesh.artifact_identity = "mesh:pine-01";
    mesh.source_digest = carto::assets::sha256(source_bytes);
    mesh.artifact_digest = carto::assets::sha256(artifact_bytes);
    mesh.domain_digests.push_back({carto::assets::SourceAssetDomain::geometry,
        carto::assets::sha256(geometry_bytes)});
    mesh.authored_tags = {"conifer", "exterior"};
    mesh.dependencies = {"source.bark-02"};

    SourceAssetView material;
    material.identity = "source.bark-02";
    material.relative_source_path = "materials/bark.mtl";
    material.kind = carto::assets::SourceAssetKind::material;
    material.source_generation = carto::core::Revision{2U};
    material.importer_identity = "mtl-importer";
    material.artifact_identity = "material:bark-02";
    const std::vector<SourceAssetView> sources{mesh, material};

    const auto all = carto::studio::build_source_asset_browser(sources);
    REQUIRE(all && all.value().total_source_assets == 2U && all.value().rows.size() == 2U);
    const auto importer = carto::studio::build_source_asset_browser(sources, " OBJ-IMPORTER ");
    REQUIRE(importer && importer.value().rows.size() == 1U &&
        importer.value().rows[0].source_index == 0U);
    const auto source_path = carto::studio::build_source_asset_browser(sources, "BARK.MTL");
    REQUIRE(source_path && source_path.value().rows.size() == 1U &&
        source_path.value().rows[0].source_index == 1U);
    const auto generation = carto::studio::build_source_asset_browser(sources, "7");
    REQUIRE(generation && generation.value().rows.size() == 1U &&
        generation.value().rows[0].source_index == 0U);
    const auto importer_version = carto::studio::build_source_asset_browser(sources, "314159");
    REQUIRE(importer_version && importer_version.value().rows.size() == 1U &&
        importer_version.value().rows[0].source_index == 0U);
    const auto source_digest = carto::studio::build_source_asset_browser(sources,
        mesh.source_digest.hex().substr(0U, 12U));
    REQUIRE(source_digest && source_digest.value().rows.size() == 1U &&
        source_digest.value().rows[0].source_index == 0U);
    const auto artifact_digest = carto::studio::build_source_asset_browser(sources,
        mesh.artifact_digest.hex().substr(0U, 12U));
    REQUIRE(artifact_digest && artifact_digest.value().rows.size() == 1U &&
        artifact_digest.value().rows[0].source_index == 0U);
    const auto domain_name = carto::studio::build_source_asset_browser(sources, "GEOMETRY");
    REQUIRE(domain_name && domain_name.value().rows.size() == 1U &&
        domain_name.value().rows[0].source_index == 0U);
    const auto domain_digest = carto::studio::build_source_asset_browser(sources,
        mesh.domain_digests[0].digest.hex().substr(0U, 12U));
    REQUIRE(domain_digest && domain_digest.value().rows.size() == 1U &&
        domain_digest.value().rows[0].source_index == 0U);
    const auto authored_tag = carto::studio::build_source_asset_browser(sources, "CONIFER");
    REQUIRE(authored_tag && authored_tag.value().rows.size() == 1U &&
        authored_tag.value().rows[0].source_index == 0U);
    const auto dependency = carto::studio::build_source_asset_browser(sources, "source.bark");
    REQUIRE(dependency && dependency.value().rows.size() == 2U);
    const auto no_match = carto::studio::build_source_asset_browser(sources, "not-present");
    REQUIRE(no_match && no_match.value().total_source_assets == 2U && no_match.value().rows.empty());
    REQUIRE(!carto::studio::build_source_asset_browser(sources, std::string(257U, 'x')));
    REQUIRE(!carto::studio::build_source_asset_browser(sources, std::string_view("x\0y", 3U)));
    const std::vector<SourceAssetView> duplicate{mesh, mesh};
    REQUIRE(!carto::studio::build_source_asset_browser(duplicate));
    auto duplicate_domain = mesh;
    duplicate_domain.domain_digests.push_back(mesh.domain_digests[0]);
    const std::vector<SourceAssetView> duplicate_domain_sources{duplicate_domain};
    REQUIRE(!carto::studio::build_source_asset_browser(duplicate_domain_sources));
    auto empty_domain_digest = mesh;
    empty_domain_digest.domain_digests[0].digest = {};
    const std::vector<SourceAssetView> empty_domain_sources{empty_domain_digest};
    REQUIRE(!carto::studio::build_source_asset_browser(empty_domain_sources));
    auto invalid_import_options = mesh;
    invalid_import_options.options.meters_per_source_unit = 0.0;
    const std::vector<SourceAssetView> invalid_import_option_sources{invalid_import_options};
    REQUIRE(!carto::studio::build_source_asset_browser(invalid_import_option_sources));
    auto unknown_domain = mesh;
    unknown_domain.domain_digests[0].domain =
        static_cast<carto::assets::SourceAssetDomain>(3U);
    const std::vector<SourceAssetView> unknown_domain_sources{unknown_domain};
    REQUIRE(!carto::studio::build_source_asset_browser(unknown_domain_sources));
    auto invalid_tag = mesh;
    invalid_tag.authored_tags = {"Conifer"};
    const std::vector<SourceAssetView> invalid_tag_sources{invalid_tag, material};
    REQUIRE(!carto::studio::build_source_asset_browser(invalid_tag_sources));
    auto self_dependency = mesh;
    self_dependency.dependencies = {self_dependency.identity};
    const std::vector<SourceAssetView> self_dependency_sources{self_dependency, material};
    REQUIRE(!carto::studio::build_source_asset_browser(self_dependency_sources));
    auto missing_dependency = mesh;
    missing_dependency.dependencies = {"source.absent"};
    const std::vector<SourceAssetView> missing_dependency_sources{missing_dependency, material};
    REQUIRE(!carto::studio::build_source_asset_browser(missing_dependency_sources));
    const std::vector<SourceAssetView> unnamed{SourceAssetView{}};
    REQUIRE(!carto::studio::build_source_asset_browser(unnamed));
    REQUIRE(sources[0].identity == mesh.identity &&
        sources[0].source_generation == mesh.source_generation &&
        sources[0].source_digest == mesh.source_digest &&
        sources[0].domain_digests == mesh.domain_digests &&
        sources[0].authored_tags == mesh.authored_tags &&
        sources[0].dependencies == mesh.dependencies &&
        sources[1].identity == material.identity);
}

void source_blob_integrity_status_is_snapshot_bound_and_explicit() {
    using carto::assets::BlobOrphanReport;
    using carto::assets::Sha256Digest;
    using carto::studio::SourceBlobIntegrityStatus;

    const std::array<std::uint8_t, 4U> bytes{'b', 'l', 'o', 'b'};
    const Sha256Digest digest = carto::assets::sha256(bytes);
    BlobOrphanReport report;
    report.missing_live_references.push_back(digest);
    REQUIRE(carto::studio::source_blob_integrity_status(&report, false, digest) ==
        SourceBlobIntegrityStatus::unchecked);
    report.missing_live_references.clear();
    REQUIRE(carto::studio::source_blob_integrity_status(nullptr, true, digest) ==
        SourceBlobIntegrityStatus::unchecked);
    REQUIRE(carto::studio::source_blob_integrity_status(&report, true, digest) ==
        SourceBlobIntegrityStatus::verified);
    REQUIRE(carto::studio::source_blob_integrity_label(SourceBlobIntegrityStatus::verified) ==
        "verified on last inventory");

    report.missing_live_references.push_back(digest);
    REQUIRE(carto::studio::source_blob_integrity_status(&report, true, digest) ==
        SourceBlobIntegrityStatus::missing);
    report.missing_live_references.clear();
    report.corrupt.push_back(digest);
    REQUIRE(carto::studio::source_blob_integrity_status(&report, true, digest) ==
        SourceBlobIntegrityStatus::corrupt);
    report.corrupt.clear();
    report.orphan_candidates.push_back(digest);
    REQUIRE(carto::studio::source_blob_integrity_status(&report, true, digest) ==
        SourceBlobIntegrityStatus::catalog_mismatch);
    REQUIRE(carto::studio::source_blob_integrity_label(
        SourceBlobIntegrityStatus::catalog_mismatch) == "catalog reachability mismatch");
}

void mesh_thumbnail_projection_uses_bounded_compiled_geometry() {
    auto editable = carto::geometry::make_box({1.0, 2.0, 3.0});
    REQUIRE(editable);
    const auto compiled = editable.value().compile();
    REQUIRE(compiled && compiled.value().valid());
    const auto projection = carto::studio::build_mesh_thumbnail_projection(compiled.value());
    REQUIRE(projection &&
        projection.value().vertices.size() == compiled.value().positions.size() &&
        projection.value().triangles.size() == compiled.value().indices.size() / 3U);
    REQUIRE(std::all_of(projection.value().vertices.begin(), projection.value().vertices.end(),
        [](const auto& point) {
            return std::isfinite(point.x) && std::isfinite(point.y) &&
                point.x >= 0.0 && point.x <= 1.0 && point.y >= 0.0 && point.y <= 1.0;
        }));
    REQUIRE(projection.value().projected_extent.x > 0.0 &&
        projection.value().projected_extent.y > 0.0);
    const auto rotated_projection = carto::studio::build_mesh_thumbnail_projection(
        compiled.value(), carto::studio::MeshThumbnailView{0, 0});
    REQUIRE(rotated_projection && rotated_projection.value().vertices.size() ==
        projection.value().vertices.size());
    bool changed_view = false;
    for (std::size_t index = 0U; index < projection.value().vertices.size(); ++index) {
        changed_view = changed_view ||
            rotated_projection.value().vertices[index].x != projection.value().vertices[index].x ||
            rotated_projection.value().vertices[index].y != projection.value().vertices[index].y;
    }
    REQUIRE(changed_view);
    REQUIRE(!carto::studio::build_mesh_thumbnail_projection(
        compiled.value(), carto::studio::MeshThumbnailView{-1, 0}));
    const auto repeated = carto::studio::build_mesh_thumbnail_projection(compiled.value());
    REQUIRE(repeated && repeated.value().vertices.size() == projection.value().vertices.size() &&
        repeated.value().triangles.size() == projection.value().triangles.size() &&
        repeated.value().vertices.front().x == projection.value().vertices.front().x &&
        repeated.value().vertices.front().y == projection.value().vertices.front().y);

    auto larger_editable = carto::geometry::make_box({2.0, 4.0, 6.0});
    REQUIRE(larger_editable);
    const auto larger_compiled = larger_editable.value().compile();
    REQUIRE(larger_compiled);
    const auto larger_projection = carto::studio::build_mesh_thumbnail_projection(
        larger_compiled.value());
    REQUIRE(larger_projection);
    const auto shared_frame = carto::studio::mesh_thumbnail_shared_frame(
        &projection.value(), &larger_projection.value());
    REQUIRE(shared_frame);
    const auto smaller_point = carto::studio::mesh_thumbnail_vertex_at_shared_frame(
        projection.value(), 0U, *shared_frame);
    const auto larger_point = carto::studio::mesh_thumbnail_vertex_at_shared_frame(
        larger_projection.value(), 0U, *shared_frame);
    REQUIRE(smaller_point && larger_point &&
        std::abs(larger_point->x - 0.5) > std::abs(smaller_point->x - 0.5));
    REQUIRE(!carto::studio::mesh_thumbnail_vertex_at_shared_frame(
        projection.value(), projection.value().vertices.size(), *shared_frame));
    REQUIRE(!carto::studio::mesh_thumbnail_vertex_at_shared_frame(
        projection.value(), 0U, carto::studio::MeshThumbnailSharedFrame{}));

    auto translated_mesh = compiled.value();
    for (auto& point : translated_mesh.positions) point.x += 4.0;
    const auto translated_projection = carto::studio::build_mesh_thumbnail_projection(
        translated_mesh);
    REQUIRE(translated_projection);
    const auto translation_frame = carto::studio::mesh_thumbnail_shared_frame(
        &projection.value(), &translated_projection.value());
    REQUIRE(translation_frame);
    const auto original_origin_vertex = carto::studio::mesh_thumbnail_vertex_at_shared_frame(
        projection.value(), 0U, *translation_frame);
    const auto translated_origin_vertex = carto::studio::mesh_thumbnail_vertex_at_shared_frame(
        translated_projection.value(), 0U, *translation_frame);
    REQUIRE(original_origin_vertex && translated_origin_vertex &&
        original_origin_vertex->x != translated_origin_vertex->x);

    auto oversized = compiled.value();
    while (oversized.positions.size() <= carto::studio::kMaxThumbnailVertices) {
        oversized.positions.push_back(oversized.positions.front());
        oversized.normals.push_back(oversized.normals.front());
        oversized.vertex_ids.push_back(oversized.vertex_ids.front());
    }
    REQUIRE(oversized.valid());
    const auto refused = carto::studio::build_mesh_thumbnail_projection(oversized);
    REQUIRE(!refused && refused.error().code == carto::core::ErrorCode::unsupported);

    auto invalid = compiled.value();
    invalid.indices.front() = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(!carto::studio::build_mesh_thumbnail_projection(invalid));
}

void mesh_thumbnail_depth_buffer_resolves_crossings_independent_of_triangle_order() {
    const auto add_projected_triangle = [](
        carto::geometry::EditableMesh& mesh,
        std::array<double, 3U> depths) {
        const auto first = mesh.add_vertex({depths[0U], 0.0, 0.0});
        const auto second = mesh.add_vertex({depths[1U], 0.0, -1.0});
        const auto third = mesh.add_vertex({depths[2U], 1.0, 0.0});
        if (!first || !second || !third) return false;
        return static_cast<bool>(mesh.add_face(
            {first.value(), second.value(), third.value()}));
    };

    carto::geometry::EditableMesh layered;
    REQUIRE(add_projected_triangle(layered, {0.0, 0.0, 0.0}));
    REQUIRE(add_projected_triangle(layered, {5.0, 5.0, 5.0}));
    const auto layered_mesh = layered.compile();
    REQUIRE(layered_mesh);
    const auto layered_projection = carto::studio::build_mesh_thumbnail_projection(
        layered_mesh.value(), carto::studio::MeshThumbnailView{0, 0});
    REQUIRE(layered_projection && layered_projection.value().triangles.size() == 2U);
    const auto layered_frame = carto::studio::mesh_thumbnail_shared_frame(
        &layered_projection.value(), nullptr);
    REQUIRE(layered_frame);
    const auto layered_raster = carto::studio::rasterize_mesh_thumbnail(
        layered_projection.value(), *layered_frame, 64U, 64U);
    REQUIRE(layered_raster);
    const auto farthest_visible = std::max_element(
        layered_raster.value().camera_depth.begin(),
        layered_raster.value().camera_depth.end());
    REQUIRE(farthest_visible != layered_raster.value().camera_depth.end() &&
        *farthest_visible > 4.9);

    carto::geometry::EditableMesh crossing;
    REQUIRE(add_projected_triangle(crossing, {0.0, 2.0, 0.0}));
    REQUIRE(add_projected_triangle(crossing, {2.0, 0.0, 2.0}));
    const auto crossing_mesh = crossing.compile();
    REQUIRE(crossing_mesh);
    auto crossing_projection = carto::studio::build_mesh_thumbnail_projection(
        crossing_mesh.value(), carto::studio::MeshThumbnailView{0, 0});
    REQUIRE(crossing_projection && crossing_projection.value().triangles.size() == 2U);
    const auto crossing_frame = carto::studio::mesh_thumbnail_shared_frame(
        &crossing_projection.value(), nullptr);
    REQUIRE(crossing_frame);
    const auto crossing_raster = carto::studio::rasterize_mesh_thumbnail(
        crossing_projection.value(), *crossing_frame, 64U, 64U);
    REQUIRE(crossing_raster);
    auto reversed_projection = crossing_projection.value();
    std::reverse(reversed_projection.triangles.begin(), reversed_projection.triangles.end());
    const auto reversed_raster = carto::studio::rasterize_mesh_thumbnail(
        reversed_projection, *crossing_frame, 64U, 64U);
    REQUIRE(reversed_raster &&
        reversed_raster.value().camera_depth == crossing_raster.value().camera_depth);
    REQUIRE(!carto::studio::rasterize_mesh_thumbnail(
        crossing_projection.value(), *crossing_frame, 0U, 64U));
}

void thumbnail_cache_tracks_exact_compiled_source_without_stale_fallback() {
    auto editable = carto::geometry::make_box({1.0, 2.0, 3.0});
    REQUIRE(editable);
    auto compiled = editable.value().compile();
    REQUIRE(compiled);
    auto first = std::make_shared<const carto::geometry::CompiledMesh>(compiled.value());
    carto::studio::MeshThumbnailCacheEntry cache;
    const auto& initial = cache.projection_for(7U, first->source_revision, first);
    REQUIRE(initial && !initial->vertices.empty());
    const auto initial_frame = carto::studio::mesh_thumbnail_shared_frame(
        &*initial, nullptr);
    REQUIRE(initial_frame);
    const auto& initial_raster = cache.raster_for(&*initial_frame);
    REQUIRE(initial_raster && initial_raster->width == 64U &&
        initial_raster->height == 64U && !initial_raster->pixel_digest.is_zero());
    const auto initial_raster_digest = initial_raster->pixel_digest;
    const auto& absent_raster = cache.raster_for(nullptr);
    REQUIRE(!absent_raster);
    REQUIRE(cache.raster_for(&*initial_frame) &&
        cache.raster_for(&*initial_frame)->pixel_digest ==
            initial_raster_digest);
    const auto initial_vertices = initial->vertices;
    const auto& rotated = cache.projection_for(7U, first->source_revision, first,
        carto::studio::MeshThumbnailView{0, 0});
    REQUIRE(rotated && rotated->vertices.size() == initial_vertices.size());
    bool rotated_cache_changed = false;
    for (std::size_t index = 0U; index < initial_vertices.size(); ++index) {
        rotated_cache_changed = rotated_cache_changed ||
            rotated->vertices[index].x != initial_vertices[index].x ||
            rotated->vertices[index].y != initial_vertices[index].y;
    }
    REQUIRE(rotated_cache_changed);
    REQUIRE(cache.projection_for(7U, first->source_revision, first));

    auto replacement = compiled.value();
    replacement.positions.front().x += 0.25;
    auto current = std::make_shared<const carto::geometry::CompiledMesh>(replacement);
    REQUIRE(current->valid() && current->source_revision == first->source_revision);
    const auto& refreshed = cache.projection_for(7U, current->source_revision, current);
    REQUIRE(refreshed && refreshed->vertices.size() == initial_vertices.size());
    bool changed = false;
    for (std::size_t index = 0U; index < initial_vertices.size(); ++index) {
        changed = changed || refreshed->vertices[index].x != initial_vertices[index].x ||
            refreshed->vertices[index].y != initial_vertices[index].y;
    }
    REQUIRE(changed);

    auto aliased_source = std::make_shared<carto::geometry::CompiledMesh>(compiled.value());
    std::shared_ptr<const carto::geometry::CompiledMesh> aliased_view = aliased_source;
    carto::studio::MeshThumbnailCacheEntry aliased_cache;
    const auto& aliased_initial = aliased_cache.projection_for(
        7U, aliased_source->source_revision, aliased_view);
    REQUIRE(aliased_initial && !aliased_initial->vertices.empty());
    const auto aliased_initial_vertices = aliased_initial->vertices;
    aliased_source->positions.front().x += 0.5;
    const auto& aliased_refreshed = aliased_cache.projection_for(
        7U, aliased_source->source_revision, aliased_view);
    REQUIRE(aliased_refreshed && aliased_refreshed->vertices.size() ==
        aliased_initial_vertices.size());
    changed = false;
    for (std::size_t index = 0U; index < aliased_initial_vertices.size(); ++index) {
        changed = changed || aliased_refreshed->vertices[index].x !=
                aliased_initial_vertices[index].x ||
            aliased_refreshed->vertices[index].y != aliased_initial_vertices[index].y;
    }
    REQUIRE(changed);

    const carto::core::Revision mismatched_revision{
        aliased_source->source_revision.value() + 1U};
    REQUIRE(!aliased_cache.projection_for(7U, mismatched_revision, aliased_view));
    REQUIRE(aliased_cache.projection_for(
        7U, aliased_source->source_revision, aliased_view));

    aliased_source->indices.front() = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(!aliased_cache.projection_for(
        7U, aliased_source->source_revision, aliased_view));
    REQUIRE(!aliased_cache.projection_for(
        7U, aliased_source->source_revision, aliased_view));

    replacement.indices.front() = std::numeric_limits<std::uint32_t>::max();
    auto invalid = std::make_shared<const carto::geometry::CompiledMesh>(replacement);
    REQUIRE(!cache.projection_for(7U, invalid->source_revision, invalid));
    REQUIRE(!cache.projection_for(7U, invalid->source_revision, invalid));
    REQUIRE(cache.projection_for(8U, first->source_revision, first));
    const auto source_revision = first->source_revision;
    first.reset();
    REQUIRE(!cache.projection_for(8U, source_revision, {}));
}

} // namespace

int main() {
    try {
        hierarchy_and_filter_context();
        malformed_hierarchy_is_explicit_even_when_filtered();
        deep_hierarchy_is_bounded_and_iterative();
        real_assets_group_instances_and_preserve_counts();
        mesh_drop_sources_require_current_revision_bound_roots();
        source_registry_search_is_read_only_and_generation_bound();
        source_blob_integrity_status_is_snapshot_bound_and_explicit();
        mesh_thumbnail_projection_uses_bounded_compiled_geometry();
        mesh_thumbnail_depth_buffer_resolves_crossings_independent_of_triangle_order();
        thumbnail_cache_tracks_exact_compiled_source_without_stale_fallback();
        std::cout << "Desktop browser tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Desktop browser test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
