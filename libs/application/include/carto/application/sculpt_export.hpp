#pragma once
#include <carto/application/sculpt_preview_provider.hpp>
namespace carto::application {
// Materialize an identity-preserving sculpt preview into a disposable mesh for
// explicit export. Source and stack remain unchanged; never persist this copy.
[[nodiscard]] core::Result<geometry::EditableMesh> materialize_sculpt_for_export(
    const geometry::EditableMesh& source, const production::SculptStack& stack,
    const SculptPreviewProvider& provider);
}
