#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/production/sculpt_stack.hpp>

#include <cstddef>
#include <string_view>

namespace carto::application {

// Evaluates an authored SculptStack for display. A trusted host installs the
// provider; its evaluator is not part of this repository. Implementations must
// not mutate the source mesh or the stack, must keep the source's vertex and
// face identities, and must fail instead of returning the unevaluated source.
// The result is disposable display geometry, never authoring truth.
class SculptPreviewProvider {
public:
    virtual ~SculptPreviewProvider() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual core::Result<geometry::CompiledMesh> evaluate(
        const geometry::EditableMesh& source,
        const production::SculptStack& stack) const = 0;
};

// Viewport-facing state of the optional evaluated preview. Without a provider
// the viewport always shows the authored source mesh.
struct SculptPreviewStatus {
    bool provider_installed = false;
    bool enabled = false;
    std::size_t evaluated_meshes = 0U;
    std::size_t refused_meshes = 0U;
};

} // namespace carto::application
