#include <carto/native_source/project_sweep_source.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace carto::native_source {
namespace {
assets::Sha256Digest digest_text(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}
} // namespace

core::Result<ProjectStraightSweep> derive_project_straight_sweep(
    const project::ProjectDocument& project, scene::ObjectId selected_object,
    std::string_view sweep_source_identity) {
    using Result = core::Result<ProjectStraightSweep>;
    if (sweep_source_identity.empty() ||
        sweep_source_identity.size() > geometry::SweepSource::kMaxIdentityBytes) {
        return Result::failure(invalid("sweep source identity is outside its bound"));
    }
    const project::ProjectDocument snapshot(project);
    auto selected = derive_project_mesh_source(snapshot, selected_object);
    if (!selected) return Result::failure(selected.error());
    const auto sweep = snapshot.sweep_sources().find(std::string(sweep_source_identity));
    if (sweep == snapshot.sweep_sources().end() ||
        sweep->second.owner_object_id == 0U ||
        sweep->second.owner_object_id != selected_object.value) {
        return Result::failure(invalid("sweep source does not name the selected object's frame"));
    }
    const auto path = snapshot.curve_paths().find(sweep->second.centerline_identity);
    if (path == snapshot.curve_paths().end()) {
        return Result::failure(invalid("sweep centerline is absent from the project"));
    }
    auto measure = geometry::measure_straight_sweep(sweep->second, path->second);
    if (!measure) return Result::failure(measure.error());
    auto generated = geometry::build_sweep_mesh(sweep->second, path->second);
    if (!generated) return Result::failure(generated.error());
    const auto generated_digest = project::canonical_mesh_snapshot_digest(generated.value().mesh);
    if (generated_digest != selected.value().editable_mesh_digest) {
        return Result::failure(invalid("selected editable-mesh snapshot differs from the regenerated sweep"));
    }
    ProjectStraightSweep result;
    result.selected_mesh = std::move(selected.value());
    result.source_measure = std::move(measure.value());
    result.sweep_source_bytes = sweep->second.serialize();
    result.centerline_source_bytes = path->second.serialize();
    result.sweep_source_digest = digest_text(result.sweep_source_bytes);
    result.centerline_source_digest = digest_text(result.centerline_source_bytes);
    result.generated_mesh_digest = generated_digest;
    const std::string operation_bytes = "carto.native.straight-sweep.operation/1\n" +
        result.sweep_source_digest.hex() + "\n" + result.centerline_source_digest.hex() + "\n" +
        result.selected_mesh.source_digest.hex() + "\n";
    result.operation_identity = digest_text(operation_bytes);
    return Result::success(std::move(result));
}

} // namespace carto::native_source
