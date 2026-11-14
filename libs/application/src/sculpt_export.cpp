#include <carto/application/sculpt_export.hpp>
namespace carto::application {
core::Result<geometry::EditableMesh> materialize_sculpt_for_export(
    const geometry::EditableMesh& source, const production::SculptStack& stack,
    const SculptPreviewProvider& provider, SculptExportFramePolicy frames) {
    if (frames != SculptExportFramePolicy::require_current &&
        frames != SculptExportFramePolicy::geometry_only)
        return core::Result<geometry::EditableMesh>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument, "invalid sculpt export frame policy"));
    auto original = source.compile();
    if (!original) return core::Result<geometry::EditableMesh>::failure(original.error());
    auto evaluated = provider.evaluate(source, stack);
    if (!evaluated) return core::Result<geometry::EditableMesh>::failure(evaluated.error());
    const auto& product = evaluated.value();
    const auto& base = original.value();
    if (!product.valid() || product.vertex_ids != base.vertex_ids ||
        product.indices != base.indices || product.triangle_faces != base.triangle_faces ||
        product.triangle_edges != base.triangle_edges) {
        return core::Result<geometry::EditableMesh>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed, "evaluated sculpt changed source topology or identity"));
    }
    geometry::MeshPatch patch;
    patch.expected_revision = source.revision();
    for (std::size_t i=0; i<product.positions.size(); ++i) {
        const auto& before=base.positions[i]; const auto& after=product.positions[i];
        if (before.x!=after.x || before.y!=after.y || before.z!=after.z)
            patch.vertex_changes.push_back({base.vertex_ids[i],before,after});
    }
    // Authored corner frames describe the source surface. Preserving them
    // after deformation would silently export stale shading. Unchanged or
    // hidden sculpt remains safe, but changed surfaces require rebaking.
    if (!patch.vertex_changes.empty() && frames == SculptExportFramePolicy::require_current) {
        for (const auto& [id, layer] : source.attributes().layers()) {
            (void)layer;
            if (id == "cartographer.normal0" || id.starts_with("cartographer.tangent."))
                return core::Result<geometry::EditableMesh>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "sculpt export cannot preserve authored normals or tangents after deformation"));
        }
    }
    auto output=source;
    if (!patch.vertex_changes.empty()) {
        if (auto applied=output.apply_patch(patch); !applied)
            return core::Result<geometry::EditableMesh>::failure(applied.error().with_context("sculpt export geometry"));
    }
    auto check=output.compile();
    if (!check || check.value().vertex_ids != product.vertex_ids ||
        check.value().indices != product.indices || check.value().triangle_faces != product.triangle_faces ||
        check.value().triangle_edges != product.triangle_edges)
        return core::Result<geometry::EditableMesh>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed, "sculpt export cannot reproduce evaluated topology"));
    return core::Result<geometry::EditableMesh>::success(std::move(output));
}
}
