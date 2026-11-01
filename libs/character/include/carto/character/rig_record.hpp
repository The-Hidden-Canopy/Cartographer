#pragma once

#include <carto/character/definition.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace carto::character {

// Canonical bounded encoding of every field of RigDefinition. Referenced
// kinematic graph and joint limits are separate content-addressed records.
inline constexpr std::size_t kMaxRigDefinitionBytes = 8U * 1024U * 1024U;
[[nodiscard]] core::Result<std::string> serialize_rig_definition(const RigDefinition& rig);
[[nodiscard]] core::Result<RigDefinition> deserialize_rig_definition(std::string_view text);
[[nodiscard]] core::Result<assets::Sha256Digest> rig_definition_digest(const RigDefinition& rig);

} // namespace carto::character
