#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/scene/scene.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace carto::render {

struct RenderInstance {
    scene::ObjectId object;
    std::shared_ptr<const geometry::CompiledMesh> mesh;
    core::Transform world_transform = core::Transform::identity();
    core::Revision source_revision;
};

class RenderScene {
public:
    [[nodiscard]] core::Result<void> upsert(RenderInstance instance);
    [[nodiscard]] core::Result<void> remove(scene::ObjectId object);
    [[nodiscard]] const RenderInstance* find(scene::ObjectId object) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return instances_.size(); }
    [[nodiscard]] std::vector<RenderInstance> instances() const;

private:
    std::map<scene::ObjectId, RenderInstance> instances_;
};

} // namespace carto::render
